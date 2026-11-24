#pragma once

// Pins below are the netpack's own wiring. The "timerwiring" build
// (-DNP_TIMER_WIRING=1, env xiao_esp32s3_timerwiring) is for an FPV C5 Lap
// Timer made into a netpack: its XIAO C5 becomes the ESP-NOW bridge on the
// timer's own UART wires, so nothing is rewired.
//   UART to the bridge  D3 / GPIO4 (TX) -> C5 D4 / GPIO23 (RX)
//                       D4 / GPIO5 (RX) <- C5 D5 / GPIO24 (TX)
//   buzzer +            D5 / GPIO6 (- to GND)
//   mode switch         D1 / GPIO2 to GND
//   notification LED    D0 / GPIO1
//   LiPo voltage        D2 / GPIO3 (as the netpack)
// NP_VARIANT goes into the firmware (fwVariant()): the online update takes
// this build's own file, and a file of the other build is refused.
#if NP_TIMER_WIRING
#define NP_VARIANT       "timerwiring"
#define UART_TX_PIN      4    // D3
#define UART_RX_PIN      5    // D4
#define BUZZER_PIN_POS   6    // D5
#define BUZZER_PIN_NEG   -1   // GND
#define MODE_SW_PIN      2    // D1
#define MODE_SW_LABEL    "D1 (GPIO2)"
#define MODE_SW_LABEL_JA "D1（GPIO2）"
#define LED_NOTIFY_PIN   1    // D0
#else
#define NP_VARIANT       "std"
#endif

// WiFi (use NVS values if saved; these are compile-time defaults)
#define WIFI_SSID        "ELRS-NETpack-Setup"
#define WIFI_PASSWORD    "elrs-netpack"

#define TCP_PORT         8080
#define MDNS_HOSTNAME    "elrs-netpack"

// UART to ESP32 Wrover-E
// Wire: XIAO D0(GPIO1/TX) -> ESP32 GPIO26(RX)
//       XIAO D1(GPIO2/RX) <- ESP32 GPIO27(TX)
#ifndef UART_TX_PIN
#define UART_TX_PIN      1    // D0
#endif
#ifndef UART_RX_PIN
#define UART_RX_PIN      2    // D1
#endif
#define UART_BAUD        115200

// Voltage monitor
// Divider: LiPo+ -- R1(100k) --+-- GPIO3 -- R2(100k) -- GND
// ratio = (R1+R2)/R2 = 2.0 for equal resistors
#define VBAT_ADC_PIN          3        // GPIO3 = D2/A0, ADC1 (compatible with WiFi)
#define VBAT_ADC_RESOLUTION   4095.0f
#define VBAT_VREF             3.3f
#define VBAT_DEFAULT_RATIO    2.0f
#define VBAT_DEFAULT_ALARM_V  3.5f    // 1S LiPo low-voltage warning threshold

// AP mode (captive portal)
#define ap_ssid          "ELRS-NETpack-Setup"
#define ap_password      "elrs-netpack"

// Built-in LED (XIAO ESP32-S3: GPIO21, active-LOW)
#define LED_BUILTIN_PIN  21
#define LED_AP_INTERVAL  200   // ms - fast blink half-period in AP mode

// Buzzer (GPIO4 = positive pole, GPIO6 = negative pole)
// Alarm ON : GPIO4=HIGH, GPIO6=LOW  -> 3.3 V across buzzer
// Alarm OFF: GPIO4=LOW,  GPIO6=LOW
// BUZZER_PIN_NEG -1: the negative pole on GND (timerwiring).
#ifndef BUZZER_PIN_POS
#define BUZZER_PIN_POS   4    // D3
#endif
#ifndef BUZZER_PIN_NEG
#define BUZZER_PIN_NEG   6    // D5
#endif

// Notification LED (mirrors buzzer state)
#ifndef LED_NOTIFY_PIN
#define LED_NOTIFY_PIN   9
#endif

// Host mode: the netpack's own WiFi (the timer and the phones join it).
// Channels 6-11 only, as the timer (the ESP-NOW bridge stays on ch 1).
#define HOST_SSID_DEFAULT  "ELRS-NETpack"
#define HOST_PASS_DEFAULT  "elrs-netpack"
#define HOST_CH_DEFAULT    6
#define HOST_MAX_STA       10

// Host mode, devices besides FPV C5 Lap Timer (phones, PCs): limited as a
// bridge (default 5, as FPV C5 Lap Timer's own host mode). One more place
// is kept for FPV C5 Lap Timer at all times.
#define HOST_PHONES_DEFAULT 5
#define HOST_PHONES_MAX     (HOST_MAX_STA - 1)

// Mode switch (optional, ticked on the WiFi page): D4 / GPIO5 to GND,
// internal pull-up, read once at boot. Connected (ON) = host mode.
#ifndef MODE_SW_PIN
#define MODE_SW_PIN      5    // D4
#endif
#ifndef MODE_SW_LABEL                 // the pin's name on the WiFi page
#define MODE_SW_LABEL    "D4 (GPIO5)"
#define MODE_SW_LABEL_JA "D4（GPIO5）"
#endif

// The admin password: every page but the QR page asks for it (a login,
// kept 12 h). Public default: change it on the WiFi page.
#define ADMIN_PASS_DEFAULT "admin"
