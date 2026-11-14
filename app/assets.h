// 内嵌前端资源的查表接口。
//
// 资源由 resources.rc 编进 exe；这里只负责"按文件名取字节 + 给 MIME"，
// 不碰 WebView2，便于单独测试。
#pragma once

#include <string>
#include <vector>

// 按文件名（相对路径，如 "styles.css"）取内嵌资源。
// 找到返回 true，data/mime 被填充；找不到返回 false。
bool EmbeddedAsset(const std::wstring& name, std::vector<unsigned char>& data, std::wstring& mime);

// 是否至少有一个资源可用（用于判断要不要退回"读磁盘 src/ 目录"的老路径）。
bool HasEmbeddedAssets();
