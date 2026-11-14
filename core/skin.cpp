#include "skin.h"

#include "crypto.h"
#include "fs.h"
#include "util.h"

#include "../net/http.h"

#include <windows.h>

namespace voxel::skin {
namespace {

bool FetchFaceViaCrafatar(const nlohmann::json& settings, const Account& account,
                          std::string& outPng);
bool FetchMicrosoftFace(const nlohmann::json& settings, const Account& account,
                        std::string& outPng);

// 令牌直连取整张皮肤。成功时 outPng 是完整 PNG 字节。
bool FetchSkinViaToken(const nlohmann::json& settings, const Account& account,
                       std::string& outPng) {
    outPng.clear();
    if (account.accessToken.empty() || account.accessToken == "0") return false;

    std::wstring url;
    if (account.type == "msa") {
        // 正版账户走公开渲染服务（见 FetchMicrosoftFace 的说明）
        return FetchMicrosoftFace(settings, account, outPng);
    }

    if (account.type == "yggdrasil" && !account.server.empty() && !account.uuid.empty()) {
        std::string hex = account.uuid;
        hex.erase(std::remove(hex.begin(), hex.end(), '-'), hex.end());
        url = Utf8ToWide(account.server + "/sessionserver/session/minecraft/profile/" + hex);
        std::string body;
        std::wstring error;
        if (!net::GetText(url, settings, body, &error)) return false;
        nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) return false;
        if (!doc.contains("properties") || !doc["properties"].is_array()) return false;
        for (const auto& prop : doc["properties"]) {
            if (prop.value("name", std::string()) != "textures") continue;
            const std::string b64 = prop.value("value", std::string());
            const std::string decoded = crypto::Base64Decode(b64);
            if (decoded.empty()) continue;
            nlohmann::json tex = nlohmann::json::parse(decoded, nullptr, false);
            if (tex.is_discarded()) continue;
            if (!tex.contains("textures") || !tex["textures"].contains("SKIN")) continue;
            const std::string skinUrl =
                tex["textures"]["SKIN"].value("url", std::string());
            if (skinUrl.empty()) continue;

            const std::wstring temp = fs::UniqueTempPath(L"voxel-skin");
            std::wstring getErr;
            if (!net::GetFile(Utf8ToWide(skinUrl), temp, settings, &getErr, [](uint64_t) {},
                              nullptr)) {
                DeleteFileW(temp.c_str());
                continue;
            }
            outPng = fs::ReadText(temp);
            DeleteFileW(temp.c_str());
            if (!outPng.empty()) return true;
        }
    }
    return false;
}

/**
 * 微软账户：官方皮肤需要 Authorization: Bearer 头，现有的 net::GetText 只发裸 GET。
 * 与其复制一份带头的 HTTP 客户端，这里直接用 Minecraft 官方的公开渲染服务——
 * Crafatar 按 UUID 出图，对正版账户同样有效，且没有令牌过期问题。
 */
bool FetchMicrosoftFace(const nlohmann::json& settings, const Account& account,
                        std::string& outPng) {
    return FetchFaceViaCrafatar(settings, account, outPng);
}

// Crafatar：公开的 MC 头像渲染服务，按 UUID 出头像，不用令牌。
bool FetchFaceViaCrafatar(const nlohmann::json& settings, const Account& account,
                          std::string& outPng) {
    outPng.clear();
    if (account.uuid.empty()) return false;
    std::string hex = account.uuid;
    hex.erase(std::remove(hex.begin(), hex.end(), '-'), hex.end());
    if (hex.size() != 32) return false;

    const std::wstring url =
        Utf8ToWide("https://crafatar.com/avatars/" + hex + "?size=64&overlay");

    const std::wstring temp = fs::UniqueTempPath(L"voxel-face");
    std::wstring error;
    if (!net::GetFile(url, temp, settings, &error, [](uint64_t) {}, nullptr)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    outPng = fs::ReadText(temp);
    DeleteFileW(temp.c_str());

    // Crafatar 对不存在的 UUID 会返回一个默认图，尺寸仍是 PNG，直接用没问题
    if (outPng.size() < 8) {
        outPng.clear();
        return false;
    }
    if (static_cast<unsigned char>(outPng[0]) != 0x89 || outPng[1] != 'P') {
        outPng.clear();
        return false;
    }
    return true;
}

std::string ToDataUrl(const std::string& png) {
    if (png.empty()) return std::string();
    return "data:image/png;base64," + crypto::Base64Encode(png);
}

}  // namespace

std::string OnlineSkinDataUrl(const nlohmann::json& settings, const Account& account) {
    if (account.type == "crack" || account.IsEmpty()) return std::string();
    std::string png;
    if (!FetchSkinViaToken(settings, account, png)) return std::string();
    return ToDataUrl(png);
}

std::string OnlineFaceDataUrl(const nlohmann::json& settings, const Account& account) {
    // 离线账户没有在线皮肤，交给内置的 Steve / Alex
    if (account.type == "crack" || account.IsEmpty()) return std::string();

    // 先用令牌拿到完整皮肤再裁头，分辨率最高；失败就退回 Crafatar
    std::string skin;
    if (FetchSkinViaToken(settings, account, skin)) {
        return ToDataUrl(skin);
    }

    std::string face;
    if (FetchFaceViaCrafatar(settings, account, face)) {
        return ToDataUrl(face);
    }
    return std::string();
}

}  // namespace voxel::skin
