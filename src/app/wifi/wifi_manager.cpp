#include "wifi_manager.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_sntp.h>
#include <lwip/ip_addr.h>
#include <sys/time.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include "app/config/nvs_table.h"
#include "app/config/settings.h"
#include "utils/logger.h"

static const char *TAG = "WifiManager";

static const int CONNECT_TIMEOUT_MS  = 15000;
static const int NTP_SERVER_TIMEOUT_MS = 5000;  // per-server wait
static const int NTP_MAX_RETRIES       = 2;     // retries per server
// Overall budget for all NTP (UDP) attempts; once exhausted we fall back to
// HTTP so a UDP-blocking network does not stall the wake cycle for minutes.
static const unsigned long NTP_TOTAL_BUDGET_MS = 30000;
static const int HTTP_TIME_TIMEOUT_MS = 6000;   // per HTTP time source

// Приоритет источников времени (побеждает первый успешный):
// 1) сервер, полученный от DHCP (option 42) — его выдаёт сам роутер;
// 2) российские публичные серверы (Яндекс и MSK-IX);
// 3) пул OpenWrt;
// 4) исторический список ниже (остальные страны и anycast).
// DHCP-сервер читается до вызова configTime(), который перезаписывает слоты SNTP.
static String s_dhcpNtpServer;
RTC_DATA_ATTR static char s_lastNtpServer[64] = {0};  // last successful NTP, RTC

// Рекомендованные Яндекс.Cloud для РФ + проверенные вручную (UDP/123, 21.11.2026).
// ntp.yandex.ru / ntp1.yandex.ru не отвечают — удалены.
static const char * const NTP_RUSSIAN[] = {
    "ntp.msk-ix.ru",
    "ntp.ix.ru",
    "ntp2.vniiftri.ru",
    "0.ru.pool.ntp.org",
    "1.ru.pool.ntp.org",
    "ntp0.NL.net",
};

static const char * const NTP_OPENWRT[] = {
    "openwrt.pool.ntp.org",
    "0.openwrt.pool.ntp.org",
    "1.openwrt.pool.ntp.org",
};

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

// Региональные пулы: выбираются по координатам из конфигурации.
static const char *const NTP_EUROPE[] = {
    "europe.pool.ntp.org", "de.pool.ntp.org", "fr.pool.ntp.org",
    "uk.pool.ntp.org", "ntp.se",
};
static const char *const NTP_ASIA[] = {
    "asia.pool.ntp.org", "ntp.nict.jp", "ntp.aliyun.com", "ntp.tencent.com",
};
static const char *const NTP_NORTH_AMERICA[] = {
    "north-america.pool.ntp.org", "us.pool.ntp.org", "time.google.com",
};
static const char *const NTP_SOUTH_AMERICA[] = {
    "south-america.pool.ntp.org", "a.ntp.br",
};
static const char *const NTP_AFRICA[] = {
    "africa.pool.ntp.org", "za.pool.ntp.org",
};
static const char *const NTP_OCEANIA[] = {
    "oceania.pool.ntp.org", "au.pool.ntp.org",
};

enum class NtpRegion { Russia, Europe, Asia, NorthAmerica, SouthAmerica, Africa, Oceania, Global };

NtpRegion ntpRegionForCoordinates(double lat, double lon) {
    // Грубая географическая классификация: достаточно, чтобы первым шел
    // ближайший пул, а остальные оставались запасными.
    if (lat >= 45.0 && lat <= 75.0 && lon >= 20.0 && lon <= 180.0)
        return NtpRegion::Russia;
    if (lon >= -12.0 && lon <= 45.0 && lat >= 35.0 && lat <= 72.0)
        return NtpRegion::Europe;
    if (lat > 10.0 && lon >= -170.0 && lon <= -50.0)
        return NtpRegion::NorthAmerica;
    if (lat < 15.0 && lon >= -90.0 && lon <= -30.0)
        return NtpRegion::SouthAmerica;
    if (lat >= -35.0 && lat <= 35.0 && lon >= -20.0 && lon <= 55.0)
        return NtpRegion::Africa;
    if (lat < -10.0 && lon >= 110.0 && lon <= 180.0)
        return NtpRegion::Oceania;
    if (lon >= 40.0 && lon <= 150.0 && lat >= -10.0 && lat <= 60.0)
        return NtpRegion::Asia;
    return NtpRegion::Global;
}

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

namespace wifiReconnect {
namespace {
String _primarySsid, _primaryPassword, _backupSsid, _backupPassword;
String _currentSsid, _currentPassword;
bool     _backupTried = false;
bool     _active = false;
bool     _justConnected = false;
uint32_t _attemptStartMs = 0;

void beginAttempt(const String &ssid, const String &password) {
    if (ssid.length() == 0) return;
    _currentSsid = ssid;
    _currentPassword = password;
    _active = true;
    _attemptStartMs = millis();
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), password.c_str());
    log_i(TAG, "Background WiFi -> %s", ssid.c_str());
}
}  // namespace

void configure(const String &primarySsid, const String &primaryPassword,
               const String &backupSsid, const String &backupPassword) {
    _primarySsid = primarySsid;
    _primaryPassword = primaryPassword;
    _backupSsid = backupSsid;
    _backupPassword = backupPassword;
}

void start() {
    _justConnected = false;
    _backupTried = false;
    _active = false;
    beginAttempt(_primarySsid, _primaryPassword);
}

bool connected() {
    return WiFi.status() == WL_CONNECTED;
}

bool poll() {
    _justConnected = false;
    if (connected()) {
        const bool wasActive = _active;
        _active = false;
        if (wasActive) {
            _justConnected = true;
            log_i(TAG, "Background WiFi connected to %s", WiFi.SSID().c_str());
            return true;
        }
        return false;
    }
    if (!_active) {
        // Потеря соединения: начинаем с основной сети.
        _backupTried = false;
        beginAttempt(_primarySsid, _primaryPassword);
        return false;
    }
    if (millis() - _attemptStartMs < CONNECT_TIMEOUT_MS) return false;

    // Основная не поднялась — пробуем резервную один раз.
    if (!_backupTried && _backupSsid.length() > 0) {
        _backupTried = true;
        beginAttempt(_backupSsid, _backupPassword);
        return false;
    }
    _active = false;   // следующий poll начнёт новую попытку с основной сети
    return false;
}
}  // namespace wifiReconnect

bool WifiManager::connect(const String &ssid, const String &password) {
    WiFi.mode(WIFI_STA);   // инициализирует LwIP/tcpip (Network.begin)

#if LWIP_DHCP_GET_NTP_SRV
    // Вызывать можно только после WiFi.mode(): иначе esp_sntp_servermode_dhcp()
    // дёргает tcpip_callback() с ещё неготовым mbox и плата падает в assert.
    // DHCP ещё не стартовал (начнётся после WiFi.begin + ассоциации), поэтому
    // этой точки достаточно, чтобы получить сервер из option 42.
    esp_sntp_servermode_dhcp(true);
#endif

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

void WifiManager::syncTime(int utcOffsetHours, double lat, double lon) {
    long offsetSec = (long)utcOffsetHours * 3600L;
    const unsigned long ntpStart = millis();

    // RTC теряется при перезагрузке — подтягиваем последний успешный сервер из NVS.
    if (s_lastNtpServer[0] == '\0') {
        Settings store(NVS_NAMESPACE_WEATHER);
        const String saved = store.GetString(NVS_KEY_NTP_SERVER, DEFAULT_NTP_SERVER);
        if (saved.length() > 0) {
            strncpy(s_lastNtpServer, saved.c_str(), sizeof(s_lastNtpServer) - 1);
            s_lastNtpServer[sizeof(s_lastNtpServer) - 1] = '\0';
        }
    }

    // Сначала читаем DHCP option 42: configTime() ниже перезапишет слоты SNTP.
    s_dhcpNtpServer.clear();
    const ip_addr_t *dhcpAddr = esp_sntp_getserver(0);
    // Router без option 42 оставляет в слоте 0.0.0.0 — пробовать его бессмысленно.
    if (dhcpAddr != nullptr && IP_IS_V4(dhcpAddr) &&
        dhcpAddr->u_addr.ip4.addr != 0 &&
        dhcpAddr->u_addr.ip4.addr != 0xFFFFFFFFU) {
        s_dhcpNtpServer = IPAddress(dhcpAddr->u_addr.ip4.addr).toString();
    } else {
        const char *dhcpName = esp_sntp_getservername(0);
        if (dhcpName != nullptr && *dhcpName != '\0') {
            s_dhcpNtpServer = dhcpName;
        }
    }
    if (s_dhcpNtpServer.length()) {
        log_i(TAG, "NTP from DHCP option 42: %s", s_dhcpNtpServer.c_str());
    } else {
        log_i(TAG, "DHCP did not provide an NTP server (option 42)");
    }

    std::vector<String> servers;
    auto addServer = [&servers](const char *server) {
        if (std::find(servers.begin(), servers.end(), String(server)) == servers.end())
            servers.push_back(String(server));
    };
    // Прошлый успешный сервер и DHCP option 42 имеют наивысший приоритет.
    if (s_lastNtpServer[0] != '\0') addServer(s_lastNtpServer);
    if (s_dhcpNtpServer.length()) addServer(s_dhcpNtpServer.c_str());

    // Далее — региональный пул по координатам из конфигурации.
    const NtpRegion region = ntpRegionForCoordinates(lat, lon);
    log_i(TAG, "NTP region for %.2f,%.2f: %d", lat, lon, static_cast<int>(region));
    switch (region) {
        case NtpRegion::Russia:
            for (const char *s : NTP_RUSSIAN) addServer(s);
            break;
        case NtpRegion::Europe:
            for (const char *s : NTP_EUROPE) addServer(s);
            break;
        case NtpRegion::Asia:
            for (const char *s : NTP_ASIA) addServer(s);
            break;
        case NtpRegion::NorthAmerica:
            for (const char *s : NTP_NORTH_AMERICA) addServer(s);
            break;
        case NtpRegion::SouthAmerica:
            for (const char *s : NTP_SOUTH_AMERICA) addServer(s);
            break;
        case NtpRegion::Africa:
            for (const char *s : NTP_AFRICA) addServer(s);
            break;
        case NtpRegion::Oceania:
            for (const char *s : NTP_OCEANIA) addServer(s);
            break;
        case NtpRegion::Global:
            break;
    }
    for (const char *s : NTP_OPENWRT) addServer(s);
    for (uint8_t i = 0; i < NTP_SERVER_CNT; ++i) addServer(NTP_SERVERS[i]);

    for (size_t si = 0; si < servers.size(); si++) {
        if (millis() - ntpStart >= NTP_TOTAL_BUDGET_MS) {
            log_w(TAG, "NTP time budget exhausted (%lu ms) — falling back to HTTP",
                  (unsigned long)NTP_TOTAL_BUDGET_MS);
            break;
        }
        const char *server = servers[si].c_str();
        if (si > 0 && s_lastNtpServer[0] != '\0' &&
            strcmp(server, s_lastNtpServer) == 0) {
            continue;   // уже пробывали первым
        }
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
            strncpy(s_lastNtpServer, server, sizeof(s_lastNtpServer) - 1);
            s_lastNtpServer[sizeof(s_lastNtpServer) - 1] = '\0';
            {
                Settings store(NVS_NAMESPACE_WEATHER, true);
                store.SetString(NVS_KEY_NTP_SERVER, s_lastNtpServer);
            }
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
