#pragma once
// Where the log goes: the USB serial normally; UART0 (D6 = GPIO43 TX,
// 115200) while USB tethering holds the USB port.
#include <Arduino.h>
extern Print *g_log;
#define LOG (*g_log)
