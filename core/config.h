// 配置存取
//
// 刻意复用旧 Electron 版的数据目录 %APPDATA%\Voxel Launcher：
// 用户已有的 launcher-config.json 直接生效，不需要迁移向导，也不会出现
// "装完新版发现版本全丢了" 的观感。

#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace voxel {

// %APPDATA%\Voxel Launcher
std::wstring UserDataDir();

// %APPDATA%\Voxel Launcher\launcher-config.json
std::wstring ConfigFile();

// 与旧 config-store.js 的 DEFAULT_SETTINGS 逐字段一致
nlohmann::json DefaultSettings();

// 读配置；文件缺失或损坏时返回默认值（绝不因为配置坏了就起不来）
nlohmann::json LoadSettings();

// 原子写入（先写 .tmp 再替换），避免断电留下半个文件
bool SaveSettings(const nlohmann::json& settings);

// 只保留合法键，并把读到的值覆盖到 base 上
nlohmann::json MergeAllowed(const nlohmann::json& patch, const nlohmann::json& base);

}  // namespace voxel
