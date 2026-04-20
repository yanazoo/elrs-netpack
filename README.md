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
