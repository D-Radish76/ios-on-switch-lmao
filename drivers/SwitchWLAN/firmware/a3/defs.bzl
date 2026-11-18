"""Build a Cortex-R4 bootstrap in its bounded persistent firmware island."""

def _payload_impl(ctx):
    elf = ctx.actions.declare_file(ctx.label.name + ".elf")
    args = ctx.actions.args()
    args.add_all([
        "-mcpu=cortex-r4",
        "-mthumb",
        "-mfloat-abi=soft",
        "-mno-unaligned-access",
        "-Os",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables",
        "-ffunction-sections",
        "-fdata-sections",
        "-nostdlib",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wl,--build-id=none,--gc-sections,--fatal-warnings,--no-warn-rwx-segments",
    ])
    args.add("-DDEVELOPMENT=" + ("1" if ctx.attr.development else "0"))
    if ctx.attr.lto:
        args.add("-flto")
    if ctx.attr.candidate:
        args.add("-DSWITCH_A3_CANDIDATE=1")
        args.add_all([
            "-fno-caller-saves",
            "-fno-partial-inlining",
            "-fno-schedule-insns2",
            "-fno-tree-loop-optimize",
            "-fno-optimize-sibling-calls",
            "-fno-ipa-bit-cp",
            "-fno-tree-vrp",
        ])
    for symbol in ctx.attr.retain_symbols:
        args.add("-Wl,--undefined=" + symbol)
    args.add("-T", ctx.file.link_script)
    args.add_all(ctx.files.srcs)
    args.add("-o", elf)
    ctx.actions.run(
        executable = ctx.executable._gcc,
        arguments = [args],
        inputs = ctx.files.srcs + ctx.files.hdrs + [ctx.file.link_script],
        tools = ctx.files._toolchain,
        outputs = [elf],
        mnemonic = "WlanA3Link",
        env = {"PATH": "/usr/bin:/bin"},
    )
    binary = ctx.actions.declare_file(ctx.label.name + ".bin")
    ctx.actions.run(
        executable = ctx.executable._objcopy,
        arguments = ["-O", "binary", elf.path, binary.path],
        inputs = [elf],
        tools = ctx.files._toolchain,
        outputs = [binary],
        mnemonic = "WlanA3Objcopy",
    )
    header = ctx.outputs.header
    ctx.actions.run(
        executable = ctx.executable._header,
        arguments = [elf.path, binary.path, header.path] + (["--candidate"] if ctx.attr.candidate else []),
        inputs = [elf, binary],
        outputs = [header],
        mnemonic = "WlanA3Header",
    )
    return [
        DefaultInfo(files = depset([header])),
        OutputGroupInfo(elf = depset([elf]), binary = depset([binary])),
    ]

switch_wlan_a3_payload = rule(
    implementation = _payload_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".c", ".S"], mandatory = True),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "link_script": attr.label(allow_single_file = True, mandatory = True),
        "header": attr.output(mandatory = True),
        "development": attr.bool(default = False),
        "retain_symbols": attr.string_list(),
        "lto": attr.bool(default = False),
        "candidate": attr.bool(default = False),
        "_gcc": attr.label(default = "@arm_none_eabi//:bin/arm-none-eabi-gcc", executable = True, allow_single_file = True, cfg = "exec"),
        "_objcopy": attr.label(default = "@arm_none_eabi//:bin/arm-none-eabi-objcopy", executable = True, allow_single_file = True, cfg = "exec"),
        "_toolchain": attr.label(default = "@arm_none_eabi//:all"),
        "_header": attr.label(default = ":payload_header", executable = True, cfg = "exec"),
    },
)
