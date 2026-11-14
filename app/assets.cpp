#include "assets.h"

#include <windows.h>

namespace {

// ID 必须与 resources.rc 保持一致。
struct AssetEntry {
    int id;
    const wchar_t* name;
    const wchar_t* mime;
};

const AssetEntry kAssets[] = {
    {101, L"index.html", L"text/html; charset=utf-8"},
    {102, L"styles.css", L"text/css; charset=utf-8"},
    {103, L"app.js", L"application/javascript; charset=utf-8"},
    {104, L"voxel-api.js", L"application/javascript; charset=utf-8"},
    // 应用图标：界面里当 logo 用，和 exe 图标是同一张图
    {105, L"icon.png", L"image/png"},
};

bool LoadResourceBytes(int id, std::vector<unsigned char>& data) {
    // RT_RCDATA == 10；这里用整数避免某些 SDK 头把宏定义成字符串
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!res) return false;

    HGLOBAL loaded = LoadResource(nullptr, res);
    if (!loaded) return false;

    const DWORD size = SizeofResource(nullptr, res);
    const void* bytes = LockResource(loaded);
    if (!bytes || size == 0) return false;

    const auto* begin = static_cast<const unsigned char*>(bytes);
    data.assign(begin, begin + size);
    return true;
}

}  // namespace

bool EmbeddedAsset(const std::wstring& name, std::vector<unsigned char>& data, std::wstring& mime) {
    for (const auto& entry : kAssets) {
        if (name != entry.name) continue;
        if (!LoadResourceBytes(entry.id, data)) return false;
        mime = entry.mime;
        return true;
    }
    return false;
}

bool HasEmbeddedAssets() {
    std::vector<unsigned char> data;
    std::wstring mime;
    return EmbeddedAsset(L"index.html", data, mime);
}
