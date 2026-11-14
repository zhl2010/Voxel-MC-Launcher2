#include "vmeta.h"

#include "config.h"
#include "crypto.h"
#include "fs.h"
#include "installer.h"
#include "util.h"

#include <atomic>
#include <map>

namespace voxel::vmeta {
namespace {

std::wstring MetaDir() {
    static std::wstring dir;
    static std::atomic<bool> ready{false};
    if (!ready.load()) {
        dir = fs::Join(UserDataDir(), L"version-meta");
        fs::EnsureDir(dir);
        ready = true;
    }
    return dir;
}

// 版本 id 不能直接当文件名（可能含路径分隔符等），用 sha1 收敛成定长安全名
std::wstring BaseNameFor(const std::string& versionId) {
    return Utf8ToWide(crypto::Sha1Hex(versionId));
}

std::wstring JsonPath(const std::string& versionId) {
    return fs::Join(MetaDir(), BaseNameFor(versionId) + L".json");
}

std::wstring IconPath(const std::string& versionId) {
    return fs::Join(MetaDir(), BaseNameFor(versionId) + L".png");
}

}  // namespace

Meta Load(const std::string& versionId) {
    Meta meta;
    const std::string raw = fs::ReadText(JsonPath(versionId));
    if (raw.empty()) return meta;
    nlohmann::json doc = nlohmann::json::parse(raw, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return meta;
    meta.displayName = doc.value("displayName", std::string());
    meta.icon = doc.value("icon", std::string());
    return meta;
}

bool Save(const std::string& versionId, const Meta& meta) {
    if (!SafeVersionId(versionId)) return false;
    const std::wstring dir = MetaDir();
    if (!fs::EnsureDir(dir)) return false;
    const nlohmann::json doc = {
        {"versionId", versionId},
        {"displayName", meta.displayName},
        {"icon", meta.icon},
    };
    return fs::WriteTextAtomic(JsonPath(versionId), doc.dump(2));
}

nlohmann::json All() {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& path : fs::ListFiles(MetaDir(), false)) {
        if (fs::BaseName(path).size() < 5) continue;
        if (path.substr(path.size() - 5) != L".json") continue;
        const std::string raw = fs::ReadText(path);
        if (raw.empty()) continue;
        nlohmann::json doc = nlohmann::json::parse(raw, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) continue;
        const std::string id = doc.value("versionId", std::string());
        if (id.empty()) continue;
        out[id] = {{"displayName", doc.value("displayName", std::string())},
                   {"icon", doc.value("icon", std::string())}};
    }
    return out;
}

bool SaveIcon(const std::string& versionId, const std::string& pngBytes, std::string* error) {
    if (!SafeVersionId(versionId)) {
        if (error) *error = "版本名非法";
        return false;
    }
    if (pngBytes.size() < 8 || static_cast<unsigned char>(pngBytes[0]) != 0x89 ||
        pngBytes[1] != 'P' || pngBytes[2] != 'N' || pngBytes[3] != 'G') {
        if (error) *error = "请选择 PNG 格式的图片";
        return false;
    }
    if (pngBytes.size() > 4 * 1024 * 1024) {
        if (error) *error = "图片过大（上限 4 MB）";
        return false;
    }
    if (!fs::EnsureDir(MetaDir())) {
        if (error) *error = "无法创建元数据目录";
        return false;
    }
    if (!fs::WriteText(IconPath(versionId), pngBytes)) {
        if (error) *error = "图标保存失败";
        return false;
    }
    Meta meta = Load(versionId);
    meta.icon = "custom";
    if (!Save(versionId, meta)) {
        if (error) *error = "图标记录写入失败";
        return false;
    }
    return true;
}

std::string LoadIcon(const std::string& versionId) {
    const std::wstring path = IconPath(versionId);
    if (!fs::Exists(path)) return std::string();
    return fs::ReadText(path);
}

}  // namespace voxel::vmeta
