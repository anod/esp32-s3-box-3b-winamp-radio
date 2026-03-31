// ============================================================
// Configuration — override via .env file (see README)
// ============================================================

#pragma once

// Wi-Fi credentials
#ifndef WIFI_SSID
#define WIFI_SSID     "YOUR_WIFI_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// Audio
#define DEFAULT_VOLUME  15   // 0–21
#define MAX_VOLUME      21
