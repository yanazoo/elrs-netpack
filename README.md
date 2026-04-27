# ELRS Netpack — TCP → WiFi → ESP-NOW Bridge

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
│  XIAO ESP32-S3      │  elrs-xiao-bridge/
│  WiFi STA           │  Receives TCP, forwards via UART
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

Before flashing the XIAO, edit `elrs-xiao-bridge/include/config.h`:

```cpp
#define WIFI_SSID     "your_ssid"
#define WIFI_PASSWORD "your_password"
```

> **Security note:** `config.h` contains your WiFi password. Do not commit it to a public repository.

---

## Flash XIAO ESP32-S3 (`elrs-xiao-bridge/`)

1. Open the folder in VS Code:
   **File → Open Folder → `elrs-xiao-bridge/`**
2. Wait for PlatformIO to install packages (first time only)
3. Connect XIAO via USB-C
4. PlatformIO sidebar → **`xiao_esp32s3` → General → Upload**

If flashing fails, enter bootloader mode:
```
① Hold BOOT button
② Press and release RST
③ Release BOOT
```

**Expected serial output:**
```
[boot] XIAO ESP32-S3 WiFi bridge
[wifi] connecting to your_ssid
[wifi] connected, IP=192.168.x.xxx
[mdns] elrs-netpack.local
[tcp] listening on port 8080
[boot] ready
```

---

## Flash ESP32 Wrover-E (`elrs-espnow-bridge/`)

1. Open the folder in VS Code:
   **File → Open Folder → `elrs-espnow-bridge/`**
2. Connect ESP32 via USB
3. PlatformIO sidebar → **`esp32dev` → General → Upload**

**Expected serial output:**
```
[boot] ESP32 Wrover-E ESP-NOW bridge
[wifi] channel fixed to 1
[espnow] (re)initialized
[boot] ready
```

---

## elrs-xiao-webui — Enhanced XIAO Firmware (Web UI Edition)

`elrs-xiao-webui/` is an enhanced replacement for `elrs-xiao-bridge/` that adds a **Web UI, captive portal, battery voltage monitor, buzzer, and LED notifications** while keeping the full TCP MSP bridge functionality.

> Flash **either** `elrs-xiao-bridge/` **or** `elrs-xiao-webui/` to the XIAO — not both.

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
| TCP MSP bridge | Same as `elrs-xiao-bridge` — full compatibility |
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
| Fast reconnect | Immediate `WiFi.reconnect()` on disconnect; 15 s before full retry |
| Max TX power | 21 dBm (both WiFi and ESP-NOW sides) |
| Backpack version | Reports firmware version 10.1 to RotorHazard |

---

### Web UI Pages

**`/wifi` — WiFi Settings**
- Enter SSID and password → save and connect
- Leave password blank to keep existing password

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
| STA connected (normal) | Heartbeat double-pulse, peak brightness ~4%, 2 s period |
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
| WiFi AUTH_EXPIRE loop | Wrong password in `config.h` → re-flash |
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

The dual-MCU ESP-NOW bridge approach was developed to resolve the hardware limitation of sharing a single 2.4 GHz radio between WiFi STA and ESP-NOW on the ESP32-S3.
