# ELRS Netpack — from WiFi (TCP) to the ELRS backpacks

English | [日本語](README.ja.md)

> [!IMPORTANT]
> This fork uses **two ESP32 boards** to build a **TCP → WiFi → ESP-NOW** backpack link. It is **not compatible** with the original Waveshare ESP32-S3 Ethernet board.

> [!NOTE]
> This project is not affiliated with the ExpressLRS organization.

---

## Overview

A lap timer (**FPV C5 Lap Timer** or **RotorHazard**) sends MSP packets (the goggles' OSD, race signals). This system takes them over WiFi (TCP) and delivers them to the **ELRS VRx backpacks** (FPV goggles) over ESP-NOW. It is built from two boards.

| Board | Firmware | Role |
|---|---|---|
| **netpack XIAO** (Seeed XIAO ESP32-S3) | `elrs-xiao-webui/` | WiFi, TCP server (port 8080), web settings; passes the MSP to the bridge over UART |
| **ELRS Backpack bridge** (XIAO ESP32-C3 / XIAO ESP32-C5 / ESP32 Wrover-E) | `elrs-espnow-bridge/` | ESP-NOW (2.4 GHz, ch 1) to and from the goggles' and radios' backpacks |

```
 FPV C5 Lap Timer / RotorHazard
        │  WiFi, TCP 8080
        ▼
┌──────────────────────────┐
│ netpack XIAO (S3)          │  elrs-xiao-webui/
│ WiFi (client / host)       │  TCP ⇄ UART, web settings, mDNS
└────────────┬─────────────┘
             │  UART 115200 baud (TX / RX / GND)
             ▼
┌──────────────────────────┐
│ ELRS Backpack bridge       │  elrs-espnow-bridge/
│ XIAO C3 / XIAO C5 / Wrover-E │
└────────────┬─────────────┘
             │  ESP-NOW (2.4 GHz, ch 1)
             ▼
   ELRS VRx backpacks (goggles), radios' backpacks
```

**Why two boards?** An ESP32 has one 2.4 GHz radio. WiFi (a router or FPV C5 Lap Timer's WiFi) and ESP-NOW (fixed on ch 1) on one board don't share a channel and packets get lost. Splitting the roles over two boards joined by UART solves it.

> FPV C5 Lap Timer can also take the bridge **wired straight to FPV C5 Lap Timer over UART** (no netpack XIAO; see FPV C5 Lap Timer's [docs/ELRS_UART_SETUP.md](https://github.com/yanazoo/FPV-C5-Lap-Timer/blob/main/docs/ELRS_UART_SETUP.md)). The netpack is for a bridge that can't sit next to FPV C5 Lap Timer, and for RotorHazard.

---

## Two ways to run it

**A. Client mode (default)** — the netpack XIAO joins FPV C5 Lap Timer's WiFi (FPV C5 Lap Timer in host mode) or the venue's router. FPV C5 Lap Timer (or RotorHazard) finds the netpack as `elrs-netpack.local` and connects to TCP 8080.

**B. Host mode (v2.2.0)** — the netpack XIAO runs its own WiFi; FPV C5 Lap Timer (client mode) and phones join it. A setup built around the netpack. An Android phone on its USB port can take this WiFi to the internet with **USB tethering** ([below](#host-mode-and-usb-tethering)).

```
 phones (viewers, admin) ──┐
 FPV C5 Lap Timer ─────────┼── WiFi "ELRS-NETpack" ── netpack XIAO ──UART── ELRS Backpack bridge ── goggles
 (client mode)             ┘     (ch 6-11)              │USB
                                                        └── Android phone (USB tethering) ── internet
```

---

## Hardware

| Part | Notes |
|---|---|
| Seeed Studio XIAO ESP32-S3 | The netpack XIAO; the external-antenna model is recommended |
| A board for the ELRS Backpack bridge (one of) | **XIAO ESP32-C3** (small), **XIAO ESP32-C5** (small, v2.2.1), **ESP32 Wrover-E** (any ESP32 dev board with 4 MB+ flash) |
| 3 jumper wires | TX / RX / GND |
| USB cables | Writing and power (data-capable) |
| (optional) passive buzzer, LED, resistors, LiPo | See [Extra hardware](#extra-hardware-optional) |
| (for USB tethering) USB-C → USB-A OTG adapter | See [USB tethering cabling and power](#usb-tethering-cabling-and-power-important) |

---

## Wiring (netpack XIAO ↔ bridge)

TX and RX **cross**.

| netpack XIAO (S3) | | XIAO ESP32-C3 | XIAO ESP32-C5 | ESP32 Wrover-E |
|---|---|---|---|---|
| D0 / GPIO1 (TX) | → | D7 / GPIO20 (RX) | D7 / GPIO12 (RX) | GPIO26 (RX) |
| D1 / GPIO2 (RX) | ← | D6 / GPIO21 (TX) | D6 / GPIO11 (TX) | GPIO27 (TX) |
| GND | — | GND | GND | GND |

> Don't join 3V3 / 5V between the boards (GND only). Power each board over USB, or from one supply to each board's 5V / GND.

---

## Writing the firmware

### The easy way: the web flasher (no PlatformIO)

Open **[https://yanazoo.github.io/elrs-netpack/](https://yanazoo.github.io/elrs-netpack/)** in **Chrome / Edge** on a PC, plug the device in by USB and press "書き込む" (write).

- Writes the latest release of **ELRS Netpack (XIAO ESP32-S3)** and the **ELRS Backpack bridge** (pick the board: XIAO C3 / XIAO C5 / Wrover-E).
- "**ファイルを指定して書き込む**" (your own files) writes .bin files (a test build, an older release) or a release zip. Addresses come from the file names (editable); a firmware.bin gets the otadata that starts the first app slot.
- Leave "Erase device" / "erase all" unticked to keep the settings (WiFi and so on).
- If it can't connect, hold BOOT while plugging the board in (download mode).

### Updating the netpack XIAO from its settings page

netpack settings → "**更新**" (Update, `/update`):

- **Online**: "🔍 最新版を確認" (check) → "⬇️ … に更新" (update). Needs the internet (a router in client mode, or USB tethering in host mode).
- **A file**: pick a firmware.bin (`elrs-netpack-xiao_esp32s3-firmware.bin`, or the one in the release zip).

### Release files

[Releases](https://github.com/yanazoo/elrs-netpack/releases) carry:

| File | Contents |
|---|---|
| `elrs-netpack-<version>-xiao_esp32s3.zip` | netpack XIAO (bootloader, partitions, firmware) |
| `elrs-espnow-bridge-<version>-<board>.zip` | bridge (`xiao_esp32c3` / `xiao_esp32c5` / `esp32dev`) |
| `*-full.bin` | one image, written at `0x0` (esptool-js and others) |
| `elrs-netpack-xiao_esp32s3-firmware.bin`, `elrs-netpack-version.txt` | for the netpack's online update |

> GitHub Actions builds the web flasher at each release and puts it on GitHub Pages (Settings → Pages → Source must be **GitHub Actions**).

### PlatformIO (from the source)

1. Install **VS Code** and the **PlatformIO IDE** extension.
2. Clone:
   ```powershell
   git clone https://github.com/yanazoo/elrs-netpack
   cd elrs-netpack
   ```
3. Write (`COMx` is the port):

| Device | Command |
|---|---|
| netpack XIAO (S3) | `pio run -d elrs-xiao-webui -e xiao_esp32s3 -t upload --upload-port COMx` |
| bridge (XIAO C3) | `pio run -d elrs-espnow-bridge -e xiao_esp32c3 -t upload --upload-port COMx` |
| bridge (XIAO C5) | `pio run -d elrs-espnow-bridge -e xiao_esp32c5 -t upload --upload-port COMx` |
| bridge (Wrover-E) | `pio run -d elrs-espnow-bridge -e esp32dev -t upload --upload-port COMx` |

- The netpack XIAO and the XIAO C5 bridge build with pioarduino (Arduino-ESP32 3.3); the first build downloads its packages and takes a while.
- If writing fails: hold BOOT, press and release RST, release BOOT, then write.

**Boot output (USB serial, 115200)**

netpack XIAO (first boot, no WiFi set):
```
[boot] XIAO ESP32-S3 WiFi bridge + Web UI  v2.3.0 (xxxxxxx)
[wifi] not configured — starting captive portal immediately
[ap] IP=192.168.4.1
[web] HTTP server on port 80
[boot] ready
```

Bridge:
```
[boot] ESP-NOW bridge (UART RX=GPIO20 TX=GPIO21)   ← XIAO C3 (XIAO C5: GPIO12 / GPIO11, Wrover-E: GPIO26 / GPIO27)
[wifi] channel fixed to 1
[espnow] (re)initialized
[boot] ready
```

---

## First setup (netpack XIAO)

No source edits: everything is set on the web pages.

1. After writing, the netpack XIAO runs the setup WiFi **`ELRS-NETpack-Setup`** (password `elrs-netpack`). Join it from a phone or PC and the settings page opens (else `http://192.168.4.1/`).
2. On "**WiFi**" pick the connection and save.
   - **Client**: the SSID and password of the network to join (FPV C5 Lap Timer's WiFi or a router); up to 5.
   - **Host**: the SSID, password and channel of the netpack's own WiFi.
3. Later, open `http://elrs-netpack.local/` (client mode, or its address), or `http://192.168.4.1/` in host mode.

The bridge needs no setup. Its ESP-NOW channel is `ESPNOW_CHANNEL` in `platformio.ini` (default 1), fixed at build time.

---

## WiFi settings (`/wifi`)

**State**: at the top, a link to **FPV C5 Lap Timer**'s screen (the MSP peer, or `fpv-c5-lt.local` looked up on the same WiFi; "not found" otherwise); then the mode and address, the MSP connection, the firmware version; in host mode the devices on the WiFi and the USB tethering state.

**Client mode** (v2.3.0)
- Up to **5 networks** (SSID, password). Passwords are not shown (blank keeps one); clear an SSID to delete it.
- **Which one**: **the selected one (●)**, or **automatic** (a scan when joining; the strongest saved network in range).
- **Address**: automatic (DHCP) or **fixed** (IP address, subnet mask, gateway, DNS; a blank DNS is the gateway). The fixed address is used on every saved network; put it in FPV C5 Lap Timer's "netpack address" for an instant connection.
- When no network can be joined, the setup WiFi (`ELRS-NETpack-Setup`) comes up and the netpack retries every 15 s. When a joined network drops, it reconnects every 10 s and brings the setup WiFi up after 60 s.
- The network saved by v2.2.x becomes the first one.

**Host mode**: SSID, password (8-63 characters), channel (6-11), **phones / PCs** (1-9, default 2) (v2.3.0).
- A limit for use as a bridge: **one place is always kept free for FPV C5 Lap Timer**; phones / PCs up to the number set (default 2: settings, a look at FPV C5 Lap Timer).
- One more is sent off and kept out for 30 s. FPV C5 Lap Timer is known as the device its OSD connection (TCP) came from (once seen, it can always join).
- Raise it for a crowd watching on the netpack's WiFi (10 devices in all at most).

**USB tethering**: on / off (host mode only).

Changing the connection, the host's WiFi or tethering restarts the netpack after saving.

> **WiFi channel**: ESP-NOW is fixed on **channel 1** (the ELRS VRx backpack's). Keep the router the netpack joins, and the host-mode WiFi, on 2.4 GHz **channels 6-11** (1-4 compete with the OSD).

---

## Host mode and USB tethering

**Host mode** (WiFi settings page)

1. Under "接続方式" (connection) pick **Host**, enter the host SSID, password (8-63 characters) and channel (6-11), and save. The netpack restarts with that WiFi (defaults: SSID `ELRS-NETpack`, password `elrs-netpack`, ch 6).
2. The settings page is at `http://192.168.4.1/` (or `http://elrs-netpack.local/`).
3. Put FPV C5 Lap Timer in **client mode** with this SSID and password. Leave its netpack address at `auto`: it finds the netpack as `elrs-netpack.local`.
4. Phones join the same WiFi; the "**FPV C5 Lap Timer**" link at the top of the netpack's page opens its screen (the MSP peer, or `fpv-c5-lt.local` found on the same WiFi).
5. Phones / PCs **up to the number set** (default 2); one more place is always free for FPV C5 Lap Timer. The page lists the devices on the WiFi with their signal.
6. To go back, pick "Client" and save.

**USB tethering** (host mode only)

With an Android phone on the XIAO's USB-C and its "USB tethering" on, the devices on the netpack's WiFi (FPV C5 Lap Timer, phones) reach the internet through the phone's mobile data (FPV C5 Lap Timer's update check and online update, for instance).

- Tick "**USB テザリング**" (USB tethering) on the WiFi settings page and save (off by default).
- On the phone: Settings → Network & internet → Hotspot & tethering → **USB tethering** (names vary). RNDIS (most Android phones) and CDC-NCM / ECM (newer Pixels and others) work. **iPhones do not.**
- The settings page shows the state (waiting for a phone → turn tethering on → waiting for an address → **up**).
- The netpack passes the devices' DNS on to the phone's and routes their traffic out through the phone (NAPT).
- The tethering phone itself should not join the netpack's WiFi (its way out would loop back to the netpack).

### USB tethering cabling and power (important)

- The XIAO is the USB **host**. Plug a **USB-C (male) → USB-A (female) OTG adapter** into the XIAO and use the phone's charging cable (USB-A → phone). A USB-C ↔ USB-C cable makes the phone the host and does not work.
- The phone only sees the connection with **5 V on VBUS**. The XIAO's 5V pin is the USB VBUS, so a netpack **powered through its 5V pin** (through a diode, from a 5 V BEC or USB supply) puts 5 V there. With only a LiPo on the BAT pads there is no 5 V, and the phone sees nothing.
- The phone tries to charge from VBUS (up to about 500 mA). Leave the supply some headroom.
- While it is on, the USB port is not the XIAO's serial port: **the log goes to D6 (GPIO43, UART0 TX, 115200)**. Update from the **Update** page, or hold BOOT while plugging the XIAO into a PC.

---

## Connecting FPV C5 Lap Timer and RotorHazard

**FPV C5 Lap Timer** (Settings → ELRS OSD)
- Connection: **"WiFi（elrs-netpack XIAO, TCP）"**
- netpack address: `auto` (FPV C5 Lap Timer in host mode looks among the devices on its WiFi; in client mode it looks up `elrs-netpack.local`). With a fixed netpack address, that IP can go here directly.
- "ELRS netpack 接続中" (connected) on the settings screen means it works.

**RotorHazard** (Raspberry Pi)
- Install the plugin **[yanazoo/vrxc_elrs](https://github.com/yanazoo/vrxc_elrs)** and set:
```
Settings → ELRS Backpack General
  Backpack connection type → SOCKET
  ELRS Netpack address     → elrs-netpack.local (or the netpack's IP address)
  Port                     → 8080
```

---

## Extra hardware (optional)

| Part | Notes |
|---|---|
| Passive buzzer | GPIO4 (+) / GPIO6 (−) |
| 7-color auto-cycling LED | GPIO9 (D10) + **100 Ω resistor** in series |
| 2 × 100 kΩ resistors | LiPo voltage divider |
| LiPo battery | 1S recommended (shared by the netpack XIAO and the bridge) |

| XIAO GPIO | Pin | Connects to |
|---|---|---|
| GPIO3 | D2 / A0 | Divider midpoint (LiPo+ → R1 100kΩ → GPIO3 → R2 100kΩ → GND) |
| GPIO4 | D3 | Buzzer + |
| GPIO6 | D5 | Buzzer − |
| GPIO9 | D10 | LED anode (+ 100 Ω → GND) |
| GPIO21 | — | Built-in LED (active-LOW, on board) |
| GPIO43 | D6 | Log out (UART0 TX, while USB tethering is on) |

> **Important:** always put a **100 Ω** resistor in series with the notification LED (to protect GPIO9).

---

## Features (netpack XIAO: elrs-xiao-webui)

| Feature | Description |
|---|---|
| TCP MSP bridge | TCP 8080 ⇄ UART; a new connection replaces the old one (FPV C5 Lap Timer restarted or rejoined) |
| Web settings | WiFi, voltage and update pages; Japanese / English |
| WiFi (client) | 5 networks (selected / automatic), fixed address, setup WiFi (captive portal) when none can be joined |
| WiFi (host) | The netpack's own WiFi (SSID, password, ch 6-11), FPV C5 Lap Timer + phones / PCs (1-9, default 2), DNS relay |
| USB tethering | In host mode, to the internet through an Android phone (RNDIS / NCM / ECM, NAPT) |
| mDNS | `elrs-netpack.local` (web and TCP) |
| Firmware update | Online (GitHub release) or from a file |
| Voltage monitor and alarm | Shows the LiPo voltage; buzzer and LED below the threshold |
| Buzzer / LED notices | 2 beeps on WiFi joined, 1 on settings saved, 1 when FPV C5 Lap Timer (MSP) connects |
| Lost-connection notice | **When the TCP (MSP) connection to FPV C5 Lap Timer drops: 5 long beeps and the LED blinking at medium speed** (until it reconnects). In host mode it is noticed as soon as FPV C5 Lap Timer leaves the netpack's WiFi, otherwise in about 11 s (TCP keepalive). The netpack's own WiFi dropping has its own notice (buzzer for 5 s, fast LED blink) |
| Max TX power | 21 dBm on WiFi and ESP-NOW |
| Backpack version | Reports version 10.1 to the connecting host |

### How the ELRS Backpack bridge sends (elrs-espnow-bridge)

It is **connectionless**, like UDP. The bridge takes the pilot's bind-phrase UID as its own MAC (a goggle backpack only shows OSD packets from that UID) and sends ESP-NOW **broadcasts**: no waiting for an ACK, no retries.

- One pilot's screen update (until the next `SET_SEND_UID`, or 20 ms without sends) goes out **3 times** in a row (`SEND_REPEATS`).
- Copies of the same packet are at least **3 ms** apart. Each pilot's ELRS 2.4 GHz radio hops every few ms and about a third of its hops land in channel 1, so a slightly later copy meets a different hop and gets through more often.
- Goggles that are switched off, however many, don't delay anyone else's OSD. Two goggles on the same bind phrase show the same thing.
- Longer dropouts are covered by the sender's spaced resends (RotorHazard's vrxc_elrs, the lap timer).
- **Receive filter** (custom MSP `0x0F01`, payload = a 6-byte UID, empty to clear): when the host (FPV C5 Lap Timer) names the race director's UID, the bridge passes on only frames **from** that UID (the director's radio switch), not other bridges' broadcast OSD or pilots' own DVR switches. The bridge answers `0x0F01` (payload `1`). Hosts that don't send it (RotorHazard's vrxc_elrs) get everything as before. While a filter is set, the bridge also listens in promiscuous mode for the director's switch (v2.12); the same frame heard twice within 500 ms is passed on once.
- **Link watch** (custom MSP `0x0F03`, payload `1` start / `0` stop, answered with `0x0F03`): asked by the host (FPV C5 Lap Timer), the bridge hears all ESP-NOW on channel 1 and reads the link reports pilots' radio backpacks send (ELRS 3.4+, *Backpack → Telemetry = ESPNOW*). Per UID it reports `0x0F04` (payload: UID 6 bytes, uplink LQ, age in 0.1 s), at once on link up / down and every second otherwise. It stops by itself 20 s after the host's last request.
- **VTX reports** (custom MSP `0x0F05`, during the link watch): when a pilot's radio sends its VTX setting (Lua *VTX Administrator*), the bridge reports `0x0F05` (payload: UID 6 bytes, the VTX setting's first 2 bytes = band/channel index `(band-1)*8+channel`, or a frequency in MHz).
- The XIAO C5 bridge stays on 2.4 GHz (no 5 GHz).
- Frames received over ESP-NOW are queued and written to UART from `loop()` (the WiFi task never waits on the UART).
- Per-packet logs only with `-DBRIDGE_VERBOSE`.

---

## Web UI pages

**`/wifi` — WiFi**: see [above](#wifi-settings-wifi).

**`/update` — Firmware update**
- The firmware's version (release and build)
- Online: "🔍 最新版を確認" (check) → "⬇️ … に更新" (update) when there is a newer release; it is written and the netpack restarts
- A file: pick the firmware.bin of `elrs-xiao-webui`; it is written and the netpack restarts. Settings are kept

**`/voltage` — Voltage monitor**
- Shows the current LiPo voltage
- Divider ratio: `2.0` for equal resistors (100 kΩ + 100 kΩ)
- Alarm threshold: default 3.5 V (1S LiPo low voltage)
- Voltage alarm, buzzer and notification LED on / off each

---

## LEDs

**Notification LED (GPIO9 — 7-color auto-cycling)**

| State | Pattern |
|---|---|
| Setup WiFi (waiting for settings) | Slow blink, 500 ms |
| WiFi lost (client mode) | Fast blink, 80 ms (until reconnected) |
| The connection to FPV C5 Lap Timer (MSP) dropped (after it was up) | Medium blink, 300 ms (until it reconnects) |
| Voltage alarm | On |
| Normal (client joined, or host mode) | Heartbeat, 2 pulses / 2 s (peak ~4%) |
| LED disabled | Off |

**Built-in LED (GPIO21 — active-LOW)**

| State | Pattern |
|---|---|
| Joining WiFi | On |
| Setup WiFi | Blink 200 ms |
| Joined, or host mode | Off |

---

## Stored settings (NVS)

Every setting survives power-off (ESP32 Preferences, namespace `elrs`). Only an erase in the web flasher clears them.

| Key | Type | Default | Description |
|---|---|---|---|
| `netMode` | UChar | 0 | Connection (0 = client, 1 = host) |
| `ss0`–`ss4` / `sp0`–`sp4` | String | (empty) | Client networks 1-5: SSID / password (v2.3.0) |
| `nets` | Bool | false | The 5 networks are saved (false: `ssid` / `wifiPass` become the first) |
| `staSel` | UChar | 0 | The selected network (0-4) |
| `staAuto` | Bool | false | Automatic (the strongest saved network) |
| `ipFix` | Bool | false | Fixed address |
| `ip` / `mask` / `gw` / `dns` | UInt | 0 / 255.255.255.0 / 0 / 0 | The fixed IP address, subnet mask, gateway, DNS |
| `configured` | Bool | false | At least one network is saved |
| `ssid` / `wifiPass` | String | — | The network of v2.2.x and before (moved to the first) |
| `hSsid` | String | `ELRS-NETpack` | Host mode SSID |
| `hPass` | String | `elrs-netpack` | Host mode password |
| `hCh` | UChar | 6 | Host mode channel (6-11) |
| `hMax` | UChar | 2 | Host mode phones / PCs (1-9, besides FPV C5 Lap Timer) |
| `lapMac` | Bytes | — | FPV C5 Lap Timer's MAC (learned from its OSD connection; never sent off by the limit) |
| `tether` | Bool | false | USB tethering (host mode only) |
| `vbatRatio` | Float | 2.0 | Voltage divider ratio |
| `alarmV` | Float | 3.5 | Alarm threshold (V) |
| `vbatAlarmEn` | Bool | false | Voltage alarm on |
| `buzzerEn` | Bool | true | Buzzer on |
| `ledEn` | Bool | true | Notification LED on |
| `langJa` | Bool | true | Language (true = Japanese) |

---

## Default WiFi (public defaults; changing them is recommended)

| | Setup WiFi (client mode, not joined) | Host mode WiFi |
|---|---|---|
| SSID | `ELRS-NETpack-Setup` | `ELRS-NETpack` (editable) |
| Password | `elrs-netpack` | `elrs-netpack` (editable) |
| IP address | `192.168.4.1` | `192.168.4.1` |
| Settings URL | `http://192.168.4.1/` | `http://192.168.4.1/` |

---

## Troubleshooting

| Symptom | What to do |
|---|---|
| The netpack doesn't join the WiFi | Join the setup WiFi (`ELRS-NETpack-Setup`) and check the SSID and password; the router must offer 2.4 GHz (5 GHz only won't do) |
| `elrs-netpack.local` not found | Same network? Fix the netpack's address and use that IP |
| FPV C5 Lap Timer doesn't show "ELRS netpack 接続中" | FPV C5 Lap Timer's connection ("WiFi（elrs-netpack XIAO, TCP）"), its netpack address (`auto` or the IP), the "MSP" line on the netpack's page |
| No OSD in the goggles | The bridge wiring (TX/RX crossed, GND), the bridge's boot log, the bind phrase matching the goggles, the ESP-NOW channel (1) |
| OSD drops or lags | Router / host WiFi on ch 6-11; the bridge's antenna (fit the XIAO C3 / C5 antenna) |
| USB tethering stays at "waiting for a phone" | The OTG adapter on the XIAO side; power through the 5V pin (VBUS needs 5 V) |
| USB tethering stays at "turn tethering on" | Turn the phone's USB tethering on; iPhones don't work |
| Can't write over USB while tethering | Use the Update page, or hold BOOT while plugging into a PC |
| A PlatformIO build fails | The first build downloads packages (takes a while); try `pio pkg update`. On Windows, `No module named intelhex`: `C:\Users\<user>\.platformio\penv\Scripts\python.exe -m pip install intelhex` |

---

## Credits & fork info

This project is a fork of the **ELRS Netpack** concept designed for the Waveshare ESP32-S3 Ethernet board.

- Original ELRS backpack protocol: [ExpressLRS/backpack](https://github.com/ExpressLRS/backpack)
- RotorHazard: [RotorHazard/RotorHazard](https://github.com/RotorHazard/RotorHazard)
- FPV C5 Lap Timer: [yanazoo/FPV-C5-Lap-Timer](https://github.com/yanazoo/FPV-C5-Lap-Timer)
- Seeed Studio XIAO ESP32-S3 docs: [wiki.seeedstudio.com](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- This fork: [yanazoo/elrs-netpack](https://github.com/yanazoo/elrs-netpack)

The two-board setup (netpack XIAO + ELRS Backpack bridge) was made to get around an ESP32 sharing its 2.4 GHz radio between WiFi and ESP-NOW.
