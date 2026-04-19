#pragma once

#define WIFI_SSID        "y_air-GL"
#define WIFI_PASSWORD    "your_password_here"

#define TCP_PORT         8080
#define MDNS_HOSTNAME    "elrs-netpack"

// UART to ESP32 Wrover-E
// Wire: XIAO D0(GPIO1/TX) → ESP32 GPIO26(RX)
//       XIAO D1(GPIO2/RX) ← ESP32 GPIO27(TX)
#define UART_TX_PIN      1   // D0
#define UART_RX_PIN      2   // D1
#define UART_BAUD        115200
