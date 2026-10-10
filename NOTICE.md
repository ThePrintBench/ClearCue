# Licensing notice: ClearCue firmware (`env:clearcue`)

The product firmware links **wolfSSL**, which is licensed under the **GNU GPL v3**.
The firmware binary built from `env:clearcue` (and every `web/firmware/*.bin` produced
from it) is therefore distributed under the GPL v3 as a whole. The project's own code
must stay under a GPLv3-compatible licence.

## Third-party components in the product image

| Component | Version | Licence | How it is obtained |
|---|---|---|---|
| wolfSSL | v5.9.4-stable, commit `3c5eead44904df64e6a5a1f4ebdce377d35a849a` | GPL-3.0 | Fetched by `tools/wolfssl_config.py`, pinned and verified by commit |
| Arduino core for ESP32 | 3.3.7 (pioarduino 55.03.37) | LGPL-2.1+ | PlatformIO platform package |
| ESP-IDF libraries | 5.5 (as shipped with the core) | Apache-2.0, plus Espressif binary blobs for the Wi-Fi/PHY | PlatformIO platform package |
| Mbed TLS (part of ESP-IDF, used for OTA) | 3.6.5 | Apache-2.0 | PlatformIO platform package |
| ArduinoJson | 7.x | MIT | `lib_deps` |
| ESP Web Tools (flasher page only, not in firmware) | 10.4.0 | Apache-2.0 | unpkg CDN |

## Corresponding source

For each released firmware version: this repository at the release tag, plus
wolfSSL at the commit above. `pio run -e clearcue` rebuilds the exact image.
Tag every release and publish the tag before shipping units with it.

## Installation information (GPLv3 §6, "User Products")

Owners can install modified firmware: the board accepts unsigned images over USB
(`pio run -e clearcue -t upload`, the web flasher, or `esptool write-flash 0x0`),
and through the settings page's firmware upload. Do not enable Secure Boot,
flash encryption, or OTA signature enforcement on shipped units unless owners are
given the means (keys) to install their own builds.
