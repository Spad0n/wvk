-- HLSL -> SPIR-V for wvk, through dxc.
--
--   target("game")
--       add_deps("wvk")
--       add_rules("hlsl2spv")
--       add_files("shaders/lit.hlsl", {stages = {vertex = "vertex_main", fragment = "pixel_main"}})
--
-- One output per stage: <targetdir>/<outputdir>/<basename>.<vert|frag|comp|mesh|task>.spv
-- The wvk source root is added to the include path automatically when the target depends on wvk,
-- so shaders write #include "shaders/wvk.hlsl".
--
-- Target values (all optional):
--   hlsl2spv.includedirs    extra -I directories; relative paths are taken from the target's xmake.lua
--   hlsl2spv.outputdir      subdirectory of the target directory for the .spv files (default: none)
--   hlsl2spv.shader_model   default "6_6", the minimum for ResourceDescriptorHeap
--   hlsl2spv.flags          extra dxc flags
--
-- dxc lookup: $DXC, then $VULKAN_SDK/bin, then PATH. The descriptor-heap flags need a recent dxc,
-- such as the one shipped with the Vulkan SDK; the one in the Windows SDK is usually too old.

rule("hlsl2spv")
    set_extensions(".hlsl")

    on_config(function (target)
        import("lib.detect.find_program")

        local dxc = os.getenv("DXC")
        if not dxc or not os.isfile(dxc) then
            local paths = {}
            for _, envname in ipairs({"VULKAN_SDK", "VK_SDK_PATH"}) do
                local sdk = os.getenv(envname)
                if sdk then
                    table.insert(paths, path.join(sdk, "bin"))
                    table.insert(paths, path.join(sdk, "Bin"))
                end
            end
            dxc = find_program("dxc", {paths = paths})
        end
        target:data_set("hlsl2spv.dxc", dxc)

        local includedirs = {}
        for _, dir in ipairs(table.wrap(target:values("hlsl2spv.includedirs"))) do
            table.insert(includedirs, path.absolute(dir, target:scriptdir()))
        end
        -- The wvk source root, wherever the including project put it.
        local wvkdep = target:dep("wvk")
        if wvkdep then
            table.insert(includedirs, wvkdep:scriptdir())
        end
        target:data_set("hlsl2spv.includedirs", includedirs)
    end)

    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        local dxc = target:data("hlsl2spv.dxc")
        if not dxc then
            raise("hlsl2spv: dxc not found. Install the Vulkan SDK, or set DXC to the dxc executable.")
        end

        local fileconfig = target:fileconfig(sourcefile) or {}
        local stages = fileconfig.stages
        if not stages then
            raise("hlsl2spv: %s has no stages, e.g. add_files(\"%s\", {stages = {vertex = \"vertex_main\"}})",
                  sourcefile, sourcefile)
        end

        local kinds = {
            vertex   = {profile = "vs", suffix = "vert"},
            fragment = {profile = "ps", suffix = "frag"},
            pixel    = {profile = "ps", suffix = "frag"},
            compute  = {profile = "cs", suffix = "comp"},
            mesh     = {profile = "ms", suffix = "mesh"},
            task     = {profile = "as", suffix = "task"},
        }

        local shader_model = target:values("hlsl2spv.shader_model") or "6_6"
        local outputdir = target:targetdir()
        if target:values("hlsl2spv.outputdir") then
            outputdir = path.join(outputdir, target:values("hlsl2spv.outputdir"))
        end
        local includedirs = target:data("hlsl2spv.includedirs") or {}

        local common = {
            "-spirv",
            "-fspv-target-env=vulkan1.3",
            "-fspv-entrypoint-name=main",
            "-fvk-use-scalar-layout",
            "-fvk-bind-resource-heap", "0", "0",
            "-fvk-bind-sampler-heap", "1", "0",
        }
        if is_mode("debug") then
            table.insert(common, "-Od")
            table.insert(common, "-Zi")
        else
            table.insert(common, "-O3")
        end
        for _, dir in ipairs(includedirs) do
            table.insert(common, "-I")
            table.insert(common, dir)
        end
        table.join2(common, table.wrap(target:values("hlsl2spv.flags")))

        local basename = path.basename(sourcefile)
        local outputs = {}
        batchcmds:mkdir(outputdir)
        -- Sorted so the command order, and therefore the build log, is stable.
        for _, stage in ipairs(table.orderkeys(stages)) do
            local kind = kinds[stage]
            if not kind then
                raise("hlsl2spv: unknown stage '%s' for %s", stage, sourcefile)
            end
            local outputfile = path.join(outputdir, basename .. "." .. kind.suffix .. ".spv")
            batchcmds:show_progress(opt.progress, "${color.build.object}compiling.hlsl %s (%s)", sourcefile, stage)
            batchcmds:vrunv(dxc, table.join(common, {
                "-T", kind.profile .. "_" .. shader_model,
                "-E", stages[stage],
                "-Fo", outputfile,
                sourcefile,
            }))
            table.insert(outputs, outputfile)
        end

        -- Rebuild when the source or wvk.hlsl changes. Headers the shader includes itself are not
        -- tracked; list them with add_files(..., {depfiles = {...}}) if needed.
        batchcmds:add_depfiles(sourcefile)
        for _, dir in ipairs(includedirs) do
            local header = path.join(dir, "shaders", "wvk.hlsl")
            if os.isfile(header) then
                batchcmds:add_depfiles(header)
            end
        end
        batchcmds:add_depfiles(table.wrap(fileconfig.depfiles))
        local oldest
        for _, outputfile in ipairs(outputs) do
            local mtime = os.mtime(outputfile)
            if not oldest or mtime < oldest then
                oldest = mtime
            end
        end
        batchcmds:set_depmtime(oldest or 0)
        batchcmds:set_depcache(target:dependfile(outputs[1]))
    end)
rule_end()
