# echolink-p4

An EchoLink client for the **LilyGO T-Display-P4** (ESP32-P4). It uses the
board's own ES8311 codec, electret mic and speaker, so you can connect to
EchoLink stations, links, repeaters and conferences without a PC.

**Status: early and not yet tested on hardware.** It builds cleanly with
ESP-IDF v5.5.4. The protocol pieces were checked on the host (see
[Verification](#verification)). It has not run on a board yet.

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
- Half-duplex PTT: hold the **BOOT** button, or use the `ptt` console command.
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
from the [LilygoBox releases](https://github.com/Xinyuan-LilyGO/lilygobox-espidf/releases/latest).
The T-Display-P4 README explains the procedure.

## Build

```bash
git clone --recursive <this repo>    # or: git submodule update --init --recursive
. ~/esp/esp-idf/export.sh             # ESP-IDF v5.5.4+
idf.py set-target esp32p4
idf.py menuconfig                     # optional: EchoLink Client -> defaults
idf.py build flash monitor
```

On an aarch64 Linux host, Espressif doesn't ship a ninja binary. Run
`pip install ninja` inside the IDF Python env.

If boot fails with a chip-revision error, your P4 is older than v3.x. Enable
`ESP32P4_SELECTS_REV_LESS_V3` in menuconfig.

## First-time setup (serial console)

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
  (hold BOOT to talk; ECHOTEST plays it back)
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
  ptt.c/.h         BOOT button hold-to-talk
  console_cmds.*   serial REPL
  settings.*       NVS-backed configuration
components/gsm     libgsm 1.0.22 (TU Berlin), unmodified, see COPYRIGHT
third_party/esp_hosted  LilyGO ESP-Hosted fork (submodule)
```

## Verification

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

- LVGL UI on the MIPI-DSI panel (station list, touch PTT, RX/TX indicator)
- Ethernet (IP101) as an alternative to Wi-Fi
- Receive jitter handling that uses RTP sequence numbers (reorder/loss concealment)
- Optional VOX, courtesy tone, AGC on the mic path
- Speex codec support (SvxLink's PRIV "SPEEX" extension) for SvxLink peers
