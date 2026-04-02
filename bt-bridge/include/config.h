// ============================================================
// BT Bridge Configuration
// ============================================================

#pragma once

// Bluetooth speaker name to connect to
#ifndef BT_SINK_NAME
#define BT_SINK_NAME  "JBL Flip 4"
#endif

// Ring buffer size in stereo frames (~186ms at 44.1kHz)
#define RING_BUF_FRAMES  8192

// Status LED pin (GPIO 2 = onboard LED on most ESP32 dev boards)
// Note: DevKit V4 LED is active-low (LOW = ON, HIGH = OFF)
#define LED_PIN       2
#define LED_ON        LOW
#define LED_OFF       HIGH
