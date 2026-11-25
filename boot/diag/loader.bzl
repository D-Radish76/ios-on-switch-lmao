"""//boot:diag_loader, the D0 loader diagnostic: its own loader, bl33.bin and hekate entry, built apart from the normal
loader. It compiles with the normal loader's flags (Xcode's clang, as boot/defs.bzl does), links with its own script, and
the link fails unless the map and the disassembly pass tools/package/diag_loader_inspect.py.

    clang -c, one per source ... objects
    diag_loader.py link ........ loader.elf, loader.map, loader.dis, inspection.txt, loader.uboot.bin
    diag_loader.py build ....... package/ (bootloader/ini/ios-diag-loader.ini, switchroot/ios-diag-loader/*)
"""

load("//boot:defs.bzl", "LOADER_COPTS")

_XCODE_SHELL = 'DEVELOPER_DIR="$(/usr/bin/sed -n "s/^DEVELOPER_DIR=//p" "$1")"; export DEVELOPER_DIR; shift; exec "$@"'

def _impl(ctx):
    includes = []
    for marker in ctx.files.include_markers:
        includes += ["-I", marker.dirname]
    objects = []
    for i, src in enumerate(ctx.files.srcs):
        obj = ctx.actions.declare_file("%s/part-%d.o" % (ctx.label.name, i))
        ctx.actions.run_shell(
            command = _XCODE_SHELL,
            arguments = [ctx.file.xcode_env.path, "/usr/bin/clang"] + LOADER_COPTS + ctx.attr.defines + includes + ["-c", src.path, "-o", obj.path],
            inputs = depset([src, ctx.file.xcode_env, ctx.file.xcode_identity] + ctx.files.hdrs),
            outputs = [obj],
            env = {"LC_ALL": "C", "PATH": "/usr/bin:/bin", "TZ": "UTC"},
            mnemonic = "DiagLoaderCompile",
            progress_message = "Compiling the D0 loader's %s" % src.short_path,
            execution_requirements = {"requires-darwin": ""},
        )
        objects.append(obj)
    name = ctx.label.name
    elf = ctx.actions.declare_file(name + "/loader.elf")
    linkmap = ctx.actions.declare_file(name + "/loader.map")
    dis = ctx.actions.declare_file(name + "/loader.dis")
    report = ctx.actions.declare_file(name + "/inspection.txt")
    loader = ctx.actions.declare_file(name + "/loader.uboot.bin")
    args = ctx.actions.args()
    args.add("link")
    args.add("--ld-lld", ctx.file._ld_lld)
    args.add("--objcopy", ctx.file._objcopy)
    args.add("--objdump", ctx.file._objdump)
    args.add("--linker-script", ctx.file.linker_script)
    args.add("--map", linkmap)
    args.add("--disassembly", dis)
    args.add("--report", report)
    args.add("--elf", elf)
    args.add("--out", loader)
    args.add_all(objects)
    ctx.actions.run(
        executable = ctx.executable._diag_loader,
        arguments = [args],
        inputs = objects + [ctx.file.linker_script],
        tools = [ctx.file._ld_lld, ctx.file._objcopy, ctx.file._objdump],
        outputs = [elf, linkmap, dis, report, loader],
        mnemonic = "DiagLoaderLink",
        progress_message = "Linking and inspecting the D0 loader",
    )
    package = ctx.actions.declare_directory(name + "/package")
    build = ctx.actions.args()
    build.add("build")
    build.add("--template", ctx.file.script_template)
    build.add("--boards", ctx.file.boards)
    build.add("--board", ctx.attr.board)
    build.add("--loader", loader)
    build.add("--dtb", ctx.file.dtb)
    build.add("--bl31", ctx.file.bl31)
    build.add("--bl33", ctx.file.bl33)
    build.add("--dwell", str(ctx.attr.dwell))
    build.add("--entry", ctx.attr.entry)
    build.add("--title", ctx.attr.title)
    build.add("--ident", ctx.attr.ident)
    build.add("--out", package.path)
    ctx.actions.run(
        executable = ctx.executable._diag_loader,
        arguments = [build],
        inputs = [loader, ctx.file.script_template, ctx.file.boards, ctx.file.dtb, ctx.file.bl31, ctx.file.bl33],
        outputs = [package],
        mnemonic = "DiagLoaderPackage",
        progress_message = "Composing the D0 loader's bl33.bin and hekate entry",
    )
    return [DefaultInfo(files = depset([package, loader, elf, linkmap, dis, report]))]

diag_loader_package = rule(
    implementation = _impl,
    doc = "The D0 loader diagnostic's SD card files (package/) and what was inspected (loader.map, loader.dis, inspection.txt).",
    attrs = {
        "srcs": attr.label_list(allow_files = [".c", ".S"], mandatory = True, doc = "in link order"),
        "hdrs": attr.label_list(allow_files = True),
        "include_markers": attr.label_list(allow_files = True, doc = "one file per -I directory, in order"),
        "linker_script": attr.label(mandatory = True, allow_single_file = True),
        "script_template": attr.label(mandatory = True, allow_single_file = True),
        "boards": attr.label(default = "//kits/SwitchBoard:SwitchBoard.h", allow_single_file = True),
        "board": attr.string(mandatory = True, doc = "the board row whose console the script accepts"),
        "dtb": attr.label(mandatory = True, allow_single_file = True),
        "bl31": attr.label(mandatory = True, allow_single_file = True),
        "bl33": attr.label(mandatory = True, allow_single_file = True),
        "dwell": attr.int(default = 15),
        "defines": attr.string_list(doc = "extra compile flags for every source (D0-R's -DSWITCH_DIAG_RESERVATIONS=1)"),
        "entry": attr.string(default = "ios-diag-loader", doc = "the directory under switchroot/ and the ini's file name"),
        "title": attr.string(default = "iOS Diag Loader", doc = "the hekate menu entry"),
        "ident": attr.string(default = "iDiagL", doc = "the entry's id in the ini"),
        "xcode_env": attr.label(mandatory = True, allow_single_file = True),
        "xcode_identity": attr.label(mandatory = True, allow_single_file = True),
        "_diag_loader": attr.label(default = "//tools/package:diag_loader", executable = True, cfg = "exec"),
        "_ld_lld": attr.label(default = "@llvm//:bin/ld.lld", cfg = "exec", allow_single_file = True),
        "_objcopy": attr.label(default = "@llvm//:bin/llvm-objcopy", cfg = "exec", allow_single_file = True),
        "_objdump": attr.label(default = "@llvm//:bin/llvm-objdump", cfg = "exec", allow_single_file = True),
    },
)
