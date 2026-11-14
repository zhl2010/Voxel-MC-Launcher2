// Modrinth 内容下载：模组 / 资源包 / 光影包。
//
// 三者共用 Modrinth 的 v2 API，区别只在 project_type 与落盘目录：
//   mod          → <gameDir>/mods                     或 <version>/mods
//   resourcepack → <gameDir>/resourcepacks            或 <version>/resourcepacks
//   shader       → <gameDir>/shaderpacks              或 <version>/shaderpacks
//
// 关键设计：安装目标由调用方显式指定（versionId 为空 = 当前选择的版本，
// 否则落到该实例自己的目录），不再隐式跟随"当前选中版本"——
// 用户可以直接给某个实例下载模组，而不必先切换过去。
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace voxel::mods {

// 内容类型 → 落盘目录名
std::string DirNameFor(const std::string& projectType);

// 搜索。payload:
//   { query, projectType: mod|resourcepack|shader, gameVersion, loader, limit, offset }
// 返回 { hits: [...], total, query, page, pageSize }
nlohmann::json Search(const nlohmann::json& settings, const nlohmann::json& payload);

// 某项目的可用版本列表。payload: { projectId, gameVersion, loader }
nlohmann::json Versions(const nlohmann::json& settings, const nlohmann::json& payload);

// 安装。payload:
//   { projectId, projectType, versionId, targetVersionId }
// targetVersionId 为空时用启动器当前选择的版本；给出则装进那个实例。
// 返回 { ok, path, fileName, id, projectType, targetVersionId }
nlohmann::json Install(const nlohmann::json& settings, const std::wstring& gameDir,
                       const nlohmann::json& payload);

// 列出某个目标目录下已安装的内容文件名。
nlohmann::json Installed(const std::wstring& gameDir, const std::string& targetVersionId,
                         bool isolated);

// 取项目图标（返回 data URL，避免页面跨域）。payload: { iconUrl }
nlohmann::json Icon(const nlohmann::json& settings, const nlohmann::json& payload);

}  // namespace voxel::mods
