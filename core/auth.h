// 账户：离线登录、会话持久化、本地皮肤。
//
// 持久化沿用旧 Electron 版的 account-session.json 外壳（version/encrypted/payload），
// 写出来的 encrypted=false 是明文 base64 —— 离线账户本来就没有秘密，
// 这样 Electron 版还能把这份会话读回去，两个版本可以共用同一个账户。
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace voxel {

struct Account {
    std::string name;
    std::string uuid;
    std::string accessToken;
    std::string type;  // crack / msa / yggdrasil
    std::string server;
    nlohmann::json userProperties = nlohmann::json::object();

    nlohmann::json ToJson() const;
    bool IsEmpty() const { return name.empty(); }
};

Account MakeOfflineAccount(const std::string& name);

bool LoadAccount(Account& out);
bool SaveAccount(const Account& account);
void ClearAccount();

// 本地皮肤（离线账户）：skins/<uuid>.png
std::string SkinDataUrl();
bool ApplySkin(const std::wstring& pngPath, std::string* error);
void ClearSkin();

// 用户名校验：3–16 位字母/数字/下划线（与前端一致）
bool ValidOfflineName(const std::string& name);

}  // namespace voxel
