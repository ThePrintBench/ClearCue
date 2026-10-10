# ClearCue

A physical **Set Ready / Clear Plate** button for 3D-printer farms running **Watchtower** or **Bambuddy**. One button per printer: the light shows whether the printer is ready, and a press tells the server the bed has been cleared.

I used the same button and 3D-printed enclosure as [Bambutton](https://github.com/EdwardChamberlain/Bambutton), but I was already building on the same board when Bambutton was released. The wiring and the code are different. The enclosure model is on [MakerWorld (Bambutton)](https://makerworld.com/en/models/2747607-bambutton-on-machine-bambuddy-plate-tracking).

## What it does

- **One firmware for both servers.** Pick Watchtower or Bambuddy on the settings page; nothing is compiled in per printer.
- **No-code setup.** A phone joins the button's setup Wi-Fi to connect it to your network; the rest is filled in from a browser on a computer.
- **HTTPS that works with Watchtower's own certificate.** Watchtower's local CA uses Name Constraints, which the ESP32's built-in TLS library can't parse, so ClearCue uses wolfSSL for server traffic. One click ("Trust this server") pins your server's certificate so the button verifies it.
- **Updates** over the network from your own update server, by uploading a file on the settings page, or over USB from a web page.
- **Diagnostics.** "Test connection" explains failures in plain English, and detailed serial logging can be switched on from the settings page.

## Hardware

| Part | Notes |
|---|---|
| ESP32-C3 SuperMini | Native USB, no driver needed. I got mine from [Amazon](https://www.amazon.com/dp/B0DFWG87JS). |
| 16 mm illuminated momentary button, **tri-colour (RGB) LED, 3–6 V**, normally open | e.g. [AliExpress](https://www.aliexpress.us/item/3256804846894412.html). Pick the tri-colour (RGB), 3–6 V, 16 mm option. The 3–6 V rating means the resistors are built in. |
| USB-C cable + any USB power supply | It draws very little; PD is not needed. |

### Wiring

LEDs are common-anode: the common pin goes to 3V3 and each colour is switched by the ESP32 pulling its pin low.

```text
Button pin          ESP32-C3 SuperMini
NO     (PIN1)  ->   GND
NO     (PIN2)  ->   GPIO10
Blue   (PIN3)  ->   GPIO5
Green  (PIN4)  ->   GPIO3
Red    (PIN5)  ->   GPIO4
Common (PIN6)  ->   3V3
```

Check your button's datasheet: on tri-colour buttons the blue LED is usually PIN3, shifting the others right compared with the bi-colour version, but sellers vary.

**Blue at 3.3 V:** blue LEDs need about 3.0–3.2 V, so with the common on 3V3 the blue can be dim. Test it before building a batch (power the common from 3V3 and touch the blue pin to GND). If it's too dim, run the common from 5V and switch each colour through a logic-level N-MOSFET (2N7002 / AO3400), then set `LED_ACTIVE_LOW = false` in `src/clearcue/main.cpp`. Never connect 5V to a GPIO directly.

## Installing the firmware

Pick one:

- **Web flasher** (easiest): open the hosted flasher page (`web/index.html`) in Chrome or Edge on a computer, plug the board in, click **Connect**. It can also send your Wi-Fi details straight after flashing.
- **PlatformIO**: `pio run -t upload` (see *Building* below).
- **Production / many boards**: `tools/batch_flash.ps1` flashes each board you plug in and logs its MAC address and setup-network name to a CSV.

If the board doesn't show up over USB, hold **BOOT** while plugging it in.

## Setting it up

1. Power the button. When it isn't set up, it glows **solid blue** and creates a Wi-Fi network called `ClearCue-XXXX`.
2. On your phone, join `ClearCue-XXXX`. The setup page opens on its own (or go to `http://192.168.4.1`).
3. Pick your 2.4 GHz Wi-Fi network, enter the password, tap **Connect**. The phone then shows the button's address, e.g. `http://clearcue-1a2b.local` and its IP.
4. On a computer on the same network, open that address and fill in:
   - **Server type**: Watchtower (default) or Bambuddy
   - **API base URL**: Watchtower ends in `/api/public/v1`, Bambuddy in `/api/v1`
   - **API key**: Watchtower keys start with `wt_` and need the `control` scope plus `set_ready`; Bambuddy keys need printer read plus clear-plate
   - **Printer**: Watchtower uses the printer *name* (exact, case-sensitive); Bambuddy uses the numeric printer *ID*
5. Press **Save**, then **Test connection**.

No computer? Tap "Set up everything on this phone instead" on the phone's setup page.

For HTTPS: open **HTTPS certificate check → Trust this server**, compare the fingerprint with your server's CA, and confirm. Without a trusted certificate the button still encrypts, but doesn't check who it's talking to.

## Using it

**Lights**

| Light | Meaning |
|---|---|
| Solid green | Printer is ready, nothing to do |
| Solid red | Printer busy, or the plate needs clearing |
| Solid blue | Setup network is on (first setup, or Wi-Fi lost for over 5 minutes) |
| Slow blue blink | On Wi-Fi, but server details still need filling in |
| Fast red/green alternating | Connecting to Wi-Fi |
| Slow red blink | Can't reach the server: open the settings page and use **Test connection** |
| White flicker | Button held over 3 s: release to cancel, keep holding to 10 s to factory reset |

**Button**

- **Short press**: Watchtower sends `set_ready` (only when the printer is online, not printing and not already ready); Bambuddy clears the plate (only when Bambuddy is waiting for one). Three green blinks = accepted, two green blinks = nothing to do, three red blinks = failed.
- **Hold 10 seconds**: factory reset. Erases all settings and returns to setup mode. The settings page has the same thing under **Device actions**.

**Settings page**: `http://<device-name>.local` on your network. Optional settings password (user `admin`). The footer shows firmware version, device name and MAC address.

**Updates**: **Firmware → Check for update** (needs an update server, see below), or upload a `clearcue-<version>-app.bin` file on the same page.

**Troubleshooting over USB**: tick **Device → Serial debug logging**, Save, and open the serial port at 115200 baud (PuTTY, or `pio device monitor`). Every server check, server reply, and button press is logged with what it decided and why.

## Building

Requirements: VS Code with the **pioarduino** extension (not upstream PlatformIO), and **Git** on your PATH (the build fetches wolfSSL).

```bash
pio run                 # build -> web/manifest.json + web/firmware/clearcue-<ver>-{factory,app}.bin
pio run -t upload       # flash over USB
pio device monitor      # serial log (enable debug logging on the settings page first)
```

The first build clones wolfSSL into `.pio/deps/` (pinned to an exact commit) and takes a few minutes.

Configuration lives in `platformio.ini` under `[env:clearcue]`:

| Option | Purpose |
|---|---|
| `custom_product_name` | Name used for the setup network, hostnames, pages and file names |
| `custom_ota_base_url` | HTTPS address where you host the `web/` folder; compiled in as the update server |
| `custom_source_url` | Link to this repository, shown on the settings page with the licence notice |

The firmware version is in `src/clearcue/version.h`. Bump it for every release.

### Releasing

1. Bump `FW_VERSION`, run `pio run`.
2. Upload the `web/` folder to the `custom_ota_base_url` host: the `.bin` files first, `firmware/ota.json` last. The update server must use a Let's Encrypt certificate (the update client only trusts ISRG Root X1/X2).
3. Tag the release in git; for GPL compliance, the tag is the corresponding source.

### GitHub Actions

`.github/workflows/build.yml` builds the firmware and runs the TLS host test on every push and pull request; the build output is downloadable from the run page. Pushing a tag `v<FW_VERSION>` (e.g. `git tag v2.4.3 && git push origin v2.4.3`) also checks the tag matches `version.h` and publishes a GitHub Release with the factory and app images, a zip of the web flasher folder, and the exact wolfSSL source the binary was built from.

Optional repository variables (*Settings → Secrets and variables → Actions → Variables*): `CLEARCUE_OTA_BASE_URL` and `CLEARCUE_SOURCE_URL` override the matching `custom_*` options (the source URL defaults to the repository), and `DEPLOY_PAGES=true` publishes `web/` to GitHub Pages on each release. Pages needs *Settings → Pages → Source: GitHub Actions*, and its `github-pages` environment must allow `v*` tags. For OTA from Pages, use a custom domain: those certificates come from Let's Encrypt.

### Tests

`test/tls_host/run.sh` builds the pinned wolfSSL with the firmware's exact settings on Linux and runs the TLS policy against a Watchtower-style test CA (Name Constraints, RSA-4096) served by `openssl s_server`: 17 cases covering trust, host/IP matching, name-constraint violations, expiry, and trust-on-first-use.

```bash
test/tls_host/run.sh .pio/deps/wolfssl-v5.9.4-stable [path/to/your-watchtower-ca.pem]
```

Without your CA the generated replica stands in for the anchor-load case (that's what CI does).

## Licence

The firmware links wolfSSL (GPLv3), so firmware binaries built from this repository are distributed under the GPLv3. See [NOTICE.md](NOTICE.md) for third-party components, corresponding-source and installation-information details.
