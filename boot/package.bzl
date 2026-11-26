"""The attributes //boot:package and //boot:erista_package share: everything but the board's ADT, DTB, cores and
output directory. BUILD files cannot unpack a dictionary into a call, so the entries live in a macro."""

load(":defs.bzl", "boot_package")
load(":overlay.bzl", "BOOTSTRAP_PACKAGES", "OVERLAY_BINARIES")

# --//install:prebuilt=release: the GitHub release's kernel (prelinked here), overlay Mach-Os, GPU trust cache, lz4 and
# loader, with the release's --//boot:smp.
def _shared():
    return {
        "app_trust": "//ipsw:app_cryptex_trust",
        "bl31": "//bazel/pinned:firmware/bl31.bin",
        "bl33": "//bazel/pinned:firmware/bl33.bin",
        "bootlogo": "//boot/logo:bootlogo.bmp",
        "bootstrap_manifest": select({
            "//install:from_release": "@switch_release//:bootstrap_manifest",
            "//conditions:default": "//bootstrap:manifest",
        }),
        "bootstrap_payload": select({
            "//install:from_release": "@switch_release//:bootstrap_payload",
            "//conditions:default": "//bootstrap:payload",
        }),
        "bt_patch": "//bazel/pinned:firmware/BCM4356A3.hcd",
        "data_seed": "//bazel/pinned:data-seed/data-v10.dbr",
        "exporter": "//bazel/pinned:firmware/exporter.uimg",
        "icon": "//boot/logo:icon_ios_hue.bmp",
        "ini": ":ios_ini",
        "installer_entitlements": select({
            "//ipsw:21H461": "//apps/installer:SwitchInstaller.entitlements",
            "//ipsw:24B119": "//apps/installer:SwitchInstaller27.entitlements",
        }),
        "kernel": select({
            "//install:from_release": "//install:release_kernel",
            "//conditions:default": "//kernel:prelinked",
        }),
        "launchd_catalog_os": select({
            "//ipsw:21H461": "@ipsw//:os",
            "//conditions:default": None,
        }),
        "linker_script": ":loader/link.ld",
        "loader_hdrs": [
            ":loader/boot_args.h",
            ":loader/carveout.h",
            ":handoff/display.h",
            ":handoff/entry.S",
            ":handoff/record.h",
            ":loader/copy.h",
            ":loader/emc.h",
            ":loader/memory.h",
            ":loader/ram.h",
            ":loader/seed.h",
            ":loader/stage.h",
            ":loader/stamps.h",
            ":storage/sd_reference.h",
            "//kits/SwitchBoard:SwitchBoard.h",
        ],
        "loader_include_markers": [
            "//kits/SwitchBoard:SwitchBoard.h",
            ":loader/stage.h",
            ":handoff/record.h",
            ":storage/sd_reference.h",
        ],
        # The cacheable-copy root-boot loader, in link order.
        "loader_srcs": [
            ":loader/entry.S",
            ":loader/loader.c",
            ":loader/memory.c",
            ":handoff/probe.c",
            ":loader/copy.c",
            ":loader/copy.S",
            ":loader/emc.c",
            ":loader/seed.c",
            ":loader/boot_args.c",
            ":storage/sd_reference.c",
        ],
        # The sources that include the window's layout.h or runs.h: only these recompile when the kernel changes.
        "loader_window_srcs": [
            ":loader/entry.S",
            ":loader/loader.c",
            ":loader/copy.c",
            ":loader/copy.S",
        ],
        "lz4": select({
            "//install:from_release": "@switch_release//:lz4",
            "//conditions:default": "//tools:lz4",
        }),
        # The overlay's Mach-Os (the trust module hashes those the manifest trusts) and its other files, each by the
        # source tree/overlay/manifest.json names it by.
        "overlay_binaries": select({
            "//install:from_release": {"@switch_release//:overlay/" + source: source for source in OVERLAY_BINARIES.values()},
            "//conditions:default": OVERLAY_BINARIES,
        }) | {
            "//drivers/SwitchInstallPolicy:installd": "build/switch-apps/installd",
        } | select({
            ":local_debug": {
                "//drivers/TegraGM20BMetal:fetchform": "build/switch-gpu/fetchform",
                "//apps/hwtest:switch-texpeer": "build/switch-hwtest/switch-texpeer",
            },
            "//conditions:default": {},
        }) | select({
            ":local_debug_21H461": {"//apps/hwtest:SwitchHWTest17": "build/switch-hwtest/SwitchHWTest.ipa"},
            ":local_debug_24B119": {
                "//apps/hwtest:SwitchHWTest27": "build/switch-hwtest/SwitchHWTest.ipa",
                "//apps/hwtest:SwitchHWTest27e": "build/switch-hwtest/SwitchHWTest27e.ipa",
            },
            "//conditions:default": {},
        }),
        "overlay_data": {
            "//boot/logo:apple-logo~ipad.png": "build/progressui/apple-logo~ipad.png",
        } | select({
            "//install:from_release": {
                "@switch_release//:bootstrap/packages/" + package + ".deb": source
                for package, source in BOOTSTRAP_PACKAGES.items()
            },
            "//conditions:default": {
                "//bootstrap:managed_" + package: source
                for package, source in BOOTSTRAP_PACKAGES.items()
            },
        }) | select({
            "//ipsw:21H461": {
                "//bazel/pinned:overlay/wallpaper-1x/21H461/7570.iPadOS_17_Flat.plist": "build/wallpaper-1x/21H461/7570.iPadOS_17_Flat.plist",
                "//bazel/pinned:overlay/wallpaper-1x/21H461/7560.Lake_Flat.plist": "build/wallpaper-1x/21H461/7560.Lake_Flat.plist",
            },
            "//ipsw:24B119": {
                "//bazel/pinned:overlay/wallpaper-1x/24B119/7570.iPadOS_17.plist": "build/wallpaper-1x/24B119/7570.iPadOS_17.plist",
                "//bazel/pinned:overlay/wallpaper-1x/24B119/7560.Lake.plist": "build/wallpaper-1x/24B119/7560.Lake.plist",
            },
        }),
        "overlay_files": ["//overlay:overlay_sources"],
        "overlay_manifest": "//overlay:manifest.json",
        "portrait_header": "//drivers/NativeMobileDisplay:SwitchPortrait.h",
        "prebuilt_loader": select({
            "//install:from_release": "@switch_release//:loader",
            "//conditions:default": None,
        }),
        "prebuilt_loader_constants": select({
            "//install:from_release": "@switch_release//:loader_constants",
            "//conditions:default": None,
        }),
        "script_template": ":u-boot-script.txt",
        "static_trust": "//ipsw:static_trust",
        "system_trust": "//ipsw:system_cryptex_trust",
        "xcode_env": select({
            "//install:from_release": None,
            "//conditions:default": "@xcode//:xcode.env",
        }),
        "xcode_identity": select({
            "//install:from_release": None,
            "//conditions:default": "@xcode//:identity",
        }),
    }

def boot_entry(name, adt, dtb, smp, **own):
    """boot_package with the shared attributes; `own` are the board's other attributes (out, tags, loader_defines)."""
    boot_package(name = name, adt = adt, dtb = dtb, smp = smp, **(_shared() | own))
