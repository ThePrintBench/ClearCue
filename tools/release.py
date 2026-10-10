# PlatformIO extra script for [env:clearcue] (runs as "pre:").
#
# Pre-build:  turns custom_product_name / custom_ota_base_url from platformio.ini
#             into PRODUCT_NAME / DEFAULT_OTA_URL defines.
# Post-build: writes the release artifacts into web/ :
#   web/firmware/<slug>-<ver>-factory.bin  merged bootloader+partitions+boot_app0+app at 0x0
#                                          (ESP Web Tools + production flashing)
#   web/firmware/<slug>-<ver>-app.bin      app-only image (OTA + "upload .bin" on the settings page)
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

    parts = []
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
        parts += [env.subst(offset), env.subst(image)]
    parts += [env.subst("$ESP32_APP_OFFSET"), app]

    esptool = env.subst("$UPLOADER").strip('"')
    cmd = [
        esptool, "--chip", env.BoardConfig().get("build.mcu", "esp32c3"),
        "merge-bin", "-o", factory,
        "--flash-mode", env.subst("${__get_board_flash_mode(__env__)}"),
        "--flash-freq", env.subst("${__get_board_f_image(__env__)}"),
        "--flash-size", env.BoardConfig().get("upload.flash_size", "4MB"),
    ] + parts
    print("[release] " + " ".join(cmd))
    subprocess.check_call(cmd)
    shutil.copyfile(app, app_out)

    with open(os.path.join(web, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump({
            "name": product,
            "version": version,
            "new_install_prompt_erase": True,
            "builds": [{
                "chipFamily": "ESP32-C3",
                "parts": [{"path": f"firmware/{factory_name}", "offset": 0}],
            }],
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
