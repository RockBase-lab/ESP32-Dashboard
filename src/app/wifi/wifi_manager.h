#pragma once

#include <Arduino.h>

// Фоновый реконнект: вызывается из интерактивного цикла, никогда не блокирует
// обработку кнопок. При потере основной сети сам переходит на резервную.
namespace wifiReconnect {
void configure(const String &primarySsid, const String &primaryPassword,
               const String &backupSsid, const String &backupPassword);
void start();
bool connected();
bool poll();   // true, если соединение только что восстановилось
}

// WifiManager — connect to an AP, sync time via SNTP, and disconnect.
class WifiManager {
public:
    // Connect to the given SSID. Returns true when an IP is obtained.
    // Blocks until connected or a timeout (~15 s) is reached.
    bool connect(const String &ssid, const String &password);

    // Synchronize the system clock via SNTP.
    // utcOffsetHours is the UTC offset in hours, e.g. 8 for UTC+8, -5 for UTC-5.
    // lat/lon (optional) pick the nearest regional NTP pool first.
    // Falls back to HTTP time sources when all NTP (UDP) servers fail.
    void syncTime(int utcOffsetHours, double lat = 0.0, double lon = 0.0);

    // HTTP-based time sync for networks that block UDP/NTP.
    // Tries JSON time APIs first, then plain-HTTP Date headers.
    bool syncTimeHttp(int utcOffsetHours);

    // Disconnect WiFi and power down the radio.
    void disconnect();

    // Switch to SoftAP mode with the given SSID (open network, no password).
    // Default IP: 192.168.4.1. Call webServer.start() after this.
    void startAP(const String &ssid, const String &password = "");
};
