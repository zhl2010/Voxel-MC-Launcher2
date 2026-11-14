// 版本解析 + 文件计划 + 并行下载 + natives 解压。
//
// 这一层替代了旧 Electron 版里 eml-lib 的下载职责，但保持同一套判定规则：
//  版本 JSON 的 inheritsFrom 递归合并 → 规则过滤库 → 资源索引 → objects 下载。
// 目录布局完全按官方 .minecraft 规范，所以装出来的目录 PCL/官方启动器也能直接用。
#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace voxel {

struct VersionLevel {
    std::string id;
    nlohmann::json json;
    bool local = false;
};

struct ResolvedVersion {
    std::string id;
    std::string jarId;              // 客户端 jar 所在目录名（原版那一层）
    nlohmann::json manifest;        // 合并后的清单
    std::vector<VersionLevel> levels;  // 父版本在前，自己最后
    std::wstring versionDir;
    int javaMajor = 0;              // manifest.javaVersion.majorVersion
};

struct PlanItem {
    std::wstring dest;
    std::wstring url;
    std::string sha1;
    uint64_t size = 0;
    std::string type;
    bool checkHash = false;
};

struct VersionPlan {
    std::vector<PlanItem> downloads;
    std::vector<std::wstring> nativeJars;
    int present = 0;
    int missing = 0;
    int corrupt = 0;
    uint64_t totalBytes = 0;
    std::vector<std::wstring> notes;
};

using ProgressFn = std::function<void(const nlohmann::json&)>;

bool SafeVersionId(const std::string& id);

// 规则判定（os.name / os.arch / os.version），启动参数与库过滤共用一份实现
bool RulesAllowJson(const nlohmann::json& rules);

// 本地优先，父版本缺失时才联网；失败返回 complete=false 并把原因写进 error
ResolvedVersion ResolveVersion(const std::wstring& gameDir, const nlohmann::json& settings,
                               const std::string& id, std::wstring* error);

// includeAssets=true 时连 assets/objects 一起算（需要索引文件已在本地）
VersionPlan BuildPlan(const ResolvedVersion& version, const nlohmann::json& settings,
                      const std::wstring& gameDir, bool verifyHashes, bool includeAssets);

bool DownloadPlan(const VersionPlan& plan, const nlohmann::json& settings,
                  std::atomic<bool>* cancel, const ProgressFn& progress, std::wstring* error);

bool ExtractNatives(const VersionPlan& plan, const std::wstring& gameDir, const std::string& id,
                    std::vector<std::wstring>* notes, std::atomic<bool>* cancel);

// 全流程：写版本 JSON → 下载核心文件 → 下载资源 → 解压 natives
bool EnsureVersion(const ResolvedVersion& version, const nlohmann::json& settings,
                   const std::wstring& gameDir, std::atomic<bool>* cancel,
                   const ProgressFn& progress, std::wstring* error, VersionPlan* outPlan);

// game_verify 的响应体
nlohmann::json VerifyVersion(const std::wstring& gameDir, const nlohmann::json& settings,
                             const std::string& id, std::atomic<bool>* cancel,
                             const ProgressFn& progress, std::wstring* error);

}  // namespace voxel
