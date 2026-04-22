#pragma once

// WiFi (use NVS values if saved; these are compile-time defaults)
#define WIFI_SSID        "ELRS-Netpack-Setup"
#define WIFI_PASSWORD    "elrs-netpack"

#define TCP_PORT         8080
#define MDNS_HOSTNAME    "elrs-netpack"

// UART to ESP32 Wrover-E
// Wire: XIAO D0(GPIO1/TX) -> ESP32 GPIO26(RX)
//       XIAO D1(GPIO2/RX) <- ESP32 GPIO27(TX)
#define UART_TX_PIN      1    // D0
#define UART_RX_PIN      2    // D1
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
#define ap_ssid          "ELRS-Netpack-Setup"
#define ap_password      "elrs-netpack"

// Built-in LED (XIAO ESP32-S3: GPIO21, active-LOW)
#define LED_BUILTIN_PIN  21
#define LED_AP_INTERVAL  200   // ms - fast blink half-period in AP mode

// Buzzer (GPIO44 = positive pole, GPIO8 = negative pole)
// Alarm ON : GPIO44=HIGH, GPIO8=LOW  -> 3.3 V across buzzer
// Alarm OFF: GPIO44=LOW,  GPIO8=LOW
#define BUZZER_PIN_POS   44   // D7
#define BUZZER_PIN_NEG   8    // D9

// Notification LED (mirrors buzzer state)
#define LED_NOTIFY_PIN   9
