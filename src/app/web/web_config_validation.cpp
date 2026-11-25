#include "web_config_validation.h"

#include <algorithm>
#include <cctype>

#include "app/calendar/calendar_source.h"

namespace {
std::string webConfigLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool contains(const std::string &text, const char *needle) {
    return webConfigLowerAscii(text).find(webConfigLowerAscii(needle)) != std::string::npos;
}
}  // namespace

std::string redactKeyValueForLog(const std::string &key, const std::string & /*value*/) {
    if (contains(key, "pswd") || contains(key, "password") || contains(key, "secret") ||
        contains(key, "token") || contains(key, "url") || contains(key, "apikey") ||
        contains(key, "portfolio") || contains(key, "portpos") || contains(key, "feed")) {
        return "<redacted>";
    }
    return "<configured>";
}

bool isAllowedRotationInterval(uint16_t minutes) {
    return minutes == 0 || minutes == 30 || minutes == 60 || minutes == 90 ||
           minutes == 120 || minutes == 150;
}

bool isSafeDashboardUrl(const std::string &url) {
    return normalizeCalendarSourceUrl(url).ok;
}

bool isAllowedWeatherModel(const std::string &model) {
    static const char *const kModels[] = {
        "best_match", "ecmwf_ifs025", "ecmwf_ifs04",
        "gfs_seamless", "gfs_global", "icon_seamless", "icon_global",
        "meteofrance_seamless", "jma_seamless",
    };
    for (const char *candidate : kModels) {
        if (model == candidate) return true;
    }
    return false;
}

std::string normalizeWeatherModel(const std::string &model) {
    return isAllowedWeatherModel(model) ? model : std::string("ecmwf_ifs025");
}

uint16_t normalizePortalWindowSec(int32_t seconds) {
    if (seconds <= 0) {
        return 0;
    }
    if (seconds > kPortalWindowSecMax) {
        return kPortalWindowSecMax;
    }
    return static_cast<uint16_t>(seconds);
}
