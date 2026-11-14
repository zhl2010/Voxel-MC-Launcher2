// 版本清单：官方版本清单（网络/内存/磁盘缓存/内置兜底）+ 本机实例合并。
//
// 输出结构对齐旧版 version-service.js 的 format()：
//   { latest: {release, snapshot}, versions: [...], source, error, fetchedAt }
// 每条 versions[]: {id, type, releaseTime, installed, local, baseVersion?, loader?}
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace voxel {

// gameDir 为空时回退到 %APPDATA%\.minecraft（与旧版一致）
std::wstring ResolveGameDir(const nlohmann::json& settings);

// versions_list 命令的完整响应体；force=true 时强制刷新官方清单
nlohmann::json ListVersions(const nlohmann::json& settings, bool force);

}  // namespace voxel
