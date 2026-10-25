#pragma once
// Single source of truth for the firmware version. tools/release.py reads this
// to stamp web/manifest.json and web/firmware/ota.json. Bump on every release.
#define FW_VERSION "2.5.0"
