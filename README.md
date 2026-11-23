# ELRS Netpack — TCP → WiFi → ELRS Backpack Bridge

English | [日本語](README.ja.md)

> [!IMPORTANT]
> This fork builds a **TCP → WiFi → ESP-NOW** backpack communication system using **two ESP32 boards**, replacing the original single-board Ethernet design.
> It is **not** compatible with the original Waveshare ESP32-S3 Ethernet board.

> [!NOTE]
> This project is **not** officially affiliated with or supported by the ExpressLRS organisation.

---

## Overview

RotorHazard sends MSP packets over TCP. This project bridges those packets all the way to an ELRS VRx backpack over ESP-NOW, using two ESP32 boards connected by UART:

```
RotorHazard (Raspberry Pi)
        │  TCP port 8080
        ▼
┌─────────────────────┐
│  XIAO ESP32-S3      │  elrs-xiao-webui/
│  WiFi STA + Web UI  │  Receives TCP, forwards via UART
│  TCP server + mDNS  │
└────────┬────────────┘
         │  UART 115200 baud
         │  D0(GPIO1) TX ──► GPIO26 RX
         │  D1(GPIO2) RX ◄── GPIO27 TX
         │  GND        ◄──►  GND
         ▼
┌─────────────────────┐
│  ESP32 Wrover-E     │  elrs-espnow-bridge/
│  ESP-NOW only       │  Sends/receives ESP-NOW packets
└────────┬────────────┘
         │  ESP-NOW (2.4 GHz, ch 1)
         ▼
   ELRS VRx Backpack (FPV Goggles)
```

**Why two boards?**
The ESP32-S3 has one shared 2.4 GHz radio. Running WiFi STA and ESP-NOW simultaneously on the same chip causes packet loss and peer-list corruption — a hardware limitation. Splitting the roles across two boards connected by UART solves this completely.

---

## Hardware

| Part | Notes |
|---|---|
| Seeed Studio XIAO ESP32-S3 | External-antenna (Sense) model recommended |
| ESP32 Wrover-E | Or any 4 MB+ ESP32 dev board |
| 3 jumper wires | TX / RX / GND |
| USB-C cable | Data-capable, for flashing XIAO |
| USB-A/micro cable | For flashing ESP32 |
| Raspberry Pi | Running RotorHazard |

---

## Wiring

| XIAO ESP32-S3 | ESP32 Wrover-E |
|---|---|
| D0 / GPIO1 (TX) | GPIO26 (RX) |
| D1 / GPIO2 (RX) | GPIO27 (TX) |
| GND | GND |

> Do **not** connect 3V3 / 5V between the boards — power each board separately via USB.

---

## Prerequisites

1. **VS Code** — [code.visualstudio.com](https://code.visualstudio.com/)
2. **PlatformIO IDE extension** — install from the VS Code Extensions tab (`Ctrl+Shift+X` → search "PlatformIO IDE")
3. Clone this repository:
   ```powershell
   git clone https://github.com/yanazoo/elrs-netpack
   cd elrs-netpack
   ```

---

## Configuration

The XIAO firmware (`elrs-xiao-webui/`) needs **no source-file editing**. WiFi credentials are entered through the captive portal on first boot — see [Flash XIAO ESP32-S3](#flash-xiao-esp32-s3-elrs-xiao-webui) and [Default AP Credentials](#default-ap-credentials-elrs-xiao-webui) below.

The ESP-NOW side (`elrs-espnow-bridge/`) also needs no configuration; its channel is fixed at compile time via `ESPNOW_CHANNEL` (default 1) in `platformio.ini`.

---

## Flash ESP32 Wrover-E (`elrs-espnow-bridge/`)

1. Open the folder in VS Code:
   **File → Open Folder → `elrs-espnow-bridge/`**
2. Connect ESP32 via USB
3. PlatformIO sidebar → **`esp32dev` → General → Upload**

**Expected serial output:**
```
[boot] ESP-NOW bridge (UART RX=GPIO26 TX=GPIO27)
[wifi] channel fixed to 1
[espnow] (re)initialized
[boot] ready
```

### Using a XIAO ESP32-C3 instead

A Seeed Studio XIAO ESP32-C3 works in place of the ESP32 Wrover-E (PlatformIO env **`xiao_esp32c3`**):
D7 / GPIO20 (RX) ← host TX, D6 / GPIO21 (TX) → host RX, GND ↔ GND.

```
pio run -d elrs-espnow-bridge -e xiao_esp32c3 -t upload
```

The boot log (USB serial) reads `[boot] ESP-NOW bridge (UART RX=GPIO20 TX=GPIO21)`.

### Using a XIAO ESP32-C5 instead (v2.2.1)

A Seeed Studio XIAO ESP32-C5 works too (PlatformIO env **`xiao_esp32c5`**, pioarduino), wired on the same D7 / D6 as the XIAO C3:
D7 / GPIO12 (RX) ← host TX, D6 / GPIO11 (TX) → host RX, GND ↔ GND. The C5 can do 5 GHz; the bridge stays on 2.4 GHz channel 1.

```
pio run -d elrs-espnow-bridge -e xiao_esp32c5 -t upload
```

The boot log (USB serial) reads `[boot] ESP-NOW bridge (UART RX=GPIO12 TX=GPIO11)`.

---

## elrs-xiao-webui — XIAO Firmware (Web UI Edition)

`elrs-xiao-webui/` is the XIAO firmware. It provides the full TCP MSP bridge plus a **Web UI, captive portal, battery voltage monitor, buzzer, and LED notifications**.

---

### Additional Hardware (elrs-xiao-webui)

| Part | Notes |
|---|---|
| Passive buzzer | Connected across GPIO4(+) and GPIO6(−) |
| 7-color auto-cycling LED | GPIO9 (D10) + **100 Ω resistor** in series |
| 2× 100 kΩ resistor | Voltage divider for LiPo monitoring (optional) |
| LiPo battery | 1S recommended (shared power with ESP32 Wrover-E) |

---

### Additional Wiring (elrs-xiao-webui)

| XIAO GPIO | Pin label | Connected to |
|---|---|---|
| GPIO3 | D2 / A0 | Voltage divider midpoint (LiPo+ → R1 100kΩ → GPIO3 → R2 100kΩ → GND) |
| GPIO4 | D3 | Buzzer positive (+) |
| GPIO6 | D5 | Buzzer negative (−) |
| GPIO9 | D10 | LED anode (+ 100 Ω → GND) |
| GPIO21 | — | Built-in LED (active-LOW, onboard) |

> **Important:** Always use a **100 Ω** series resistor with the notification LED to protect GPIO9.

---

### Ways to write the firmware (from v2.2.0)

| Way | For | How |
|---|---|---|
| **Web flasher** (latest) | netpack XIAO, ELRS Backpack bridge (pick XIAO C3 / XIAO C5 / Wrover-E) | Open [https://yanazoo.github.io/elrs-netpack/](https://yanazoo.github.io/elrs-netpack/) in Chrome / Edge on a PC and press the device's "書き込む" |
| **Web flasher** (your own files) | same | Drop .bin files (a test build, an older release) or a release zip on "ファイルを指定して書き込む". Addresses come from the file names (editable); a firmware.bin gets the otadata that starts the first app slot |
| **Online** (a button) | netpack XIAO | netpack settings → "更新" (Update) → "🔍 最新版を確認" → "⬇️ … に更新". Needs the internet (a router in client mode, or USB tethering in host mode) |
| **A file** | netpack XIAO | netpack settings → "更新" → pick a firmware.bin (`elrs-netpack-xiao_esp32s3-firmware.bin` or the one in the release zip) |
| **PlatformIO** (from the source on GitHub) | all | below |

- A release carries the zips (bootloader, partitions, firmware), one-file `*-full.bin` images (written at `0x0`, for esptool-js and others), and `elrs-netpack-xiao_esp32s3-firmware.bin` and `elrs-netpack-version.txt` for the online update.
- Settings (WiFi and so on) are kept every way (unless you tick erase).
- GitHub Actions builds the web flasher at each release and puts it on GitHub Pages (Settings → Pages → Source must be **GitHub Actions**).

### Flash XIAO ESP32-S3 (`elrs-xiao-webui/`)

1. Open the folder in VS Code:
   **File → Open Folder → `elrs-xiao-webui/`**
2. Wait for PlatformIO to install packages (first time only)
3. Connect XIAO via USB-C
4. PlatformIO sidebar → **`xiao_esp32s3` → General → Upload**

If a specific COM port is needed, add to `platformio.ini`:
```ini
upload_port = COM3   ; change to your port number
```

If flashing fails, enter bootloader mode:
```
① Hold BOOT button
② Press and release RST
③ Release BOOT
```

**Expected serial output (first boot — WiFi not configured):**
```
[boot] XIAO ESP32-S3 WiFi bridge + Web UI
[wifi] not configured — starting captive portal immediately
[ap] IP=192.168.4.1
[web] HTTP server on port 80
[boot] ready
```

---

### Features (elrs-xiao-webui)

| Feature | Description |
|---|---|
| TCP MSP bridge | Full TCP ⇄ UART MSP bridge to the ESP-NOW board |
| Web UI | Dark-theme settings page at `http://elrs-netpack.local` |
| Language toggle | JP / EN switchable from any page |
| RSSI display | WiFi signal strength shown in real time |
| Captive portal | AP mode on first boot or failed WiFi; browser opens automatically |
| mDNS | Accessible as `elrs-netpack.local` on local network |
| WiFi settings | SSID / password saved to NVS (flash) |
| Voltage monitor | LiPo voltage via ADC with configurable divider ratio |
| Voltage alarm | Buzzer + LED alert when voltage drops below threshold |
| Buzzer | Double-beep on WiFi connect; short beep on settings save |
| Notification LED | 7-color auto-cycling LED with PWM brightness control |
| Fast reconnect | Non-blocking auto-reconnect; retries every 10 s, falls back to captive portal after 60 s |
| Host mode (v2.2.0) | The netpack runs its own WiFi; the timer and phones join it (below) |
| USB tethering (v2.2.0) | In host mode, an Android phone on the USB port takes the WiFi to the internet (on / off in the settings) |
| Update page (v2.2.0) | Write a firmware.bin from the browser (`/update`) |
| Max TX power | 21 dBm (both WiFi and ESP-NOW sides) |
| Backpack version | Reports firmware version 10.1 to RotorHazard |

### How the ELRS Backpack bridge sends (elrs-espnow-bridge)

Connectionless, like UDP: the bridge takes the pilot's bind-phrase UID as its own MAC (goggle backpacks only accept OSD packets from that UID) and sends to the ESP-NOW **broadcast** address, without waiting for an ACK and without retries. Each burst for one pilot (all its OSD packets up to the next `SET_SEND_UID`, or 20 ms of quiet) is sent **3 times in a row** (`SEND_REPEATS`).

- Goggles that are switched off cost nothing (nothing waits for them), so up to 8 pilots with their goggles off don't delay the others.
- Two goggles bound to the same phrase both show the OSD (no ACK collision).
- One lost packet is covered by the next copy; longer dropouts by the host's own timed repeats (RotorHazard's vrxc_elrs, or the lap timer).
- Copies of one packet are at least 3 ms apart: ELRS 2.4 GHz transmitters (one per pilot, right next to the goggles) hop to a new frequency every few ms and about a third of their hops land in ESP-NOW's channel 1, so a later copy meets a different hop.
- **Listen filter** (custom MSP `0x0F01`, payload = a 6-byte UID, empty to clear): the host (the FPV C5 Lap Timer) gives the race director's UID, and the bridge then passes on only frames *from* that UID (the RD's TX switch). Other bridges' broadcast OSD and pilots' own DVR switches are no longer forwarded. The bridge answers with `0x0F01` payload `1`. Hosts that never send it (RotorHazard / vrxc_elrs) get everything, as before. While the filter is set, the bridge also sniffs the race director's switch in promiscuous mode (v2.12): the TX backpack sends it once per flip to the RD's UID, and during the OSD bursts the bridge's MAC is a pilot's UID, so plain ESP-NOW reception missed it. A frame heard both ways (or retried by the sender) within 500 ms goes to the host once.
- **Link watch** (custom MSP `0x0F03`, payload `1` on / `0` off, answered with `0x0F03`): the host (the FPV C5 Lap Timer) asks the bridge to listen to channel 1 in promiscuous mode for pilots' TX backpacks with *Backpack > Telemetry = ESPNOW* (ELRS 3.4+). For each UID it hears link statistics from, the bridge reports `0x0F04` (payload: UID 6 bytes, uplink LQ, age in 1/10 s) at once when the radio links or unlinks and then once a second. The watch ends by itself 20 s after the host's last request; hosts that never ask (RotorHazard / vrxc_elrs) see nothing new.
- **VTX report** (custom MSP `0x0F05`, while the link watch is on): when a pilot's TX sends its VTX settings (Lua *VTX Administrator*; the TX sends them when it links, and the backpack passes them on to the goggles at the same UID as `MSP_SET_VTX_CONFIG`), the bridge reports `0x0F05` (payload: UID 6 bytes, then the first two bytes of the VTX config: band/channel index `(band-1)*8+channel`, or a frequency in MHz).
- Frames heard over ESP-NOW are queued and written to the UART from `loop()`, never from the WiFi task.
- Per-packet logging: build with `-DBRIDGE_VERBOSE`.

> **WiFi channel:** ESP-NOW runs on **channel 1** (the ELRS VRx backpack's fixed channel). Put the router the netpack XIAO joins on **channel 6–11** (not 1–4), so its WiFi doesn't share the air with the OSD.

---

### Host mode and USB tethering (v2.2.0)

A setup built around the netpack. Its XIAO runs a WiFi of its own (host mode), and the timer and the phones join it. The timer's MSP goes on as before: UART → ELRS Backpack bridge → the goggles' backpack (the bridge is a separate board; its ESP-NOW stays on channel 1).

```
 phones (viewers, admin) ──┐
 FPV C5 Lap Timer ─────────┼── WiFi "ELRS-Netpack" ── netpack XIAO ──UART── ELRS Backpack bridge ── goggles
 (client mode)             ┘     (ch 6-11)              │USB
                                                        └── Android phone (USB tethering) ── internet
```

**Host mode** (WiFi settings page)

1. Under "接続方式" (connection) pick **Host**, enter the host SSID, password (8-63 characters) and channel (6-11), and save. The netpack restarts with that WiFi (defaults: SSID `ELRS-Netpack`, password `elrs-netpack`, ch 6).
2. The settings page is at `http://192.168.4.1/` (or `http://elrs-netpack.local/`).
3. Put the timer in **client mode** with this SSID and password. Leave its netpack address at `auto`: it finds the netpack as `elrs-netpack.local`.
4. Phones join the same WiFi; "**タイマーを開く**" (open the timer) on the netpack's page links to the timer (the address its MSP comes from).
5. Up to **10 devices**, the timer included. The page lists the devices on the WiFi with their signal.
6. To go back, pick "Client" and save.

**USB tethering** (host mode only)

With an Android phone on the XIAO's USB-C and its "USB tethering" on, the devices on the netpack's WiFi (timer, phones) reach the internet through the phone's mobile data (the timer's update check and online update, for instance).

- Tick "**USB テザリング**" (USB tethering) on the WiFi settings page and save (on / off; off by default).
- On the phone: Settings → Network & internet → Hotspot & tethering → **USB tethering** (names vary). RNDIS (most Android phones) and CDC-NCM / ECM (newer Pixels and others) work. **iPhones do not.**
- The settings page shows the state (waiting for a phone → turn tethering on → waiting for an address → **up**).
- The netpack passes the devices' DNS on to the phone's and routes their traffic out through the phone (NAPT).
- The tethering phone itself should not join the netpack's WiFi (its way out would loop back to the netpack).

**Cabling and power (important)**

- The XIAO is the USB **host**. Plug a **USB-C (male) → USB-A (female) OTG adapter** into the XIAO and use the phone's charging cable (USB-A → phone). A USB-C ↔ USB-C cable makes the phone the host and does not work.
- The phone only sees the connection with **5 V on VBUS**. The XIAO's 5V pin is the USB VBUS, so a netpack **powered through its 5V pin** (through a diode, from a 5 V BEC or USB supply) puts 5 V there. With only a LiPo on the BAT pads there is no 5 V, and the phone sees nothing.
- The phone tries to charge from VBUS (up to about 500 mA). Leave the supply some headroom.
- While it is on, the USB port is not the XIAO's serial port: **the log goes to D6 (GPIO43, UART0 TX, 115200)**. Update from the **Update** page (`/update`) with a firmware.bin, or hold BOOT while plugging the XIAO into a PC.

---

### Web UI Pages

**`/wifi` — WiFi Settings**
- State: the mode and address, where MSP comes from (with a link to the timer), and in host mode the devices on the WiFi and the USB tethering state
- Connection: client (joins a WiFi) / host (the netpack's own WiFi)
- Client: SSID and password (blank keeps the current password)
- Host: SSID, password (8-63 characters; blank keeps it) and channel (6-11)
- USB tethering: on / off (host mode only)
- Changing the connection, the host's WiFi or tethering restarts the netpack after saving

**`/update` — Firmware update** (v2.2.0)
- The firmware's version (release and build)
- Online: "🔍 最新版を確認" (check) → "⬇️ … に更新" (update) when there is a newer release; it is written and the netpack restarts
- A file: pick the firmware.bin of `elrs-xiao-webui`; it is written and the netpack restarts. Settings are kept

**`/voltage` — Voltage Monitor**
- Displays current LiPo voltage
- Divider ratio: set to `2.0` for equal resistors (100 kΩ + 100 kΩ)
- Alarm threshold: default 3.5 V (1S LiPo low-voltage warning)
- Enable / disable voltage alarm independently
- Enable / disable buzzer independently
- Enable / disable notification LED independently

---

### LED Behavior (GPIO9 — 7-color auto-cycling)

| State | Pattern |
|---|---|
| AP mode (needs configuration) | Slow blink 500 ms |
| WiFi disconnected | Rapid blink 80 ms (until reconnected) |
| Voltage alarm active | Solid on |
| STA connected or host mode (normal) | Heartbeat double-pulse, peak brightness ~4%, 2 s period |
| LED disabled (setting) | Off |

### Built-in LED Behavior (GPIO21 — active-LOW)

| State | Pattern |
|---|---|
| Connecting to WiFi | On (solid) |
| AP mode | Blink 200 ms |
| STA connected | Off |

---

### NVS Stored Settings

All settings survive power cycles (stored in ESP32 flash via Preferences).

| Key | Type | Default | Description |
|---|---|---|---|
| `ssid` | String | `ELRS-Netpack-Setup` | WiFi SSID |
| `wifiPass` | String | `elrs-netpack` | WiFi password |
| `configured` | Bool | false | Skip STA on first boot if false |
| `vbatRatio` | Float | 2.0 | Voltage divider ratio |
| `alarmV` | Float | 3.5 | Alarm threshold (V) |
| `vbatAlarmEn` | Bool | false | Voltage alarm enabled |
| `buzzerEn` | Bool | true | Buzzer enabled |
| `ledEn` | Bool | true | Notification LED enabled |
| `langJa` | Bool | true | Language (true = Japanese) |
| `netMode` | UChar | 0 | Connection (0 = client, 1 = host) |
| `hSsid` | String | `ELRS-Netpack` | Host mode SSID |
| `hPass` | String | `elrs-netpack` | Host mode password |
| `hCh` | UChar | 6 | Host mode channel (6-11) |
| `tether` | Bool | false | USB tethering (host mode only) |

---

### Default AP Credentials (elrs-xiao-webui)

| | Value |
|---|---|
| SSID | `ELRS-Netpack-Setup` |
| Password | `elrs-netpack` |
| IP | `192.168.4.1` |
| Settings URL | `http://192.168.4.1/` |

---

## RotorHazard Plugin

Install the following plugin on your Raspberry Pi to enable ELRS backpack communication from RotorHazard:

**[yanazoo/vrxc_elrs](https://github.com/yanazoo/vrxc_elrs)**

Follow the installation instructions in that repository, then proceed to the connection settings below.

---

## RotorHazard Connection Settings

```
Settings → ELRS Backpack General
  Backpack connection type → SOCKET
  ELRS Netpack address     → elrs-netpack.local
                             (or XIAO's IP address)
  Port                     → 8080
```

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| WiFi AUTH_EXPIRE loop | Wrong password → re-enter it via the captive portal (AP mode) |
| `elrs-netpack.local` not found | Check XIAO and RPi are on same network; try IP directly |
| ESP-NOW send error | Confirm goggles backpack is powered on |
| OSD not appearing | Verify ESP-NOW channel matches backpack (default ch 1) |
| PlatformIO board not found | Run `pio platform update espressif32` |
| `No module named intelhex` | Run `C:\Users\<you>\.platformio\penv\Scripts\python.exe -m pip install intelhex` |
| Flash failed | Enter BOOT mode (see above) |

---

## Credits & Fork Info

This project is a fork of the **ELRS Netpack** concept originally designed for the Waveshare ESP32-S3 Ethernet board.

- Original ELRS Backpack protocol: [ExpressLRS/backpack](https://github.com/ExpressLRS/backpack)
- RotorHazard: [RotorHazard/RotorHazard](https://github.com/RotorHazard/RotorHazard)
- Seeed Studio XIAO ESP32-S3 docs: [wiki.seeedstudio.com](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- This fork: [yanazoo/elrs-netpack](https://github.com/yanazoo/elrs-netpack)

The dual-MCU ELRS Backpack bridge approach was developed to resolve the hardware limitation of sharing a single 2.4 GHz radio between WiFi STA and ESP-NOW on the ESP32-S3.
