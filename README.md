# scheff-echolink

An EchoLink client for the **LilyGO T-Display-P4** (ESP32-P4), **LilyGO
T-Deck Plus** (ESP32-S3) and **Seeed Wio Tracker L2** (ESP32-S3). It uses
each board's onboard display, microphone, speaker and Wi-Fi hardware so you
can connect to EchoLink stations, links, repeaters and conferences without a
PC.

**Status: early and not yet tested on hardware.** All three targets compile
with PlatformIO's ESP-IDF v5.5, but none has been verified on a device yet.
The protocol pieces were checked on the host (see
[Verification](#verification)).

## What works (in code)

- Login to the EchoLink directory (TCP 5200). It re-registers every 5 min and
  reports ONLINE when idle and BUSY while in a QSO.
- Station list download, with lookup by callsign, node number or IP.
- Outgoing and incoming QSOs over UDP 5198/5199:
  - RTCP SDES keepalives and BYE
  - GSM 06.10 audio: 4 frames (80 ms) per packet
  - info and chat text
- Full audio path at 8 kHz mono: mic → GSM encode → UDP, and UDP → GSM
  decode → 240 ms jitter buffer → speaker.
- Half-duplex PTT: hold **BOOT** on T-Display-P4, hold **Space** on T-Deck
  Plus, hold the **user button** on Wio Tracker L2, or use the `ptt` console
  command.
- Serial console for setup and operation. Settings are saved in NVS.
- Mic → speaker loopback mode for testing the audio hardware.

## Hardware notes (T-Display-P4 V1.0)

| Function | Details |
| --- | --- |
| Codec | ES8311 @ I2C 0x18 on SDA 20 / SCL 21; I2S0 MCLK 13, BCLK 12, WS 9, DOUT 10, DIN 11 |
| Audio power | XL9535 expander (I2C 0x20 on SDA 7 / SCL 8) IO6. This turns on the NS4150B amp and the ES8311 analog rail |
| Wi-Fi | ESP32-C6 over SDIO slot 1 (CLK 18, CMD 19, D0–D3 14–17). C6 EN is XL9535 IO14 |
| PTT | BOOT = GPIO35, active low. This pin is also Ethernet RMII TXD1, so it can't be used as PTT if Ethernet is ever enabled |

Pins are taken from LilyGO's
`lilygo_device_driver/src/device/t_display_p4/t_display_p4_config.h`.

Wi-Fi uses **ESP-Hosted** through LilyGO's fork, which is a submodule in
`third_party/esp_hosted`. The fork adds a reset callback, which is needed
because the C6 EN line is on the I/O expander. The C6 must run the matching
ESP-Hosted slave firmware. The LilygoBox factory image already uses it. If
you've flashed something else onto the C6, reflash the network-adapter image
from the [LilygoBox releases](https://github.com/Xinyuan-LilyGO/lilygobox-espidf/releases/latest),
or run `scripts/flash-c6.sh <port> [image.bin]` over the C6's UART connector.
If you don't give an image, the script uses the ESP-Hosted v2.12 C6 image that
ships with PlatformIO's Arduino-ESP32.
The T-Display-P4 README explains the procedure.

## Hardware notes (T-Deck Plus)

| Function | Details |
| --- | --- |
| Display | ST7789 320x240 landscape LCD with GT911 touch |
| Audio | ES7210 microphone ADC on I2S1; I2S speaker on I2S0 |
| Wi-Fi | Native ESP32-S3 Wi-Fi |
| PTT | Hold the keyboard Space bar; raw keyboard matrix mode provides press and release state |
| Storage | microSD over the display's shared SPI2 bus |

The Plus uses the original T-Deck peripheral pin map; its integrated GPS does
not change the display, keyboard, audio, SD or battery connections used here.

## Hardware notes (Wio Tracker L2)

| Function | Details |
| --- | --- |
| Display | NV3031B 240x320 LCD over QSPI (SPI3: SCLK 42, IO0–IO3 41/40/39/38, CS 46), driven as 320x240 landscape. LP5814 backlight driver at I2C 0x2C. GT911 touch at 0x5D, polled |
| I/O expander | PCA9555 at 0x21 on I2C SDA 47 / SCL 48. It switches LCD power and reset, touch reset, the audio amp (IO12), SD power (IO14) and GNSS power (kept off) |
| Audio | ES8311 DAC at 0x18 and ES7243E microphone ADC at 0x14 on one I2S port: MCLK 10, BCLK 11, WS 12, DOUT 16, DIN 15 |
| Wi-Fi | Native ESP32-S3 Wi-Fi |
| PTT | User button = GPIO0 (also the BOOT strap), active low |
| Storage | microSD over 1-bit SDMMC: CLK 2, CMD 3, D0 1 |
| Battery | ADS1115 at 0x48, AIN0 through a 1:2 divider |

Pins come from Seeed's Meshtastic variant and camillia-mt. The panel init
sequence follows LovyanGFX's `Panel_NV3031B`. The SX1262 LoRa radio and the
GNSS module aren't used: the radio's CS (GPIO21) is held high and GNSS power
stays off. Display colours, touch mapping and the microphone path haven't been
checked on a device yet.

## Build

```bash
git clone --recursive <this repo>    # or: git submodule update --init --recursive
. ~/esp/esp-idf/export.sh             # ESP-IDF v5.5.4+
idf.py set-target esp32p4
idf.py menuconfig                     # optional: EchoLink Client -> defaults
idf.py build flash monitor
```

Or build with PlatformIO, the way camillia-mt does. PlatformIO downloads and
manages ESP-IDF itself, so you don't need a separate install:

```bash
scripts/build-upload-monitor.sh                 # build + flash + monitor
scripts/build-upload-monitor.sh --tdeck-plus    # select T-Deck Plus
scripts/build-upload-monitor.sh --wio-tracker-l2  # select Wio Tracker L2
scripts/build-upload-monitor.sh --tdisplay-p4   # explicitly select T-Display-P4
scripts/build-upload-monitor.sh --port /dev/cu.usbmodem1101
scripts/build-upload-monitor.sh --just-build    # compile only, no board needed
scripts/build-upload-monitor.sh --fullclean     # fullclean first
scripts/build-upload-monitor.sh --erase         # erase flash first (wipes settings)
```

Notes:

- All targets use the script's separate PlatformIO core
  (`~/.platformio-p4`). This stops other projects' `espressif32` versions from
  replacing the pioarduino platform.
- ESP-IDF needs space-free build and framework paths. The script builds in
  `~/.platformio-p4/build/scheff-echolink`; `ECHOLINK_BUILD_DIR` overrides it.
  If the core directory points to a volume with spaces, use a real space-free
  core/package installation, not just a symlink to the framework.
- `sdkconfig.pio.defaults` selects the board's ECO2 (pre-v3) P4 silicon.

On an aarch64 Linux host, Espressif doesn't ship a ninja binary. Run
`pip install ninja` inside the IDF Python env.

If boot fails with a chip-revision error, your P4 is older than v3.x. Enable
`ESP32P4_SELECTS_REV_LESS_V3` in menuconfig.

## Touch UI

The LVGL 9 UI uses the T-Display-P4's 568x1232 RM69A10/GT9895 display, the
T-Deck Plus's 320x240 ST7789/GT911 display, or the Wio Tracker L2's 320x240
NV3031B/GT911 display. The two 320x240 targets use compact fonts and
controls, and scroll workflows that do not fit in one viewport.

- **Wi-Fi Setup** appears on first boot, or after an erase when no network is
  saved. You can:
  - tap **Scan for networks** and pick one from the list, or
  - type the network name yourself.

  Enter the password (it's shown, not masked; open networks need none) and
  tap **Join**. A status box shows *Connecting...* then *Connected!*, and a
  second later the next step opens.
- **EchoLink Account** comes next in onboarding. Enter your callsign and
  EchoLink password (masked); name and location are optional.
- **Home** is where the device boots once onboarding is done. Your callsign is
  at the top, with an **Edit** button under it that changes the account.
  Saving logs in to the directory again straight away.
  **Wi-Fi** reopens network setup even while offline; **Back** returns without
  saving changes. A successful join saves the network and restarts the device.
  - **Search** by callsign, description or node number, or pick a letter
    (A-Z, 0-9) to list the known nodes starting with it. **`*`** lists your
    favorites; Home opens on it when you have some. Lists show 20 per page.
  - **Tap** a node to connect to it.
  - Tap a node's **gear** (on the right) for a menu: **Edit** it, or
    **Favorite** / **Unfavorite** it. Favorites show in gold.
  - **Create Manual** adds a node by hand. You enter the callsign, node
    number, a description, and optionally a host or IP address to connect to
    directly.
- **PTT:** hold the P4's **BOOT** button, the T-Deck Plus **Space** bar or
  the Wio Tracker L2 **user button** anywhere. In Chat it lights the talk
  button like the on-screen one does.
- **Chat** opens once connected:
  - Incoming and console-initiated calls also open Chat from Home. Active
    settings forms are left open until you finish or return.
  - It shows the station and the info text it sends when you connect.
  - The large button is **green** when you can talk; hold it to transmit.
  - It turns **red** while someone else is talking.
  - **Leave** disconnects and returns to Home.

### Web config

You can do the same onboarding from a phone or computer.

- **Until Wi-Fi connects**, the device runs an open setup hotspot (no password)
  named `Scheff-EchoLink-P4-XXXX`, `Scheff-EchoLink-TDeck-XXXX` or
  `Scheff-EchoLink-WioL2-XXXX`. Join it and phones open the setup page by
  themselves; otherwise browse to `http://192.168.4.1`. Because it's open,
  the Wi-Fi and EchoLink passwords you enter there cross the air
  unencrypted.
- **What the page does:** it scans for or accepts a Wi-Fi network, and saves
  the EchoLink account. After the initial Wi-Fi connection succeeds, the
  device saves the network and restarts before continuing account setup.
- **After station Wi-Fi gets an IP address**, the hotspot stays up for another
  minute so the page can show the device's new address, then turns off. The page stays
  available at `http://<device IP>` on your network, and Home shows that
  address.

The page also manages servers:

- **Servers:** search, or pick a letter (`*` for favorites) to list the saved
  servers. ☆/★ toggles a favorite. You can add, edit or
  delete one; changes go to `echolink-servers.xml`, and the device's list
  updates straight away.
- **Backup:**
  - **Download complete backup** saves `scheff-echolink-backup.xml` to your
    computer. It includes every persisted setting and the working server list.
  - **Import a backup** restores those settings and replaces the server list.
    Legacy `echolink-servers.xml` files remain importable and leave settings
    unchanged.
  - **Export to SD card** and **Import from SD card** use
    `scheff-echolink-backup.xml` at the root of a FAT-formatted microSD card.
    Import also reads `echolink-p4-backup.xml`, the name from before the
    rename.

  The complete backup includes Wi-Fi and EchoLink passwords in readable form,
  so store it securely. Favorites are saved in the file (`favorite="true"`),
  so they travel with exports and imports. An import replaces the working
  list; the default list is never touched. Complete backups may contain an
  empty server list; legacy files with no `<server>` entries are rejected.
  Imports validate the complete XML before applying changes. Failed server-list
  writes retain the active list, and failed SD exports retain the previous backup.

The page never displays saved passwords, but complete backup exports contain
them. Leaving the EchoLink password blank keeps the saved one. The page has no
login, so anyone on your network can reach it.

### Server lists

Known nodes (conferences, links and repeaters) are stored as XML on a 4 MB
FAT partition in internal flash, mounted at `/data`:

| File | Purpose |
| --- | --- |
| `echolink-default-servers.xml` | Snapshot taken from the first EchoLink directory download. It's written once and never edited, and serves as the backup. |
| `echolink-servers.xml` | Working copy, created from the default. Home reads it; manual entries and edits are saved to it. |

Each node is an element like
`<server call="*ECHOTEST*" node="9999" type="conference" desc="..." ip="..." host=""/>`.
`ip` is the address last seen in the directory. EchoLink addresses change, so
connecting still looks the station up in the live directory, unless the entry
has a fixed `host`.

## Serial console

Everything above can also be set from the serial console:

```
echolink> wifi MySSID MyPassword
echolink> set callsign N0CALL
echolink> set password ********
echolink> set name Mike
echolink> set location Anytown, ST
echolink> login
echolink> status
```

Your callsign must already be
[validated by EchoLink](https://secure.echolink.org/validation/). The
directory only sends the station list to a station that is logged in from
that IP.

## Use

```
echolink> list ECHOTEST            # search the directory
echolink> connect *ECHOTEST*       # or: c 9999, or: c 1.2.3.4
  (hold BOOT on T-Display-P4, Space on T-Deck Plus or the user button on
   Wio Tracker L2 to talk; ECHOTEST plays it back)
echolink> chat hello               # text chat
echolink> dc                       # disconnect
echolink> set volume 60
echolink> set mic_gain 30          # dB of PGA gain
echolink> loopback on              # local mic -> speaker test
```

### Network / NAT

EchoLink needs UDP **5198–5199** to reach the device. Outgoing calls usually
work through a typical home NAT, because the device sends from those same
ports. Incoming calls, and some routers that rewrite source ports, need UDP
5198–5199 forwarded to the board. TCP 5200 outbound is needed for the
directory.

Incoming connections are only accepted when:

- the device is idle,
- `accept_incoming` is on, and
- the caller's IP is in the downloaded directory list. The official client
  applies the same rule.

## Layout

```
main/
  app_main.c       startup sequence
  board.c/.h       I2C buses, XL9535 expander, power rails
  audio.c/.h       ES8311 via esp_codec_dev, playback jitter buffer, mic task
  net_wifi.c/.h    ESP-Hosted bring-up, STA connect, SNTP
  el_directory.*   directory login + station list (TCP 5200)
  el_qso.*         QSO state machine, RTCP, GSM audio, chat (UDP 5198/5199)
  ptt.c/.h         Board button/keyboard hold-to-talk
  console_cmds.*   serial REPL
  settings.*       NVS-backed configuration
  display.*        RM69A10 MIPI-DSI panel + GT9895 touch, LVGL port
  lv_mem_psram.c   LVGL allocations in PSRAM
  server_list.*    known-node XML lists on the /data FAT partition
  xml.*            validated EchoLink XML parsing via Expat
  file_store.*     recoverable file replacement and complete text reads
  ui*.c            LVGL screens: Wi-Fi setup, account, Home, Chat
  onboard.*        onboarding state shared by the screen and the web config
  web_config.c     setup hotspot, captive DNS and HTTP API; page in web/index.html
  sdcard.*         microSD (SDMMC slot 0, 4-bit) at /sdcard
  *_tdeck.c        T-Deck Plus board, display, audio, keyboard PTT and SD
  *_wio_l2.c       Wio Tracker L2 board, display, audio and SD
components/gsm     libgsm 1.0.22 (TU Berlin), unmodified, see COPYRIGHT
third_party/esp_hosted  LilyGO ESP-Hosted fork (submodule)
```

## Verification

Run the checked-in host regressions with:

```bash
python3 -m unittest discover -s tests -v
```

They require Python 3, a native C compiler with AddressSanitizer/UBSan, and
Expat headers/library. They compile production logic with hardware, RTOS and
LVGL test doubles; no board, network service or real flasher is used.

The protocol code follows SvxLink's `echolib`. The following were checked on
the host:

- **RTCP:** the SDES and BYE builders produce byte-identical output to
  SvxLink's `rtp_make_sdes` / `rtp_make_bye`.
- **GSM:** the vendored codec, built with the firmware's flags, round-trips a
  440 Hz tone using 33-byte frames.
- **Directory list:** the parser was tested against a real reply from
  `servers.echolink.org` (the "NOT LOGGED IN" message set) plus sample station
  entries.

Not verified yet: anything on hardware. That includes codec levels, I2S at
8 kHz, ESP-Hosted bring-up, and a real QSO.

## Next steps

- Ethernet (IP101) as an alternative to Wi-Fi
- Receive jitter handling that uses RTP sequence numbers (reorder/loss concealment)
- Optional VOX, courtesy tone, AGC on the mic path
- Speex codec support (SvxLink's PRIV "SPEEX" extension) for SvxLink peers
