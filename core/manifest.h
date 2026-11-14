// 官方版本清单（version_manifest_v2.json）：抓取、磁盘缓存、单版本 JSON 解析。
//
// 缓存格式与旧 Electron 版 version-service.js 写的 version-manifest-cache.json 一致，
// 所以从 Electron 版迁移过来的用户第一次打开就有一个可用的离线清单。
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace voxel {

struct ManifestVersion {
    std::string id;
    std::string type;
    std::string releaseTime;
    std::string url;
};

struct GameManifest {
    std::string latestRelease;
    std::string latestSnapshot;
    std::vector<ManifestVersion> versions;
    std::string source;  // network / memory / cache / fallback
    std::string error;
};

// force=false 时命中 10 分钟内存缓存；force=true 强制走网络
GameManifest FetchGameManifest(const nlohmann::json& settings, bool force);

// 按 id 在清单里找 url 并抓取该版本的完整 JSON；失败返回空对象
nlohmann::json FetchVersionJson(const nlohmann::json& settings, const std::string& id,
                                std::wstring* error);

}  // namespace voxel
