# Pull request: NTP, fallback page, Open-Meteo and AHT20 fixes

**Base:** `RockBase-lab/ESP32-Dashboard@main`
**Head:** `pr/upstream-fixes`
**Title:** Coordinate-aware NTP pools, fallback page fix, Open-Meteo endpoint/model fallback, AHT20 fixes

## 1. Geolocation-based NTP servers

- `src/app/wifi/wifi_manager.cpp/.h`
  - `WifiManager::syncTime(int utcOffsetHours, double lat, double lon)`.
  - The nearest regional NTP pool is selected from the configured coordinates:
    Russia/CIS, Europe, Asia, North America, South America, Africa, Oceania.
    The OpenWrt pool and the global server list remain as fallbacks.
  - The server received via DHCP option 42 and the last successful server
    (NVS `NtpServer`) still have the highest priority.
- `src/app/config/nvs_table.h` — `NtpServer` and `WthModel` keys and defaults.
- `src/app/dashboardApp.cpp` — `cfg.lat` / `cfg.lon` are passed to `syncTime()`.

## 2. Backup WiFi access point

- `src/app/config/nvs_table.h`, `app_config.h/.cpp` — new settings `WifiSSID2` /
  `WifiPSWD2` (backup network; empty by default).
- `src/app/web/web_server.cpp`, `src/html/index.html` — **Primary / Backup WiFi**
  fields in the portal; an empty backup password keeps the stored one, and the
  password never appears in JSON or logs.
- `src/app/dashboardApp.cpp` — `wifiReconnect::configure()` receives both networks.
  If the primary network is temporarily unavailable, the device no longer enters
  AP mode and blocks the UI: it renders the fallback page, starts a background
  reconnect (primary, then backup once), and `wifiReconnect::poll()` runs in the
  interactive button loop. AP mode is used only when neither SSID is configured.

## 3. Blank page on data errors

Previously, when `!dataOk` (`_fetchData()` returned false), the device went
straight to deep sleep without rendering anything — the panel stayed blank white
after EPD auto-detection. Now the warning is logged and the normal page is
rendered with fallback data (`--` instead of fabricated values).

## 4. Alternative Open-Meteo servers

- `src/app/weather/weather.cpp/.h`
  - `FORECAST_ENDPOINTS` / `AQI_ENDPOINTS` tables: canonical host plus alternative IPs;
  - connect to an alternative IP while keeping the canonical `Host`/SNI;
  - iterate endpoints until one returns a successfully parsed response;
  - added `src/app/net/bounded_string_stream.h` to cap the response at 128 KB so a
    large body cannot push out MQTT/HTTPS state.
- Requests switched to HTTPS.

## 5. Open-Meteo weather model selection

- New configuration field `weatherModel` (`WthModel`), default `ecmwf_ifs025`.
- `src/app/web/web_config_validation.cpp/.h` — model whitelist:
  `best_match`, `ecmwf_ifs025`, `ecmwf_ifs04`, `gfs_seamless`, `gfs_global`,
  `icon_seamless`, `icon_global`, `meteofrance_seamless`, `jma_seamless`.
- `src/app/web/web_server.cpp` — JSON/NVS read and write with value validation.
- `src/html/index.html` — **Weather model** dropdown in the Weather section.
- `src/app/dashboardApp.cpp` — `weather.fetchWeather(lat, lon, cfg.weatherModel)`.

## 6. AHT20 sensor and related issues

- `src/app/dashboardApp.cpp`
  - the sensor is powered only on the Weather Home page and only once every
    15 minutes (`kIndoorReadIntervalMinutes`), and is powered down right after
    the reading;
  - the last reading survives deep sleep (`s_lastIndoorReadUtc`); on failure the
    cached value is kept and the sensor is not probed again immediately;
  - explicit log messages for "skipped", "read failed" and "cached".
- `src/app/weather/weather_page_adapter.cpp`
  - fabricated `22 °C` fallback weather values replaced with `-999`
    (displayed as `--`), so missing data is not presented as a real measurement.

## Other fixes already made in this fork

- **MQTT:** NVS cache removed — live data only; configurable subscriptions, six
  sensor slots, MAC filter, kPa → mmHg pressure conversion, 5–300 s reconnect
  backoff, human-readable status and "data N" logging instead of spam.
- **Router uptime:** published to `homed/system/uptime` by the HOMEd automation
  "Weather: journal"; compact `Uptime 10d05h` line at the bottom of the MQTT page.
- **BLE:** pixel-drawn degree sign, red bold names, previous values preserved
  between scans.
- **Screen:** six MQTT cards in a 4+2 grid, larger headings, row spacing, `bat`
  only below 20 %, RSSI removed; `/screen.bmp` uses an 8-bit palette in PSRAM;
  internet status shown in red on failures.
- **Network:** static IP with subnet validation, NTP server in NVS, night mode
  00:00–06:00, daily restart at 06:00, 30-minute redraw.
- **Portal:** unused sources removed, Indoor/Telegram kept, Static IP,
  Night/Restart, Uptime topic and MQTT page title settings; sensor names capped
  at 13 characters.
- **Stability:** unnecessary network polling disabled for off pages, BLE scan
  moved after the first render, internet check running in a background task.
- **Measurements:** full refresh takes ~11.9 s (within GDEY042Z98 spec);
  the `R22=0xC7` experiment was reverted as it gave no speedup.
