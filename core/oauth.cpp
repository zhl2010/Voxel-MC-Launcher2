#include "oauth.h"

#include "auth.h"
#include "crypto.h"
#include "fs.h"
#include "util.h"

#include "../net/http.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace voxel::auth {
namespace {

// 微软官方的 Xbox Live 公共客户端 ID（Minecraft 启动器生态通用）
constexpr char kClientId[] = "00000000402b5328";
constexpr char kScope[] = "XboxLive.signin offline_access";

constexpr wchar_t kDeviceCodeUrl[] =
    L"https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr wchar_t kTokenUrl[] =
    L"https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
constexpr wchar_t kXblUrl[] = L"https://user.auth.xboxlive.com/user/authenticate";
constexpr wchar_t kXstsUrl[] = L"https://xsts.auth.xboxlive.com/xsts/authorize";
constexpr wchar_t kMcLoginUrl[] =
    L"https://api.minecraftservices.com/authentication/login_with_xbox";
constexpr wchar_t kMcProfileUrl[] = L"https://api.minecraftservices.com/minecraft/profile";

std::string UrlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : text) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

bool PostJson(const nlohmann::json& settings, const std::wstring& url, const nlohmann::json& body,
              nlohmann::json& out, std::string* error) {
    // net::GetText 只做 GET；这里用同样可靠的 WinHTTP 路径自己发 POST。
    // 复用它里面的代理选择逻辑不现实，直接构造一个最小的 POST。
    std::string proxyRaw = settings.value("proxyUrl", std::string());
    std::wstring proxy = Utf8ToWide(proxyRaw);
    while (!proxy.empty() && iswspace(proxy.front())) proxy.erase(proxy.begin());
    while (!proxy.empty() && iswspace(proxy.back())) proxy.pop_back();
    if (proxy.rfind(L"://", proxy.size() >= 3 ? proxy.size() - 3 : 0) != std::wstring::npos) {
        const size_t p = proxy.find(L"://");
        if (p != std::wstring::npos) proxy = proxy.substr(p + 3);
    }

    HINTERNET session = WinHttpOpen(
        L"VoxelLauncher/1.2 (Windows)",
        proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(L"VoxelLauncher/1.2 (Windows)", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        if (error) *error = "无法创建网络会话";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 30000);

    // 拆 host / path
    std::wstring rest = url;
    const size_t scheme = rest.find(L"://");
    const bool secure = rest.rfind(L"https", 0) == 0;
    if (scheme != std::wstring::npos) rest = rest.substr(scheme + 3);
    const size_t slash = rest.find(L'/');
    const std::wstring host = slash == std::wstring::npos ? rest : rest.substr(0, slash);
    const std::wstring path = slash == std::wstring::npos ? L"/" : rest.substr(slash);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), secure ? 443 : 80, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        if (error) *error = "无法连接登录服务器";
        return false;
    }

    HINTERNET request =
        WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (error) *error = "请求创建失败";
        return false;
    }

    const std::string payload = body.dump();
    const std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\n";
    bool ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                                 const_cast<char*>(payload.data()),
                                 static_cast<DWORD>(payload.size()),
                                 static_cast<DWORD>(payload.size()), 0) != 0 &&
              WinHttpReceiveResponse(request, nullptr) != 0;

    std::string text;
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
            chunk.resize(read);
            text += chunk;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!ok) {
        if (error) *error = "网络请求失败";
        return false;
    }
    if (status < 200 || status >= 300) {
        // 尽量把服务端的错误描述带出来，比只报状态码有用得多
        const nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        std::string detail = doc.is_object()
                                 ? doc.value("error_description",
                                             doc.value("errorMessage", doc.value("error", "")))
                                 : "";
        if (error) {
            *error = "登录失败（HTTP " + std::to_string(status) + "）" +
                     (detail.empty() ? "" : "：" + detail);
        }
        return false;
    }

    out = nlohmann::json::parse(text, nullptr, false);
    if (out.is_discarded()) {
        if (error) *error = "登录响应格式异常";
        return false;
    }
    return true;
}

bool PostForm(const nlohmann::json& settings, const std::wstring& url, const std::string& form,
              nlohmann::json& out, std::string* error) {
    // OAuth 的 token 端点只接受 form-urlencoded，所以单独走一次
    std::wstring proxy;
    HINTERNET session = WinHttpOpen(L"VoxelLauncher/1.2 (Windows)",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        session = WinHttpOpen(L"VoxelLauncher/1.2 (Windows)", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        if (error) *error = "无法创建网络会话";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 30000);

    HINTERNET connect = WinHttpConnect(session, L"login.microsoftonline.com", 443, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        if (error) *error = "无法连接登录服务器";
        return false;
    }
    const std::wstring path = url.substr(url.find(L"login.microsoftonline.com") +
                                         wcslen(L"login.microsoftonline.com"));
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (error) *error = "请求创建失败";
        return false;
    }

    const std::wstring headers =
        L"Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n";
    bool ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                                 const_cast<char*>(form.data()),
                                 static_cast<DWORD>(form.size()), static_cast<DWORD>(form.size()),
                                 0) != 0 &&
              WinHttpReceiveResponse(request, nullptr) != 0;

    std::string text;
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (ok) {
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0) break;
            chunk.resize(read);
            text += chunk;
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    out = nlohmann::json::parse(text, nullptr, false);
    if (!ok) {
        if (error) *error = "网络请求失败";
        return false;
    }
    if (status < 200 || status >= 300) {
        if (error) {
            const std::string d = out.is_object()
                                      ? out.value("error_description", out.value("error", ""))
                                      : "";
            *error = "令牌获取失败（HTTP " + std::to_string(status) + "）" +
                     (d.empty() ? "" : "：" + d);
        }
        return false;
    }
    return out.is_object();
}

}  // namespace

DeviceCode StartMicrosoftDeviceCode(const nlohmann::json& settings) {
    DeviceCode code;
    nlohmann::json body = {{"client_id", kClientId}, {"scope", kScope}};
    nlohmann::json resp;
    std::string error;
    if (!PostForm(settings, kDeviceCodeUrl,
                  "client_id=" + std::string(kClientId) + "&scope=" + UrlEncode(kScope), resp,
                  &error)) {
        code.error = error;
        return code;
    }

    code.userCode = resp.value("user_code", std::string());
    code.verifyUrl = resp.value("verification_uri", std::string());
    code.message = resp.value("message", std::string());
    code.expiresIn = resp.value("expires_in", 900);
    code.interval = resp.value("interval", 5);
    code.ok = !code.userCode.empty() && !code.verifyUrl.empty();
    if (!code.ok) code.error = "设备码响应缺少必要字段";
    (void)body;
    return code;
}

Account CompleteMicrosoftLogin(const nlohmann::json& settings, const DeviceCode& code,
                               const std::function<void(const std::string&)>& onStatus,
                               std::string* error) {
    if (!code.ok) {
        if (error) *error = code.error.empty() ? "设备码无效" : code.error;
        return Account{};
    }

    // 1) 轮询 token 端点
    const std::string form = "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" +
                             std::string(kClientId) + "&device_code=" + UrlEncode(code.userCode);
    nlohmann::json tokenDoc;
    bool got = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(std::max(60, code.expiresIn));
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(std::max(1, code.interval)));
        std::string pollError;
        if (PostForm(settings, kTokenUrl, form, tokenDoc, &pollError)) {
            got = true;
            break;
        }
        // authorization_pending 是正常状态，继续等
        if (pollError.find("authorization_pending") == std::string::npos &&
            pollError.find("slow_down") == std::string::npos) {
            if (error) *error = pollError;
            return Account{};
        }
        if (onStatus) onStatus("等待授权中…");
    }
    if (!got) {
        if (error) *error = "授权超时，请重新尝试";
        return Account{};
    }

    const std::string msAccess = tokenDoc.value("access_token", std::string());
    if (msAccess.empty()) {
        if (error) *error = "未取到 Microsoft 访问令牌";
        return Account{};
    }

    // 2) Xbox Live
    if (onStatus) onStatus("正在验证 Xbox Live…");
    nlohmann::json xblDoc;
    nlohmann::json xblBody = {
        {"Properties", {{"AuthMethod", "RPS"}, {"SiteName", "user.auth.xboxlive.com"},
                        {"RpsTicket", "d=" + msAccess}}},
        {"RelyingParty", "http://auth.xboxlive.com"},
        {"TokenType", "JWT"}};
    std::string err;
    if (!PostJson(settings, kXblUrl, xblBody, xblDoc, &err)) {
        if (error) *error = "Xbox Live 验证失败：" + err;
        return Account{};
    }
    const std::string xblToken = xblDoc.value("Token", std::string());
    std::string uhs;
    if (xblDoc.contains("DisplayClaims") && xblDoc["DisplayClaims"].contains("xui") &&
        xblDoc["DisplayClaims"]["xui"].is_array() && !xblDoc["DisplayClaims"]["xui"].empty()) {
        uhs = xblDoc["DisplayClaims"]["xui"][0].value("uhs", std::string());
    }
    if (xblToken.empty() || uhs.empty()) {
        if (error) *error = "Xbox Live 响应缺少令牌";
        return Account{};
    }

    // 3) XSTS
    if (onStatus) onStatus("正在换取 XSTS 令牌…");
    nlohmann::json xstsDoc;
    nlohmann::json xstsBody = {
        {"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", nlohmann::json::array({xblToken})}}},
        {"RelyingParty", "rp://api.minecraftservices.com/"},
        {"TokenType", "JWT"}};
    if (!PostJson(settings, kXstsUrl, xstsBody, xstsDoc, &err)) {
        if (error) *error = "XSTS 验证失败：" + err;
        return Account{};
    }
    const std::string xstsToken = xstsDoc.value("Token", std::string());
    if (xstsToken.empty()) {
        if (error) *error = "XSTS 响应缺少令牌";
        return Account{};
    }

    // 4) Minecraft 令牌
    if (onStatus) onStatus("正在登录 Minecraft…");
    nlohmann::json mcDoc;
    const std::string identity = "XBL3.0 x=" + uhs + ";" + xstsToken;
    if (!PostJson(settings, kMcLoginUrl, {{"identityToken", identity}}, mcDoc, &err)) {
        if (error) *error = "Minecraft 登录失败：" + err;
        return Account{};
    }
    const std::string mcToken = mcDoc.value("access_token", std::string());
    if (mcToken.empty()) {
        if (error) *error = "未取到 Minecraft 访问令牌";
        return Account{};
    }

    // 5) 玩家档案（拿到真正的 UUID 与名字）
    if (onStatus) onStatus("正在读取玩家档案…");
    nlohmann::json profile;
    std::string profileErr;
    {
        // GET 请求走现有的 GetText
        std::string body;
        std::wstring getErr;
        if (!net::GetText(kMcProfileUrl, settings, body, &getErr)) {
            if (error) *error = "读取玩家档案失败：" + WideToUtf8(getErr);
            return Account{};
        }
        profile = nlohmann::json::parse(body, nullptr, false);
        if (profile.is_discarded() || !profile.is_object()) {
            if (error) *error = "玩家档案格式异常";
            return Account{};
        }
    }

    Account account;
    account.name = profile.value("name", std::string());
    const std::string rawId = profile.value("id", std::string());
    // Minecraft 的 id 是无连字符的 32 位十六进制，转成标准 UUID 形式
    if (rawId.size() == 32) {
        account.uuid = rawId.substr(0, 8) + "-" + rawId.substr(8, 4) + "-" + rawId.substr(12, 4) +
                       "-" + rawId.substr(16, 4) + "-" + rawId.substr(20, 12);
    } else {
        account.uuid = rawId;
    }
    account.accessToken = mcToken;
    account.type = "msa";
    account.userProperties = nlohmann::json::object();

    if (account.name.empty() || account.uuid.empty()) {
        if (error) *error = "该账号没有 Minecraft 正版档案（需先在 minecraft.net 购买）";
        return Account{};
    }
    if (!SaveAccount(account)) {
        if (error) *error = "账号信息保存失败";
        return Account{};
    }
    return account;
}

Account YggdrasilLogin(const nlohmann::json& settings, const std::string& serverUrl,
                       const std::string& username, const std::string& password,
                       std::string* error) {
    if (username.empty() || password.empty()) {
        if (error) *error = "请填写账号和密码";
        return Account{};
    }

    // 规范化服务器地址：允许用户只填域名，自动补协议与路径
    std::string base = serverUrl;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (base.empty()) base = "https://littleskin.cn/api/yggdrasil";
    if (base.find("://") == std::string::npos) base = "https://" + base;
    if (base.find("/authserver") == std::string::npos) {
        // 大多数站点根路径就是 yggdrasil，authserver 在其下
        if (base.find("/api/yggdrasil") == std::string::npos) base += "/api/yggdrasil";
    }

    const nlohmann::json body = {{"agent", {{"name", "Minecraft"}, {"version", 1}}},
                                 {"username", username},
                                 {"password", password},
                                 {"requestUser", false}};
    const std::wstring url = Utf8ToWide(base + "/authserver/authenticate");

    nlohmann::json doc;
    std::string err;
    if (!PostJson(settings, url, body, doc, &err)) {
        if (error) *error = "外置登录失败：" + err;
        return Account{};
    }

    const std::string token = doc.value("accessToken", std::string());
    if (token.empty()) {
        if (error) *error = "服务器未返回 accessToken";
        return Account{};
    }

    Account account;
    account.accessToken = token;
    account.type = "yggdrasil";
    account.server = base;
    account.userProperties = nlohmann::json::object();
    if (doc.contains("selectedProfile") && doc["selectedProfile"].is_object()) {
        const auto& profile = doc["selectedProfile"];
        account.name = profile.value("name", std::string());
        const std::string rawId = profile.value("id", std::string());
        if (rawId.size() == 32) {
            account.uuid = rawId.substr(0, 8) + "-" + rawId.substr(8, 4) + "-" +
                           rawId.substr(12, 4) + "-" + rawId.substr(16, 4) + "-" +
                           rawId.substr(20, 12);
        } else {
            account.uuid = rawId;
        }
    }
    // 有些站点需要再调 refresh 才有 selectedProfile
    if (account.name.empty()) {
        account.name = username;
        account.uuid = crypto::OfflineUuid(username);
    }

    if (!SaveAccount(account)) {
        if (error) *error = "账号信息保存失败";
        return Account{};
    }
    return account;
}

}  // namespace voxel::auth
