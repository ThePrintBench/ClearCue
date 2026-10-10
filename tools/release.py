# PlatformIO extra script for [env:clearcue] (runs as "pre:").
#
# Pre-build:  turns custom_product_name / custom_ota_base_url from platformio.ini
#             into PRODUCT_NAME / DEFAULT_OTA_URL defines.
# Post-build: writes the release artifacts into web/ :
#   web/firmware/<slug>-<ver>-factory.bin  merged bootloader+partitions+boot_app0+app at 0x0.
#                                          Production flashing only: it overwrites NVS (settings).
#   web/firmware/<slug>-<ver>-app.bin      app-only image (OTA, "upload .bin", and the web flasher)
#   web/firmware/<slug>-<ver>-{bootloader,partitions,boot_app0}.bin
#                                          separate pieces for the web flasher, which writes them at
#                                          their own offsets and so leaves NVS (settings) alone
# Versioned names make every release a new URL, so no cache can serve a stale image
# under a manifest that advertises a newer version. Older versions are left in place
# (handy for rollback); prune web/firmware/ yourself when you want.
#   web/firmware/ota.json            {"version","url"} manifest polled by the device
#   web/manifest.json                ESP Web Tools manifest
# Version comes from src/<firmware dir>/version.h (FW_VERSION); the directory is taken
# from build_src_filter, so renaming src/clearcue only needs platformio.ini updated.

Import("env")  # noqa: F821
import json
import os
import re
import shutil
import subprocess

proj = env.subst("$PROJECT_DIR")
product = env.GetProjectOption("custom_product_name", "ClearCue")
# Environment variables override platformio.ini (used by CI: repository variables).
ota_base = (os.environ.get("CLEARCUE_OTA_BASE_URL") or env.GetProjectOption("custom_ota_base_url", "")).rstrip("/")
source_url = os.environ.get("CLEARCUE_SOURCE_URL") or env.GetProjectOption("custom_source_url", "")
slug = re.sub(r"[^a-z0-9-]", "", product.lower()) or "button"

_flt = env.GetProjectOption("build_src_filter", "")
if isinstance(_flt, (list, tuple)):
    _flt = " ".join(_flt)
_m = re.search(r"\+<([^/>*]+)/", _flt)
src_dir = os.path.join(proj, "src", _m.group(1) if _m else "clearcue")
with open(os.path.join(src_dir, "version.h"), encoding="utf-8") as f:
    version = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read()).group(1)
ver_tag = re.sub(r"[^0-9A-Za-z.+-]", "_", version)   # filename-safe

ota_manifest_url = f"{ota_base}/firmware/ota.json" if ota_base else ""
env.Append(CPPDEFINES=[
    ("PRODUCT_NAME", env.StringifyMacro(product)),
    ("DEFAULT_OTA_URL", env.StringifyMacro(ota_manifest_url)),
    ("SOURCE_URL", env.StringifyMacro(source_url)),
])


def make_release(source, target, env):
    build = env.subst("$BUILD_DIR")
    web = os.path.join(proj, "web")
    fw_dir = os.path.join(web, "firmware")
    os.makedirs(fw_dir, exist_ok=True)

    app = os.path.join(build, env.subst("${PROGNAME}.bin"))
    factory_name = f"{slug}-{ver_tag}-factory.bin"
    app_name = f"{slug}-{ver_tag}-app.bin"
    factory = os.path.join(fw_dir, factory_name)
    app_out = os.path.join(fw_dir, app_name)

    # (offset, file) for every piece of a full install: bootloader, partition table,
    # boot_app0 (OTA data) and the app.
    pieces = [(env.subst(o), env.subst(i)) for o, i in env.get("FLASH_EXTRA_IMAGES", [])]
    pieces.append((env.subst("$ESP32_APP_OFFSET"), app))

    # 1) Factory image: everything merged from 0x0. esptool pads the gaps with 0xFF,
    #    which covers NVS (0x9000), so writing it ERASES SETTINGS. Production only.
    esptool = env.subst("$UPLOADER").strip('"')
    cmd = [
        esptool, "--chip", env.BoardConfig().get("build.mcu", "esp32c3"),
        "merge-bin", "-o", factory,
        "--flash-mode", env.subst("${__get_board_flash_mode(__env__)}"),
        "--flash-freq", env.subst("${__get_board_f_image(__env__)}"),
        "--flash-size", env.BoardConfig().get("upload.flash_size", "4MB"),
    ]
    for offset, image in pieces:
        cmd += [offset, image]
    print("[release] " + " ".join(cmd))
    subprocess.check_call(cmd)
    shutil.copyfile(app, app_out)

    # 2) Web flasher: the same pieces as separate parts at their own offsets, so the
    #    NVS range is never written and settings survive a reinstall. The flasher's
    #    "erase device" option (chip erase) is the only way it wipes settings.
    #    Each piece is sliced out of the factory image rather than copied from the build
    #    dir, because merge-bin patches the bootloader header (flash mode/freq/size) and
    #    the web flasher writes bytes as-is.
    with open(factory, "rb") as f:
        merged = f.read()
    web_parts = []
    for offset, image in pieces:
        off = int(offset, 0)
        if image == app:
            name = app_name
        else:
            stem = os.path.splitext(os.path.basename(image))[0]
            name = f"{slug}-{ver_tag}-{stem}.bin"
            with open(os.path.join(fw_dir, name), "wb") as f:
                f.write(merged[off:off + os.path.getsize(image)])
        web_parts.append({"path": f"firmware/{name}", "offset": off})

    with open(os.path.join(web, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump({
            "name": product,
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [{"chipFamily": "ESP32-C3", "parts": web_parts}],
        }, f, indent=2)

    with open(os.path.join(fw_dir, "ota.json"), "w", encoding="utf-8") as f:
        json.dump({
            "version": version,
            "url": f"{ota_base}/firmware/{app_name}" if ota_base else f"firmware/{app_name}",
        }, f, indent=2)

    print(f"[release] {product} {version}: {factory}")
    if not ota_base:
        print("[release] custom_ota_base_url is empty: devices will have no default update server")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", make_release)
