#include "loader.h"

#include "fs.h"
#include "installer.h"
#include "util.h"

#include "../net/http.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxel::loader {
namespace {

// Fabric：官方 meta 端点给出 game 版本对应的 loader 列表
constexpr wchar_t kFabricGame[] =
    L"https://meta.fabricmc.net/v2/versions/loader/{mc}";
constexpr wchar_t kFabricProfile[] =
    L"https://meta.fabricmc.net/v2/versions/loader/{mc}/{loader}/profile/json";

// Quilt 与 Fabric 同构，先不接入（前端只展示四种加载器）

std::wstring ReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return text;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

// 从 JSON 数组里取第一项的某个字符串字段
std::string FirstField(const nlohmann::json& array, const char* key) {
    if (!array.is_array() || array.empty()) return std::string();
    const auto& item = array.front();
    if (!item.is_object() || !item.contains(key) || !item[key].is_string()) return std::string();
    return item[key].get<std::string>();
}

// 解析 Fabric 的 loader 列表：数组元素形如
//   { "loader": { "version": "0.16.9", "stable": true }, "intermediary": {...}, "launcherMeta": {...} }
std::vector<std::string> FabricLoaderVersions(const nlohmann::json& list, bool stableOnly) {
    std::vector<std::string> out;
    if (!list.is_array()) return out;
    for (const auto& entry : list) {
        if (!entry.is_object() || !entry.contains("loader") || !entry["loader"].is_object()) continue;
        const auto& loader = entry["loader"];
        const std::string version = loader.value("version", std::string());
        if (version.empty()) continue;
        if (stableOnly && !loader.value("stable", false)) continue;
        out.push_back(version);
    }
    return out;
}

// 把加载器 profile 的 id 改成我们要用的实例名，并补上 inheritsFrom。
//
// 官方 profile 的 id 通常是 "fabric-loader-0.16.9-1.21.1"，我们用更易读的
// "<mc>-fabric-<loader>"；同时保留 profile 自带的 libraries 与 mainClass，
// 原版内容由 inheritsFrom 在解析阶段合并，不必复制一份。
nlohmann::json RewriteProfile(nlohmann::json profile, const std::string& instanceId,
                              const std::string& mcVersion) {
    profile["id"] = instanceId;
    profile["inheritsFrom"] = mcVersion;
    if (!profile.contains("type") || !profile["type"].is_string()) profile["type"] = "release";
    if (!profile.contains("libraries") || !profile["libraries"].is_array()) {
        profile["libraries"] = nlohmann::json::array();
    }
    return profile;
}

std::string InstanceIdFor(const std::string& mc, const std::string& loader,
                          const std::string& loaderVersion) {
    return mc + "-" + loader + "-" + loaderVersion;
}

std::wstring VersionDir(const std::wstring& gameDir, const std::string& id) {
    return fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(id));
}

// 写入 <versions>/<id>/<id>.json，返回实例 id
std::string WriteInstance(const std::wstring& gameDir, const std::string& id,
                          const nlohmann::json& profileJson) {
    if (!SafeVersionId(id)) throw std::runtime_error("生成的版本名非法：" + id);
    const std::wstring dir = VersionDir(gameDir, id);
    if (!fs::EnsureDir(dir)) throw std::runtime_error("无法创建版本目录：" + WideToUtf8(dir));
    const std::wstring file = fs::Join(dir, Utf8ToWide(id) + L".json");
    if (!fs::WriteTextAtomic(file, profileJson.dump(2))) {
        throw std::runtime_error("写入版本 JSON 失败：" + WideToUtf8(file));
    }
    return id;
}

// Fabric 安装：先查列表确认版本存在，再取 profile/json 落地
nlohmann::json InstallFabric(const nlohmann::json& settings, const std::wstring& gameDir,
                             const std::string& mc, std::string loaderVersion) {
    std::wstring error;
    std::string body;

    const std::wstring listUrl = ReplaceAll(kFabricGame, L"{mc}", Utf8ToWide(mc));
    if (!net::GetText(listUrl, settings, body, &error)) {
        throw std::runtime_error("无法获取 Fabric 版本列表" + (error.empty() ? "" : "：" + WideToUtf8(error)));
    }
    nlohmann::json list = nlohmann::json::parse(body, nullptr, false);
    if (list.is_discarded()) throw std::runtime_error("Fabric 版本列表格式异常");

    // 未指定版本就用最新的稳定版
    if (loaderVersion.empty()) {
        auto stable = FabricLoaderVersions(list, true);
        if (stable.empty()) stable = FabricLoaderVersions(list, false);
        if (stable.empty()) throw std::runtime_error("该 Minecraft 版本没有可用的 Fabric 加载器");
        loaderVersion = stable.front();
    } else {
        const auto all = FabricLoaderVersions(list, false);
        if (std::find(all.begin(), all.end(), loaderVersion) == all.end()) {
            throw std::runtime_error("Fabric " + loaderVersion + " 不支持 Minecraft " + mc);
        }
    }

    const std::wstring profileUrl = ReplaceAll(
        ReplaceAll(kFabricProfile, L"{mc}", Utf8ToWide(mc)), L"{loader}", Utf8ToWide(loaderVersion));
    body.clear();
    if (!net::GetText(profileUrl, settings, body, &error)) {
        throw std::runtime_error("无法获取 Fabric 安装配置" +
                                 (error.empty() ? "" : "：" + WideToUtf8(error)));
    }
    nlohmann::json profile = nlohmann::json::parse(body, nullptr, false);
    if (profile.is_discarded() || !profile.is_object()) {
        throw std::runtime_error("Fabric 安装配置格式异常");
    }

    const std::string id = InstanceIdFor(mc, "fabric", loaderVersion);
    const int libraries = profile.contains("libraries") && profile["libraries"].is_array()
                              ? static_cast<int>(profile["libraries"].size())
                              : 0;
    WriteInstance(gameDir, id, RewriteProfile(profile, id, mc));

    return nlohmann::json{{"id", id},
                          {"loaderVersion", loaderVersion},
                          {"librariesDownloaded", libraries}};
}

}  // namespace

nlohmann::json ListLoaders(const nlohmann::json& settings, const std::string& mcVersion) {
    nlohmann::json result = nlohmann::json::object();

    // 原版始终可用
    result["vanilla"] = LoaderOption{"vanilla", "原版", mcVersion, "纯净的 Minecraft 体验", true}.ToJson();

    // Fabric：查得到的第一个稳定版就是"最新"
    LoaderOption fabric{"fabric", "Fabric", "", "轻量、更新快的模组加载器", false};
    if (!mcVersion.empty()) {
        std::wstring error;
        std::string body;
        const std::wstring url = ReplaceAll(kFabricGame, L"{mc}", Utf8ToWide(mcVersion));
        if (net::GetText(url, settings, body, &error)) {
            nlohmann::json list = nlohmann::json::parse(body, nullptr, false);
            auto stable = FabricLoaderVersions(list, true);
            if (stable.empty()) stable = FabricLoaderVersions(list, false);
            if (!stable.empty()) {
                fabric.latestVersion = stable.front();
                fabric.detail = "最新 " + fabric.latestVersion;
                fabric.available = true;
            } else {
                fabric.detail = "该版本暂不支持 Fabric";
            }
        } else {
            fabric.detail = "无法获取 Fabric 版本，请检查网络或镜像设置";
        }
    } else {
        fabric.detail = "请先选择 Minecraft 版本";
    }
    result["fabric"] = fabric.ToJson();

    // Forge / NeoForge：相关 Maven 端点没有稳定的公共元数据接口，
    // 这里如实标记为不可用，而不是给一个点了会失败的按钮。
    result["forge"] = LoaderOption{"forge", "Forge", "", "暂未接入：缺少可用的版本元数据接口", false}
                          .ToJson();
    result["neoforge"] =
        LoaderOption{"neoforge", "NeoForge", "", "暂未接入：缺少可用的版本元数据接口", false}.ToJson();

    return result;
}

nlohmann::json InstallLoader(const nlohmann::json& settings, const std::wstring& gameDir,
                             const std::string& mcVersion, const std::string& loader,
                             const std::string& loaderVersion) {
    if (mcVersion.empty()) throw std::runtime_error("请先选择一个 Minecraft 版本");
    if (!SafeVersionId(mcVersion)) throw std::runtime_error("Minecraft 版本名非法");
    if (loader.empty() || loader == "vanilla") {
        throw std::runtime_error("原版无需安装加载器");
    }

    if (loader == "fabric") {
        // 已经装过同一个组合时不重复下载：先看目标版本目录是否已存在。
        // 指定了版本就直接判；没指定则先把最新稳定版解析出来再判。
        if (!loaderVersion.empty()) {
            const std::string id = InstanceIdFor(mcVersion, "fabric", loaderVersion);
            if (fs::Exists(fs::Join(VersionDir(gameDir, id), Utf8ToWide(id) + L".json"))) {
                return nlohmann::json{{"id", id},
                                      {"loader", "fabric"},
                                      {"loaderVersion", loaderVersion},
                                      {"alreadyInstalled", true},
                                      {"librariesDownloaded", 0}};
            }
        }

        const nlohmann::json installed =
            InstallFabric(settings, gameDir, mcVersion, loaderVersion);
        return nlohmann::json{{"id", installed.value("id", std::string())},
                              {"loader", "fabric"},
                              {"loaderVersion", installed.value("loaderVersion", std::string())},
                              {"alreadyInstalled", false},
                              {"librariesDownloaded", installed.value("librariesDownloaded", 0)}};
    }

    if (loader == "forge" || loader == "neoforge") {
        throw std::runtime_error("Forge / NeoForge 暂未接入：缺少可用的公共版本元数据接口");
    }

    throw std::runtime_error("未知的加载器：" + loader);
}

}  // namespace voxel::loader
