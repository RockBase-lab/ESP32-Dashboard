#include "wifi_manager.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include "utils/logger.h"

static const char *TAG = "WifiManager";

static const int CONNECT_TIMEOUT_MS  = 15000;
static const int NTP_SERVER_TIMEOUT_MS = 5000;  // per-server wait
static const int NTP_MAX_RETRIES       = 2;     // retries per server
// Overall budget for all NTP (UDP) attempts; once exhausted we fall back to
// HTTP so a UDP-blocking network does not stall the wake cycle for minutes.
static const unsigned long NTP_TOTAL_BUDGET_MS = 30000;
static const int HTTP_TIME_TIMEOUT_MS = 6000;   // per HTTP time source

// NTP server pool — tried in order, first success wins.
static const char * const NTP_SERVERS[] = {
    // ── Asia ──────────────────────────────────────────────────────────────
    "ntp.aliyun.com",           // Alibaba CN
    "ntp.tencent.com",          // Tencent CN
    "ntp.ntsc.ac.cn",           // National Time Service Center CN
    "cn.pool.ntp.org",          // NTP Pool CN
    "asia.pool.ntp.org",        // NTP Pool Asia
    "ntp.nict.jp",              // Japan (NICT)
    "time.nist.gov",            // NIST (also serves Asia well)

    // ── Europe ────────────────────────────────────────────────────────────
    "europe.pool.ntp.org",      // NTP Pool Europe
    "de.pool.ntp.org",          // Germany
    "fr.pool.ntp.org",          // France
    "uk.pool.ntp.org",          // United Kingdom
    "ntp.se",                   // Sweden (RISE)
    "time.euro.apple.com",      // Apple Europe

    // ── North America ─────────────────────────────────────────────────────
    "north-america.pool.ntp.org", // NTP Pool North America
    "us.pool.ntp.org",          // United States
    "time.windows.com",         // Microsoft
    "time.apple.com",           // Apple

    // ── South America ─────────────────────────────────────────────────────
    "south-america.pool.ntp.org", // NTP Pool South America
    "a.ntp.br",                 // Brazil (NTP.br)

    // ── Africa ────────────────────────────────────────────────────────────
    "africa.pool.ntp.org",      // NTP Pool Africa
    "za.pool.ntp.org",          // South Africa

    // ── Oceania ───────────────────────────────────────────────────────────
    "oceania.pool.ntp.org",     // NTP Pool Oceania
    "au.pool.ntp.org",          // Australia

    // ── Global fallbacks ──────────────────────────────────────────────────
    "pool.ntp.org",             // Global pool
    "time.cloudflare.com",      // Cloudflare (anycast)
    "time.google.com",          // Google (anycast)
};
static const uint8_t NTP_SERVER_CNT = sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]);

// ─── HTTP time sources (fallback when UDP/NTP is blocked) ───────────────────
// mode 0: JSON body contains "unixtime":<seconds>
// mode 1: JSON body contains "t":"<milliseconds>"
// mode 2: read the standard HTTP Date response header
struct HttpTimeSource {
    const char *url;
    uint8_t     mode;
};
static const HttpTimeSource HTTP_TIME_SOURCES[] = {
    {"http://worldtimeapi.org/api/timezone/Etc/UTC", 0},
    {"http://api.m.taobao.com/rest/api3.do?api=mtop.common.getTimestamp", 1},
    {"http://connectivitycheck.gstatic.com/generate_204", 2},
    {"http://www.msftconnecttest.com/connecttest.txt", 2},
    {"http://detectportal.firefox.com/success.txt", 2},
};
static const uint8_t HTTP_TIME_SRC_CNT = sizeof(HTTP_TIME_SOURCES) / sizeof(HTTP_TIME_SOURCES[0]);

bool WifiManager::connect(const String &ssid, const String &password) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    log_i(TAG, "Connecting to %s ...", ssid.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > CONNECT_TIMEOUT_MS) {
            log_e(TAG, "Connection timed out");
            return false;
        }
        delay(250);
    }
    log_i(TAG, "Connected, IP=%s RSSI=%d", WiFi.localIP().toString().c_str(),
          WiFi.RSSI());
    return true;
}

// ─── Time helpers ───────────────────────────────────────────────────────────
// Civil date → seconds since 1970-01-01 (Howard Hinnant's days_from_civil),
// avoiding any dependence on timegm() availability in newlib.
static time_t epochFromUtc(int y, unsigned m, unsigned d,
                           unsigned hh, unsigned mm, unsigned ss) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const time_t days = era * 146097 + static_cast<time_t>(doe) - 719468;
    return days * 86400 + static_cast<time_t>(hh) * 3600 + mm * 60 + ss;
}

// Parse an IMF-fixdate ("Wed, 21 Oct 2015 07:28:00 GMT") into a UTC epoch.
static bool parseHttpDate(const char *date, time_t &outEpoch) {
    if (!date) {
        return false;
    }
    char wday[4] = {0}, mon[4] = {0};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(date, "%3s, %d %3s %d %d:%d:%d",
               wday, &day, mon, &year, &hour, &minute, &second) != 7) {
        return false;
    }
    static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *hit = strstr(kMonths, mon);
    if (!hit || year < 2020) {
        return false;
    }
    outEpoch = epochFromUtc(year, static_cast<unsigned>((hit - kMonths) / 3 + 1),
                            static_cast<unsigned>(day),
                            static_cast<unsigned>(hour),
                            static_cast<unsigned>(minute),
                            static_cast<unsigned>(second));
    return true;
}

// Extract the first unsigned integer following "key" in a flat JSON body.
static bool jsonIntAfter(const String &body, const char *key, int64_t &out) {
    String needle = String('"') + key + "\":";
    int i = body.indexOf(needle);
    if (i < 0) {
        return false;
    }
    i += needle.length();
    while (i < (int)body.length() && (body[i] == ' ' || body[i] == '"')) {
        i++;
    }
    int64_t value = 0;
    bool any = false;
    while (i < (int)body.length() && body[i] >= '0' && body[i] <= '9') {
        value = value * 10 + (body[i] - '0');
        any = true;
        i++;
    }
    if (!any) {
        return false;
    }
    out = value;
    return true;
}

// Set the RTC from a UTC epoch and apply the same local-time offset that
// configTime() would have applied, so getLocalTime() keeps its semantics.
// Uses the POSIX TZ form directly (sign is inverted: UTC+8 → "UTC-8").
static void applyUtcEpoch(time_t epoch, long offsetSec) {
    char tz[20];
    snprintf(tz, sizeof(tz), "UTC%+ld", -offsetSec / 3600L);
    setenv("TZ", tz, 1);
    tzset();
    struct timeval tv;
    tv.tv_sec  = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
}

void WifiManager::syncTime(int utcOffsetHours) {
    long offsetSec = (long)utcOffsetHours * 3600L;
    const unsigned long ntpStart = millis();

    for (uint8_t si = 0; si < NTP_SERVER_CNT; si++) {
        if (millis() - ntpStart >= NTP_TOTAL_BUDGET_MS) {
            log_w(TAG, "NTP time budget exhausted (%lu ms) — falling back to HTTP",
                  (unsigned long)NTP_TOTAL_BUDGET_MS);
            break;
        }
        const char *server = NTP_SERVERS[si];
        log_i(TAG, "NTP trying: %s", server);

        esp_sntp_stop();
        configTime(offsetSec, 0, server);

        bool synced = false;
        for (uint8_t retry = 0; retry < NTP_MAX_RETRIES && !synced; retry++) {
            unsigned long start = millis();
            struct tm tm_info;
            while (millis() - start < (unsigned long)NTP_SERVER_TIMEOUT_MS) {
                if (getLocalTime(&tm_info) && tm_info.tm_year > 120) {
                    synced = true;
                    break;
                }
                delay(200);
            }
        }

        if (synced) {
            struct tm tm_info;
            getLocalTime(&tm_info);
            log_i(TAG, "NTP synced via %s — %04d-%02d-%02d %02d:%02d:%02d (UTC%+d)",
                  server,
                  tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                  tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                  utcOffsetHours);
            return;
        }

        log_w(TAG, "NTP server %s failed, trying next...", server);
    }

    // UDP path exhausted (blocked or unreachable) — try plain HTTP.
    if (syncTimeHttp(utcOffsetHours)) {
        return;
    }

    log_e(TAG, "All NTP and HTTP time sync methods failed — continuing without time sync");
}

bool WifiManager::syncTimeHttp(int utcOffsetHours) {
    const long offsetSec = static_cast<long>(utcOffsetHours) * 3600L;

    for (uint8_t si = 0; si < HTTP_TIME_SRC_CNT; si++) {
        const HttpTimeSource &src = HTTP_TIME_SOURCES[si];
        log_i(TAG, "HTTP time trying: %s", src.url);

        HTTPClient http;
        http.setTimeout(HTTP_TIME_TIMEOUT_MS);
        const char *collect[] = {"Date"};
        http.collectHeaders(collect, 1);
        if (!http.begin(src.url)) {
            log_w(TAG, "HTTP begin failed: %s", src.url);
            continue;
        }

        const int code = http.GET();
        time_t epoch = 0;
        bool ok = false;
        if (code > 0) {
            if (src.mode == 2) {
                if (http.hasHeader("Date")) {
                    const String date = http.header("Date");
                    ok = parseHttpDate(date.c_str(), epoch);
                }
            } else {
                const String body = http.getString();
                int64_t v = 0;
                if (src.mode == 0) {
                    ok = jsonIntAfter(body, "unixtime", v);
                } else {
                    ok = jsonIntAfter(body, "t", v);
                    v /= 1000;  // milliseconds → seconds
                }
                // Sanity check: reject anything before 2020-01-01.
                if (ok && v >= 1577836800LL) {
                    epoch = static_cast<time_t>(v);
                } else {
                    ok = false;
                }
            }
        } else {
            log_w(TAG, "HTTP GET failed: %s code=%d", src.url, code);
        }
        http.end();

        if (!ok) {
            log_w(TAG, "HTTP time source %s failed, trying next...", src.url);
            continue;
        }

        applyUtcEpoch(epoch, offsetSec);
        struct tm tm_info;
        if (getLocalTime(&tm_info) && tm_info.tm_year > 120) {
            log_i(TAG, "HTTP time synced via %s — %04d-%02d-%02d %02d:%02d:%02d (UTC%+d)",
                  src.url,
                  tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                  tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                  utcOffsetHours);
            return true;
        }
        log_w(TAG, "HTTP time from %s rejected by local check", src.url);
    }

    log_e(TAG, "All HTTP time sources failed");
    return false;
}

void WifiManager::disconnect() {
    WiFi.disconnect(/*wifioff=*/true);
    log_i(TAG, "WiFi disconnected");
}

void WifiManager::startAP(const String &ssid, const String &password) {
    WiFi.disconnect(/*wifioff=*/true); // ensure STA mode is off
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);

    const IPAddress apIP(192, 168, 4, 1);
    const IPAddress netmask(255, 255, 255, 0);
    const bool ipOk = WiFi.softAPConfig(apIP, apIP, netmask);
    if (password.length() >= 8) {
        WiFi.softAP(ssid.c_str(), password.c_str());
    } else {
        WiFi.softAP(ssid.c_str());
    }
    log_i(TAG, "SoftAP started: SSID=%s security=%s configIP=%d IP=%s",
          ssid.c_str(), password.length() >= 8 ? "WPA2" : "open",
          ipOk ? 1 : 0,
          WiFi.softAPIP().toString().c_str());
}
