# ELRS Netpack — WiFi → ESP-NOW Bridge

English | [日本語](README.ja.md)

> [!IMPORTANT]
> This fork replaces the single-MCU WiFi + ESP-NOW design with a **dual-MCU architecture** that solves the fundamental radio conflict on ESP32-S3.
> It is **not** compatible with the original Waveshare ESP32-S3 Ethernet board.

> [!NOTE]
> This project is **not** officially affiliated with or supported by the ExpressLRS organisation.

---

## Overview

The ESP32-S3 has one shared 2.4 GHz radio. Running WiFi STA and ESP-NOW simultaneously on the same chip causes packet loss and peer-list corruption. This fork solves that by splitting the roles across two boards connected by UART:

```
RotorHazard (Raspberry Pi)
        │  TCP port 8080
        ▼
┌─────────────────────┐
│  XIAO ESP32-S3      │  elrs-xiao-bridge/
│  WiFi STA + TCP +   │
│  mDNS               │
└────────┬────────────┘
         │  UART 115200 baud
         │  D0(GPIO1) TX ──► GPIO26 RX
         │  D1(GPIO2) RX ◄── GPIO27 TX
         │  GND        ◄──►  GND
         ▼
┌─────────────────────┐
│  ESP32 Wrover-E     │  elrs-espnow-bridge/
│  ESP-NOW only       │
└────────┬────────────┘
         │  ESP-NOW (2.4 GHz, ch 1)
         ▼
   ELRS VRx Backpack (FPV Goggles)
```

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

## RotorHazard Connection Settings

```
Settings → ELRS Backpack General
  Backpack connection type → SOCKET
  ELRS Netpack address     → elrs-netpack.local
                             (or XIAO's IP address)
  Port                     → 8080
```

---

## RotorHazard Plugin

The plugin (`custom_plugins/netpack_installer/`) lets you check device connectivity and flash firmware from the RotorHazard UI.

**Install on Raspberry Pi:**
```bash
cp -r custom_plugins/netpack_installer ~/rh-data/plugins/
sudo systemctl restart rotorhazard
```

> Install to `~/rh-data/plugins/`, **not** `~/RotorHazard/src/server/plugins/`.

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

## 3D-Printable Case

Case files for the original Waveshare board are in `resources/3d-case/` (not sized for XIAO).

[![3D-Printable Case](resources/3d-case/case-photo.jpg)](resources/3d-case/)

---

## Credits & Fork Info

This project is a fork of the **ELRS Netpack** concept originally designed for the Waveshare ESP32-S3 Ethernet board.

- Original ELRS Backpack protocol: [ExpressLRS/backpack](https://github.com/ExpressLRS/backpack)
- RotorHazard: [RotorHazard/RotorHazard](https://github.com/RotorHazard/RotorHazard)
- Seeed Studio XIAO ESP32-S3 docs: [wiki.seeedstudio.com](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- This fork: [yanazoo/elrs-netpack](https://github.com/yanazoo/elrs-netpack)

The dual-MCU ESP-NOW bridge approach was developed to resolve the hardware limitation of sharing a single 2.4 GHz radio between WiFi STA and ESP-NOW on the ESP32-S3.
