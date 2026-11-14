#include "versions.h"

#include "config.h"
#include "fs.h"
#include "manifest.h"
#include "util.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <fstream>
#include <unordered_set>

#pragma comment(lib, "shell32.lib")

namespace voxel {
namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) {
        if (p) CoTaskMemFree(p);
        return std::wstring();
    }
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}

bool UnsafeVersionId(const std::wstring& id) {
    if (id.empty() || id == L"." || id == L"..") return true;
    for (wchar_t ch : id) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
            ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' ||
            ch == L'\r' || ch == L'\n') {
            return true;
        }
    }
    return false;
}

// 从版本 id 反推加载器，够用就好：真正权威的信息要解析 inheritsFrom
std::string InferLoader(const std::wstring& id) {
    std::string lower = WideToUtf8(id);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    if (lower.find("neoforge") != std::string::npos) return "neoforge";
    if (lower.find("forge") != std::string::npos) return "forge";
    if (lower.find("quilt") != std::string::npos) return "quilt";
    if (lower.find("fabric") != std::string::npos) return "fabric";
    return "vanilla";
}

// 从 id 里剥离加载器后缀拿原版号：1.21.1-fabric-0.20.1 → 1.21.1
std::string InferBaseVersion(const std::wstring& id) {
    std::string text = WideToUtf8(id);
    const char* markers[] = {"-fabric", "-quilt", "-neoforge", "-forge"};
    for (const char* m : markers) {
        size_t pos = text.find(m);
        if (pos != std::string::npos && pos > 0) return text.substr(0, pos);
    }
    return text;
}

nlohmann::json ScanLocalVersions(const std::wstring& gameDir) {
    nlohmann::json list = nlohmann::json::array();
    const std::wstring dir = fs::Join(gameDir, L"versions");

    for (const std::wstring& name : fs::ListDirs(dir)) {
        if (UnsafeVersionId(name)) continue;

        const std::wstring jsonPath = fs::Join(fs::Join(dir, name), name + L".json");
        if (!fs::Exists(jsonPath)) continue;

        nlohmann::json manifest =
            nlohmann::json::parse(fs::ReadText(jsonPath), nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object()) continue;

        std::string type = "release";
        if (manifest.value("type", std::string()) == "snapshot") type = "snapshot";

        std::string releaseTime;
        if (manifest.contains("releaseTime") && manifest["releaseTime"].is_string()) {
            releaseTime = manifest["releaseTime"].get<std::string>();
        } else if (manifest.contains("time") && manifest["time"].is_string()) {
            releaseTime = manifest["time"].get<std::string>();
        }

        nlohmann::json entry = {
            {"id", WideToUtf8(name)},
            {"type", type},
            {"releaseTime", releaseTime.empty() ? nlohmann::json(nullptr) : nlohmann::json(releaseTime)},
            {"installed", true},
            {"local", true},
            {"baseVersion", InferBaseVersion(name)},
            {"loader", InferLoader(name)},
        };

        // inheritsFrom 存在时，官方父版本号就是最可靠的依据
        if (manifest.contains("inheritsFrom") && manifest["inheritsFrom"].is_string()) {
            entry["baseVersion"] = manifest["inheritsFrom"].get<std::string>();
            entry["loader"] = InferLoader(name);
        }
        list.push_back(entry);
    }

    // 新版在前：旧版 UI 的版本库页就是这么排的
    std::sort(list.begin(), list.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("releaseTime", std::string()) > b.value("releaseTime", std::string());
    });
    return list;
}

}  // namespace

std::wstring ResolveGameDir(const nlohmann::json& settings) {
    const std::string custom = settings.value("gameDir", std::string());
    if (!custom.empty()) return Utf8ToWide(custom);
    const std::wstring roaming = KnownFolder(FOLDERID_RoamingAppData);
    if (roaming.empty()) return L".minecraft";
    return fs::Join(roaming, L".minecraft");
}

nlohmann::json ListVersions(const nlohmann::json& settings, bool force) {
    const std::wstring gameDir = ResolveGameDir(settings);
    nlohmann::json local = ScanLocalVersions(gameDir);

    std::unordered_set<std::string> installedIds;
    for (const auto& entry : local) installedIds.insert(entry.value("id", std::string()));

    const GameManifest manifest = FetchGameManifest(settings, force);

    nlohmann::json official = nlohmann::json::array();
    std::unordered_set<std::string> officialIds;
    for (const auto& version : manifest.versions) {
        if (version.id.empty()) continue;
        officialIds.insert(version.id);
        official.push_back(nlohmann::json{
            {"id", version.id},
            {"type", version.type.empty() ? "release" : version.type},
            {"releaseTime", version.releaseTime.empty() ? nlohmann::json(nullptr)
                                                        : nlohmann::json(version.releaseTime)},
            {"installed", installedIds.count(version.id) > 0},
            {"local", false},
        });
    }

    // 加载器实例（1.21.1-fabric-…）不会出现在官方清单里，单独排在前面；
    // 与官方同名的本地版本交给官方条目 + installed 标记，避免列表里出现两份
    nlohmann::json localOnly = nlohmann::json::array();
    for (const auto& entry : local) {
        if (officialIds.count(entry.value("id", std::string())) == 0) localOnly.push_back(entry);
    }

    nlohmann::json all = nlohmann::json::array();
    for (const auto& entry : localOnly) all.push_back(entry);
    for (const auto& entry : official) all.push_back(entry);

    return nlohmann::json{
        {"latest", {{"release", manifest.latestRelease}, {"snapshot", manifest.latestSnapshot}}},
        {"versions", all},
        {"source", manifest.source},
        {"error", manifest.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(manifest.error)},
    };
}

}  // namespace voxel
