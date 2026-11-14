// 版本实例的显示名与图标。
//
// 存储位置：与配置同级的 metadata 目录，按版本 id 分散存放
//   %APPDATA%\Voxel Launcher\version-meta\<sha1(id)>.json   { "displayName": "...", "icon": "blocks/stone" }
//   %APPDATA%\Voxel Launcher\version-meta\<sha1(id)>.png    自定义上传的图标
//
// 为什么不直接改 versions/<id>/<id>.json 的 id 字段：那个字段是 Minecraft 用来
// 解析继承关系的权威标识，改了会破坏 inheritsFrom 链。所以显示名只做覆盖层，
// 版本目录名和 json 一律不动——重命名只是"看起来叫什么"。
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace voxel::vmeta {

struct Meta {
    std::string displayName;  // 空 = 用版本 id
    std::string icon;         // "blocks/<name>" 方块图标，或 "custom" 表示用上传的 png
};

// 读取某版本的显示元数据
Meta Load(const std::string& versionId);

// 合并写入（patch 里缺的字段保持不变）
bool Save(const std::string& versionId, const Meta& meta);

// 列出全部自定义元数据，供 versions_list 一次性带出去
nlohmann::json All();

// 保存自定义图标（PNG 字节）。成功后 meta.icon 变为 "custom"。
bool SaveIcon(const std::string& versionId, const std::string& pngBytes, std::string* error);

// 取自定义图标字节；没有则返回空串
std::string LoadIcon(const std::string& versionId);

}  // namespace voxel::vmeta
