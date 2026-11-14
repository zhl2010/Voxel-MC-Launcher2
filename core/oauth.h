// 第三方登录：Microsoft（Xbox Live）与 Yggdrasil（外置登录，如 LittleSkin）。
//
// 两者握手流程差别很大，共同点是最后都把结果落成 Account 并写进 account-session.json，
// 后续启动游戏走同一条链路（launch.cpp 只认 Account 的 name/uuid/type/accessToken）。
//
// Microsoft 用设备码流程（device code）：
//   1. POST /devicecode 拿到 user_code 与验证地址
//   2. 用户去 microsoft.com/link 输入代码并同意
//   3. 轮询 /token 直到拿到 access_token
//   4. 用 access_token 换 Xbox Live -> XSTS -> Minecraft 令牌
// 这样不需要内嵌浏览器，也不需要处理 redirect_uri。
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

#include "core/auth.h"

namespace voxel::auth {

// 设备码流程的中间结果，用于给前端展示"请去这个网址输入这个码"
struct DeviceCode {
    std::string userCode;
    std::string verifyUrl;
    std::string message;
    int expiresIn = 0;
    int interval = 5;
    bool ok = false;
    std::string error;
};

// 第一步：申请设备码。失败时 ok=false 且 error 有原因。
DeviceCode StartMicrosoftDeviceCode(const nlohmann::json& settings);

// 第二步：带着设备码等待用户完成授权，成功后写出并返回 Account。
// onStatus 用于把"等待中/正在验证"之类的状态回传给界面。
Account CompleteMicrosoftLogin(const nlohmann::json& settings, const DeviceCode& code,
                               const std::function<void(const std::string&)>& onStatus,
                               std::string* error);

// Yggdrasil 外置登录。
//   serverUrl: 认证服务器根地址，如 https://littleskin.cn/api/yggdrasil
//   username / password: 该站点的账号
Account YggdrasilLogin(const nlohmann::json& settings, const std::string& serverUrl,
                       const std::string& username, const std::string& password,
                       std::string* error);

}  // namespace voxel::auth
