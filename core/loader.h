// 模组加载器：元数据查询与独立实例安装。
//
// 两个命令对应旧 preload 的契约：
//   loaders_list   → { mcVersion, versionType } → { vanilla, fabric, forge, neoforge }
//   loader_install → { gameVersion, loader, loaderVersion }
//                  → { id, alreadyInstalled, loaderVersion, librariesDownloaded }
//
// 安装模型与 PCL / HMCL 一致：加载器不进原版本，而是派生一个独立版本目录
// `<gameDir>/versions/<mc>-<loader>-<loaderVersion>`，其 json 用 inheritsFrom
// 指回原版。这样同一个原版可以同时装多个加载器互不干扰。
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace voxel::loader {

// 每个加载器的可用版本列表。字段与前端 getLoaderOptions() 消费的形状一致：
//   { type, label, version, detail, available }
// 只返回前三项 + 可用性；跨加载器的汇总由调用方组装。
struct LoaderOption {
    std::string type;
    std::string label;
    std::string latestVersion;
    std::string detail;
    bool available = false;

    nlohmann::json ToJson() const {
        return nlohmann::json{{"type", type},
                              {"label", label},
                              {"version", latestVersion},
                              {"detail", detail},
                              {"available", available}};
    }
};

// 查询某个 Minecraft 版本可用的加载器。网络失败时各项 available=false 并附带 detail 说明，
// 本函数不抛异常——前端只把这里当"建议性"数据，超时会退回纯原版。
nlohmann::json ListLoaders(const nlohmann::json& settings, const std::string& mcVersion);

// 安装加载器为独立版本。成功返回前端需要的字段，失败抛 std::runtime_error。
// customName 非空时用作实例目录名——同一游戏版本 + 同一加载器要装多份时靠它区分；
// 为空则用默认的 <mc>-<loader>-<版本>。
nlohmann::json InstallLoader(const nlohmann::json& settings, const std::wstring& gameDir,
                             const std::string& mcVersion, const std::string& loader,
                             const std::string& loaderVersion, const std::string& customName);

}  // namespace voxel::loader
