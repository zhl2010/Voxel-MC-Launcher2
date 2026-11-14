#include "auth.h"

#include "config.h"
#include "crypto.h"
#include "fs.h"
#include "util.h"

#include <fstream>

namespace voxel {
namespace {

std::wstring SessionFile() { return fs::Join(UserDataDir(), L"account-session.json"); }

std::wstring SkinFile() {
    // 皮肤跟随当前账户目录：登出后不再显示别人的皮肤
    Account current;
    if (!LoadAccount(current) || current.uuid.empty()) return std::wstring();
    return fs::Join(fs::Join(UserDataDir(), L"skins"), Utf8ToWide(current.uuid) + L".png");
}

}  // namespace

bool ValidOfflineName(const std::string& name) {
    if (name.size() < 3 || name.size() > 16) return false;
    for (char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok) return false;
    }
    return true;
}

nlohmann::json Account::ToJson() const {
    nlohmann::json account = {
        {"name", name},
        {"uuid", uuid},
        {"accessToken", accessToken.empty() ? "0" : accessToken},
        {"clientToken", ""},
        {"userProperties", userProperties.is_object() ? userProperties : nlohmann::json::object()},
        {"type", type.empty() ? "crack" : type},
        {"meta", {{"online", type == "msa" || type == "yggdrasil"},
                  {"type", type.empty() ? "crack" : type}}},
    };
    if (!server.empty()) account["server"] = server;
    return account;
}

Account MakeOfflineAccount(const std::string& name) {
    Account account;
    account.name = name;
    account.uuid = crypto::OfflineUuid(name);
    account.accessToken = "0";
    account.type = "crack";
    account.userProperties = nlohmann::json::object();
    return account;
}

bool LoadAccount(Account& out) {
    const std::string raw = fs::ReadText(SessionFile());
    if (raw.empty()) return false;

    nlohmann::json wrapper = nlohmann::json::parse(raw, nullptr, false);
    if (wrapper.is_discarded() || !wrapper.is_object()) return false;

    nlohmann::json payload;
    bool reencrypt = false;
    if (wrapper.contains("payload") && wrapper["payload"].is_string()) {
        const std::string encoded = wrapper["payload"].get<std::string>();
        if (wrapper.value("encrypted", false)) {
            // DPAPI 加密格式（含旧版 Electron safeStorage：同样是 DPAPI，无附加熵时
            // 一般也能解开）。解不开就要求重新登录，不把密文当成空账户。
            std::string plain;
            if (!crypto::UnprotectData(crypto::Base64Decode(encoded), plain)) return false;
            payload = nlohmann::json::parse(plain, nullptr, false);
        } else {
            // 旧版明文格式：读出来后顺手升级成加密存储
            const std::string decoded = crypto::Base64Decode(encoded);
            if (decoded.empty()) return false;
            payload = nlohmann::json::parse(decoded, nullptr, false);
            reencrypt = payload.is_object();
        }
    } else {
        payload = wrapper;  // 兼容直接写成裸账户对象的旧文件
        reencrypt = payload.is_object();
    }
    if (payload.is_discarded() || !payload.is_object()) return false;

    const std::string name = payload.value("name", std::string());
    const std::string type = payload.contains("meta") && payload["meta"].is_object()
                                 ? payload["meta"].value("type", std::string("crack"))
                                 : payload.value("type", std::string("crack"));
    if (name.empty()) return false;

    out.name = name;
    out.uuid = payload.value("uuid", std::string());
    out.accessToken = payload.value("accessToken", std::string("0"));
    out.type = type.empty() ? "crack" : type;
    out.server = payload.value("server", std::string());
    if (payload.contains("userProperties") && payload["userProperties"].is_object()) {
        out.userProperties = payload["userProperties"];
    }
    if (out.uuid.empty() && out.type == "crack") out.uuid = crypto::OfflineUuid(name);
    if (reencrypt) SaveAccount(out);  // 静默升级为 DPAPI 加密
    return true;
}

bool SaveAccount(const Account& account) {
    if (account.name.empty()) return false;
    const std::string plain = account.ToJson().dump();
    std::string blob;
    if (!crypto::ProtectData(plain, blob)) return false;
    nlohmann::json wrapper = {
        {"version", 1},
        {"encrypted", true},
        {"payload", crypto::Base64Encode(blob)},
    };
    return fs::WriteTextAtomic(SessionFile(), wrapper.dump());
}

void ClearAccount() { DeleteFileW(SessionFile().c_str()); }

std::string SkinDataUrl() {
    const std::wstring file = SkinFile();
    if (file.empty() || !fs::Exists(file)) return std::string();
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::string();
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 8) return std::string();
    return "data:image/png;base64," + crypto::Base64Encode(bytes);
}

bool ApplySkin(const std::wstring& pngPath, std::string* error) {
    Account account;
    if (!LoadAccount(account)) {
        if (error) *error = "请先登录离线账户";
        return false;
    }

    std::ifstream in(pngPath, std::ios::binary);
    if (!in) {
        if (error) *error = "无法读取该图片";
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 8 || bytes[0] != '\x89' || bytes[1] != 'P' || bytes[2] != 'N' ||
        bytes[3] != 'G') {
        if (error) *error = "请选择 PNG 格式的皮肤（64×64 或 64×32）";
        return false;
    }
    if (bytes.size() > 4 * 1024 * 1024) {
        if (error) *error = "图片过大（上限 4 MB）";
        return false;
    }

    const std::wstring dir = fs::Join(UserDataDir(), L"skins");
    const std::wstring dest = fs::Join(dir, Utf8ToWide(account.uuid) + L".png");
    if (!fs::WriteText(dest, bytes)) {
        if (error) *error = "皮肤保存失败";
        return false;
    }
    return true;
}

void ClearSkin() {
    const std::wstring file = SkinFile();
    if (file.empty()) return;
    DeleteFileW(file.c_str());
}

namespace {

// 皮肤模型与账户绑定：跟着 uuid 存，换账户互不影响
std::wstring SkinModelFile() {
    Account current;
    if (!LoadAccount(current) || current.uuid.empty()) {
        return fs::Join(UserDataDir(), L"skin-model.txt");
    }
    return fs::Join(fs::Join(UserDataDir(), L"skins"),
                    Utf8ToWide(current.uuid) + L".model");
}

}  // namespace

std::string SkinModel() {
    const std::string raw = fs::ReadText(SkinModelFile());
    std::string trimmed;
    for (char ch : raw) {
        if (ch != '\r' && ch != '\n' && ch != ' ' && ch != '\t') trimmed.push_back(ch);
    }
    return trimmed == "alex" ? "alex" : "steve";
}

bool SetSkinModel(const std::string& model) {
    if (model != "steve" && model != "alex") return false;
    const std::wstring path = SkinModelFile();
    fs::EnsureDir(fs::ParentDir(path));
    return fs::WriteTextAtomic(path, model);
}

nlohmann::json SkinInfo() {
    const std::string data = SkinDataUrl();
    return nlohmann::json{
        {"model", SkinModel()},
        {"custom", !data.empty()},
        {"dataUrl", data},
    };
}

}  // namespace voxel
