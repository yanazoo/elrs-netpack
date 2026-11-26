#pragma once
// Where the log goes: the USB serial.
#include <Arduino.h>
extern Print *g_log;
#define LOG (*g_log)
