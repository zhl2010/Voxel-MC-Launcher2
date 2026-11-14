#include "manifest.h"

#include "config.h"
#include "fs.h"
#include "util.h"

#include "../net/http.h"

#include <mutex>

namespace voxel {
namespace {

constexpr wchar_t kManifestUrl[] =
    L"https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
constexpr long long kCacheTtlMs = 10 * 60 * 1000;   // 内存缓存 10 分钟
constexpr long long kDiskTtlMs = 6 * 60 * 60 * 1000;  // 磁盘缓存只用于"网络不可用"兜底

// 内置兜底列表：完全没有网络也没有缓存时，版本页至少不是空的
const char* kFallbackVersions[] = {
    "26.3", "1.21.8", "1.21.4", "1.21.1", "1.20.6", "1.20.1",
    "1.19.4", "1.18.2", "1.16.5", "1.12.2", "1.8.9"};

struct CacheState {
    nlohmann::json raw;          // 完整清单（含 latest / versions）
    long long fetchedAt = 0;
    bool hasMemory = false;
};

CacheState g_cache;
std::mutex g_mutex;

std::wstring CacheFile() { return fs::Join(UserDataDir(), L"version-manifest-cache.json"); }

long long NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    LARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = static_cast<LONG>(ft.dwHighDateTime);
    return value.QuadPart / 10000;  // 100ns → ms
}

// 解析清单并保留 url —— 下载单个版本 JSON 时必须靠它
GameManifest Parse(const nlohmann::json& raw, const std::string& source,
                   const std::string& error = std::string()) {
    GameManifest result;
    result.source = source;
    result.error = error;
    if (raw.contains("latest") && raw["latest"].is_object()) {
        result.latestRelease = raw["latest"].value("release", std::string());
        result.latestSnapshot = raw["latest"].value("snapshot", std::string());
    }
    if (raw.contains("versions") && raw["versions"].is_array()) {
        for (const auto& entry : raw["versions"]) {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string()) continue;
            ManifestVersion version;
            version.id = entry["id"].get<std::string>();
            version.type = entry.value("type", std::string("release"));
            version.releaseTime = entry.value("releaseTime", std::string());
            version.url = entry.value("url", std::string());
            result.versions.push_back(version);
        }
    }
    return result;
}

nlohmann::json LoadDiskCache() {
    const std::string raw = fs::ReadText(CacheFile());
    if (raw.empty()) return nlohmann::json();
    nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return nlohmann::json();
    if (!parsed.contains("versions") || !parsed["versions"].is_array()) return nlohmann::json();
    return parsed;
}

void SaveDiskCache(const nlohmann::json& manifest) {
    nlohmann::json payload;
    payload["latest"] = manifest.contains("latest") ? manifest["latest"] : nlohmann::json::object();
    payload["versions"] = nlohmann::json::array();
    if (manifest.contains("versions") && manifest["versions"].is_array()) {
        for (const auto& entry : manifest["versions"]) {
            if (!entry.is_object()) continue;
            payload["versions"].push_back(nlohmann::json{
                {"id", entry.value("id", std::string())},
                {"type", entry.value("type", std::string())},
                {"releaseTime", entry.value("releaseTime", std::string())},
                {"url", entry.value("url", std::string())},
            });
        }
    }
    payload["savedAt"] = NowMs();
    fs::WriteTextAtomic(CacheFile(), payload.dump());
}

nlohmann::json FallbackManifest() {
    nlohmann::json latest = {{"release", kFallbackVersions[0]}, {"snapshot", kFallbackVersions[0]}};
    nlohmann::json versions = nlohmann::json::array();
    for (const char* id : kFallbackVersions) {
        versions.push_back(nlohmann::json{{"id", id}, {"type", "release"}, {"releaseTime", ""},
                                          {"url", ""}});
    }
    return nlohmann::json{{"latest", latest}, {"versions", versions}};
}

}  // namespace

GameManifest FetchGameManifest(const nlohmann::json& settings, bool force) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!force && g_cache.hasMemory && NowMs() - g_cache.fetchedAt < kCacheTtlMs) {
            return Parse(g_cache.raw, "memory");
        }
    }

    std::string body;
    std::wstring netError;
    if (net::GetText(kManifestUrl, settings, body, &netError)) {
        nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("versions") &&
            parsed["versions"].is_array() && parsed.contains("latest") &&
            parsed["latest"].is_object() && parsed["latest"].contains("release")) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_cache.raw = parsed;
                g_cache.fetchedAt = NowMs();
                g_cache.hasMemory = true;
            }
            SaveDiskCache(parsed);
            return Parse(parsed, "network");
        }
        netError = L"版本清单格式无效";
    }

    const std::string message = WideToUtf8(netError.empty() ? L"网络请求失败" : netError);

    // 网络失败：用磁盘缓存（旧版缓存也认，格式一致）
    nlohmann::json disk = LoadDiskCache();
    if (!disk.is_null()) {
        GameManifest result = Parse(disk, "cache", message);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cache.raw = disk;
        g_cache.fetchedAt = NowMs() - kCacheTtlMs + 60000;  // 一分钟后允许重试
        g_cache.hasMemory = true;
        return result;
    }

    // 连缓存都没有：内置列表，保证 UI 能站起来
    return Parse(FallbackManifest(), "fallback", message);
}

nlohmann::json FetchVersionJson(const nlohmann::json& settings, const std::string& id,
                                std::wstring* error) {
    // 先查内存/磁盘/网络清单，找到该版本的元数据地址
    nlohmann::json manifest;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_cache.hasMemory) manifest = g_cache.raw;
    }
    if (manifest.is_null() || !manifest.contains("versions")) {
        const GameManifest fetched = FetchGameManifest(settings, false);
        manifest = nlohmann::json{{"latest", {{"release", fetched.latestRelease},
                                              {"snapshot", fetched.latestSnapshot}}},
                                  {"versions", nlohmann::json::array()}};
        for (const auto& v : fetched.versions) {
            manifest["versions"].push_back(
                nlohmann::json{{"id", v.id}, {"type", v.type}, {"url", v.url}});
        }
    }

    std::string url;
    if (manifest.contains("versions") && manifest["versions"].is_array()) {
        for (const auto& entry : manifest["versions"]) {
            if (entry.value("id", std::string()) == id && entry.contains("url")) {
                url = entry.value("url", std::string());
                break;
            }
        }
    }
    if (url.empty()) {
        if (error) *error = L"版本清单中没有 " + Utf8ToWide(id);
        return nlohmann::json();
    }

    std::string body;
    std::wstring netError;
    if (!net::GetText(Utf8ToWide(url), settings, body, &netError)) {
        if (error) *error = netError;
        return nlohmann::json();
    }
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (error) *error = L"版本 JSON 解析失败：" + Utf8ToWide(id);
        return nlohmann::json();
    }
    return parsed;
}

}  // namespace voxel
