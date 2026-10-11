#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace jelly {

constexpr int64_t TICKS_PER_SECOND = 10000000;

std::string urlEncode(const std::string& s);
std::string buildQuery(const std::vector<std::pair<std::string, std::string>>& params);

/// Parses Jellyfin ISO-8601 dates ("2026-10-11T17:03:12.1234567Z") to unix seconds; 0 on failure.
int64_t parseIsoDate(const std::string& s);
/// Formats unix seconds as "YYYY-MM-DDTHH:MM:SS.0000000Z".
std::string formatIsoDate(int64_t unixSeconds);

/// "1:02:03" / "2:03".
std::string formatDuration(int64_t seconds);
/// "12.3 MB".
std::string formatSize(int64_t bytes);

bool readFile(const std::string& path, std::string& out);
/// Writes via a temporary file + rename so a crash or power-off never leaves a truncated file.
bool writeFileAtomic(const std::string& path, const std::string& data);
bool makeDirs(const std::string& path);
bool removeAll(const std::string& path);
bool fileExists(const std::string& path);
int64_t fileSize(const std::string& path);

bool loadJson(const std::string& path, nlohmann::json& out);
bool saveJson(const std::string& path, const nlohmann::json& j);

/// Removes a trailing '/' from a server URL and adds "http://" when no scheme was typed.
std::string normalizeServerUrl(std::string url);

/// Random hex string (device ids, play session ids).
std::string randomHex(size_t length);

}  // namespace jelly
