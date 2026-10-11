#include "jelly/util.hpp"
#include "jelly/platform.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace jelly {

int64_t systemClock() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string buildQuery(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string out;
    for (const auto& [k, v] : params) {
        if (!out.empty()) out.push_back('&');
        out += urlEncode(k) + "=" + urlEncode(v);
    }
    return out;
}

// Howard Hinnant's days_from_civil: avoids timegm(), which newlib lacks.
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t parseIsoDate(const std::string& s) {
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
    if (y < 1971) return 0;  // Jellyfin uses 0001-01-01 for "never"
    return daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
}

std::string formatIsoDate(int64_t t) {
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    // civil_from_days
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02d:%02d.0000000Z", static_cast<int>(y + (m <= 2)), m, d,
                  static_cast<int>(rem / 3600), static_cast<int>((rem % 3600) / 60), static_cast<int>(rem % 60));
    return buf;
}

std::string formatDuration(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    char buf[32];
    if (seconds >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", static_cast<int>(seconds / 3600),
                      static_cast<int>((seconds / 60) % 60), static_cast<int>(seconds % 60));
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", static_cast<int>(seconds / 60), static_cast<int>(seconds % 60));
    return buf;
}

std::string formatSize(int64_t bytes) {
    char buf[32];
    if (bytes >= 1024LL * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    else
        std::snprintf(buf, sizeof(buf), "%d KB", static_cast<int>(bytes / 1024));
    return buf;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::remove(path, ec);  // rename over an existing file fails on the 3DS FAT driver
    fs::rename(tmp, path, ec);
    return !ec;
}

bool makeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    return fs::is_directory(path, ec);
}

bool removeAll(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

int64_t fileSize(const std::string& path) {
    std::error_code ec;
    auto s = fs::file_size(path, ec);
    return ec ? 0 : static_cast<int64_t>(s);
}

bool loadJson(const std::string& path, nlohmann::json& out) {
    std::string data;
    if (!readFile(path, data)) {
        // A crash between remove and rename in writeFileAtomic leaves only the .tmp copy.
        if (!readFile(path + ".tmp", data)) return false;
    }
    out = nlohmann::json::parse(data, nullptr, false);
    return !out.is_discarded();
}

bool saveJson(const std::string& path, const nlohmann::json& j) { return writeFileAtomic(path, j.dump(1)); }

std::string normalizeServerUrl(std::string url) {
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) url.pop_back();
    while (!url.empty() && url.front() == ' ') url.erase(url.begin());
    if (!url.empty() && url.find("://") == std::string::npos) url = "http://" + url;
    return url;
}

std::string randomHex(size_t length) {
    static std::mt19937_64 rng(static_cast<uint64_t>(systemClock()) ^
                               static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&length)));
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < length; ++i) out.push_back(hex[rng() & 15]);
    return out;
}

}  // namespace jelly
