#include "mods.h"

#include "crypto.h"
#include "fs.h"
#include "installer.h"
#include "util.h"
#include "versions.h"

#include "../net/http.h"

#include <windows.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxel::mods {
namespace {

constexpr wchar_t kApi[] = L"https://api.modrinth.com/v2";

std::string UrlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
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

// Modrinth 的 facets 是 JSON 数组的数组，作为查询参数时需要 URL 编码
std::string Facets(const std::vector<std::vector<std::string>>& groups) {
    std::string json = "[";
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g) json += ",";
        json += "[";
        for (size_t i = 0; i < groups[g].size(); ++i) {
            if (i) json += ",";
            json += "\"" + groups[g][i] + "\"";
        }
        json += "]";
    }
    json += "]";
    return UrlEncode(json);
}

nlohmann::json FetchJson(const nlohmann::json& settings, const std::wstring& url,
                         const char* what) {
    std::string body;
    std::wstring error;
    if (!net::GetText(url, settings, body, &error)) {
        throw std::runtime_error(std::string("无法连接 Modrinth（") + what + "）" +
                                 (error.empty() ? "" : "：" + WideToUtf8(error)));
    }
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded()) {
        throw std::runtime_error(std::string("Modrinth 返回了无法解析的数据（") + what + "）");
    }
    return parsed;
}

// 把 Modrinth 的搜索命中裁剪成前端需要的字段，缩小 IPC 体积
nlohmann::json TrimHit(const nlohmann::json& hit) {
    if (!hit.is_object()) return nlohmann::json::object();
    return nlohmann::json{
        {"projectId", hit.value("project_id", std::string())},
        {"slug", hit.value("slug", std::string())},
        {"title", hit.value("title", std::string())},
        {"description", hit.value("description", std::string())},
        {"iconUrl", hit.value("icon_url", std::string())},
        {"downloads", hit.value("downloads", 0)},
        {"author", hit.value("author", std::string())},
        {"categories", hit.contains("categories") ? hit["categories"] : nlohmann::json::array()},
        {"projectType", hit.value("project_type", std::string())},
    };
}

// 版本列表里挑出主文件（第一个 primary，没有就取第一个）
const nlohmann::json* PrimaryFile(const nlohmann::json& version) {
    if (!version.is_object() || !version.contains("files") || !version["files"].is_array()) {
        return nullptr;
    }
    const auto& files = version["files"];
    for (const auto& f : files) {
        if (f.is_object() && f.value("primary", false)) return &f;
    }
    return files.empty() ? nullptr : &files.front();
}

// 落盘目录：给出 targetVersionId 时优先用实例自己的目录。
// 原版实例没有独立 mods 概念的话也允许，行为与 PCL 一致（各实例独立）。
// 只取 URL 的 host 部分，避免 "https://evil.com/?x=cdn.modrinth.com" 这种子串骗过白名单
std::string HostOf(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    const size_t begin = scheme + 3;
    const size_t end = url.find_first_of("/?#", begin);
    return url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::wstring TargetDir(const std::wstring& gameDir, const std::string& projectType,
                       const std::string& targetVersionId, bool* isolatedOut) {
    const std::string folder = DirNameFor(projectType);
    if (!targetVersionId.empty() && SafeVersionId(targetVersionId)) {
        if (isolatedOut) *isolatedOut = true;
        return fs::Join(fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(targetVersionId)),
                        Utf8ToWide(folder));
    }
    if (isolatedOut) *isolatedOut = false;
    return fs::Join(gameDir, Utf8ToWide(folder));
}

}  // namespace

std::string DirNameFor(const std::string& projectType) {
    if (projectType == "resourcepack") return "resourcepacks";
    if (projectType == "shader") return "shaderpacks";
    return "mods";
}

nlohmann::json Search(const nlohmann::json& settings, const nlohmann::json& payload) {
    const std::string query = payload.value("query", std::string());
    const std::string projectType =
        payload.value("projectType", std::string("mod")).empty() ? "mod"
                                                                 : payload.value("projectType", std::string("mod"));
    const std::string gameVersion = payload.value("gameVersion", std::string());
    const std::string loader = payload.value("loader", std::string());
    const int limit = std::min(50, std::max(1, payload.value("limit", 20)));
    const int offset = std::max(0, payload.value("offset", 0));

    std::vector<std::vector<std::string>> groups;
    groups.push_back({"project_type:" + projectType});
    if (!gameVersion.empty()) groups.push_back({"versions:" + gameVersion});
    // 资源包和光影包与加载器无关，只对模组加加载器过滤
    if (projectType == "mod" && !loader.empty() && loader != "vanilla") {
        groups.push_back({"categories:" + loader});
    }

    std::wstring url = std::wstring(kApi) + L"/search?limit=" + std::to_wstring(limit) +
                       L"&offset=" + std::to_wstring(offset) + L"&index=downloads";
    if (!query.empty()) url += L"&query=" + Utf8ToWide(UrlEncode(query));
    url += L"&facets=" + Utf8ToWide(Facets(groups));

    const nlohmann::json doc = FetchJson(settings, url, "搜索");

    nlohmann::json hits = nlohmann::json::array();
    if (doc.contains("hits") && doc["hits"].is_array()) {
        for (const auto& h : doc["hits"]) hits.push_back(TrimHit(h));
    }
    return nlohmann::json{{"hits", hits},
                          {"total", doc.value("total_hits", 0)},
                          {"query", query},
                          {"page", offset / limit + 1},
                          {"pageSize", limit}};
}

nlohmann::json Versions(const nlohmann::json& settings, const nlohmann::json& payload) {
    const std::string projectId = payload.value("projectId", std::string());
    if (projectId.empty()) throw std::runtime_error("缺少项目 ID");
    if (!SafeVersionId(projectId)) throw std::runtime_error("项目 ID 非法");

    const std::string gameVersion = payload.value("gameVersion", std::string());
    const std::string loader = payload.value("loader", std::string());

    std::wstring url = std::wstring(kApi) + L"/project/" + Utf8ToWide(projectId) + L"/version";
    std::vector<std::wstring> params;
    if (!gameVersion.empty()) {
        params.push_back(L"game_versions=" +
                         Utf8ToWide("[" + UrlEncode("\"" + gameVersion + "\"") + "]"));
    }
    if (!loader.empty() && loader != "vanilla") {
        params.push_back(L"loaders=" + Utf8ToWide("[" + UrlEncode("\"" + loader + "\"") + "]"));
    }
    for (size_t i = 0; i < params.size(); ++i) url += (i ? L"&" : L"?") + params[i];

    const nlohmann::json doc = FetchJson(settings, url, "版本列表");

    nlohmann::json out = nlohmann::json::array();
    if (doc.is_array()) {
        for (const auto& v : doc) {
            if (!v.is_object()) continue;
            const nlohmann::json* file = PrimaryFile(v);
            if (!file) continue;
            out.push_back(nlohmann::json{
                {"id", v.value("id", std::string())},
                {"name", v.value("name", std::string())},
                {"versionNumber", v.value("version_number", std::string())},
                {"gameVersions", v.contains("game_versions") ? v["game_versions"]
                                                             : nlohmann::json::array()},
                {"loaders", v.contains("loaders") ? v["loaders"] : nlohmann::json::array()},
                {"datePublished", v.value("date_published", std::string())},
                {"downloadUrl", (*file).value("url", std::string())},
                {"fileName", (*file).value("filename", std::string())},
                {"fileSize", (*file).value("size", 0)},
            });
        }
    }
    return nlohmann::json{{"versions", out}, {"total", out.size()}};
}

nlohmann::json Install(const nlohmann::json& settings, const std::wstring& gameDir,
                       const nlohmann::json& payload, std::atomic<bool>* cancel,
                       const InstallProgressFn& progress) {
    const std::string projectType = payload.value("projectType", std::string("mod"));
    const std::string versionId = payload.value("versionId", std::string());
    const std::string projectId = payload.value("projectId", std::string());
    const std::string targetVersionId = payload.value("targetVersionId", std::string());

    if (versionId.empty() && projectId.empty()) {
        throw std::runtime_error("缺少要安装的项目或版本");
    }
    // versionId 会拼进 /version/<id>：只允许 Modrinth ID 的字符集，
    // 别让 "../"、"?query" 之类把请求引到同主机的其他接口上
    if (!versionId.empty() && !SafeVersionId(versionId)) {
        throw std::runtime_error("版本 ID 非法");
    }

    // 拿到具体版本：给了 versionId 就直接用，否则取该项目最新的一个版本
    std::string downloadUrl;
    std::string fileName;
    std::string expectedSha1;
    std::string chosenVersionId = versionId;

    if (chosenVersionId.empty()) {
        if (!SafeVersionId(projectId)) throw std::runtime_error("项目 ID 非法");
        const std::wstring url = std::wstring(kApi) + L"/project/" + Utf8ToWide(projectId) +
                                 L"/version";
        const nlohmann::json list = FetchJson(settings, url, "版本列表");
        if (!list.is_array() || list.empty()) {
            throw std::runtime_error("该项目没有可下载的版本");
        }
        const nlohmann::json* file = PrimaryFile(list.front());
        if (!file) throw std::runtime_error("该项目的最新版本没有可下载文件");
        chosenVersionId = list.front().value("id", std::string());
        downloadUrl = (*file).value("url", std::string());
        fileName = (*file).value("filename", std::string());
        if ((*file).contains("hashes") && (*file)["hashes"].is_object()) {
            expectedSha1 = (*file)["hashes"].value("sha1", std::string());
        }
    }

    // versionId 是 Modrinth 的版本 ID；用 /version/{id} 取下载地址
    if (downloadUrl.empty()) {
        const std::wstring url = std::wstring(kApi) + L"/version/" + Utf8ToWide(versionId);
        const nlohmann::json v = FetchJson(settings, url, "版本详情");
        const nlohmann::json* file = PrimaryFile(v);
        if (!file) throw std::runtime_error("该版本没有可下载文件");
        downloadUrl = (*file).value("url", std::string());
        fileName = (*file).value("filename", std::string());
        if ((*file).contains("hashes") && (*file)["hashes"].is_object()) {
            expectedSha1 = (*file)["hashes"].value("sha1", std::string());
        }
    }

    if (downloadUrl.empty()) throw std::runtime_error("无法解析下载地址");
    // 下载地址来自远端响应：限定 https + Modrinth CDN，避免这个命令被当成任意 URL 下载器
    {
        const std::string host = HostOf(downloadUrl);
        if (downloadUrl.rfind("https://", 0) != 0 ||
            (host != "cdn.modrinth.com" && host != "cdn-raw.modrinth.com")) {
            throw std::runtime_error("下载地址不在允许的域名内：" + host);
        }
    }
    if (fileName.empty()) fileName = "download.bin";
    // 文件名来自远端，做一次清洗，避免 ../../ 之类写穿目录
    for (char& ch : fileName) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '\0') ch = '_';
    }
    if (fileName == "." || fileName == "..") fileName = "download.bin";

    bool isolated = false;
    const std::wstring dir = TargetDir(gameDir, projectType, targetVersionId, &isolated);
    if (!fs::EnsureDir(dir)) {
        throw std::runtime_error("无法创建目录：" + WideToUtf8(dir));
    }

    const std::wstring dest = fs::Join(dir, Utf8ToWide(fileName));
    std::wstring error;
    uint64_t downloaded = 0;
    if (!net::GetFile(Utf8ToWide(downloadUrl), dest, settings, &error,
                     [&](uint64_t delta) {
                         downloaded += delta;
                         if (progress) {
                             progress(nlohmann::json{{"stage", "download"},
                                                     {"fileName", fileName},
                                                     {"downloadedBytes", downloaded}});
                         }
                         // 取消由外部设置 cancel 标志，GetFile 下一次循环会自行退出
                     },
                     cancel)) {
        if (cancel && cancel->load()) throw std::runtime_error("下载已取消");
        throw std::runtime_error("下载失败" + (error.empty() ? "" : "：" + WideToUtf8(error)));
    }

    // Modrinth 响应自带 sha1：拿到就校验，不符删掉，防止 CDN 上的文件被换
    if (expectedSha1.size() == 40) {
        if (crypto::Sha1FileHex(dest) != expectedSha1) {
            DeleteFileW(dest.c_str());
            throw std::runtime_error("下载的文件校验失败（sha1 不符），已删除，请重试");
        }
    }

    return nlohmann::json{{"ok", true},
                          {"path", WideToUtf8(dest)},
                          {"fileName", fileName},
                          {"id", chosenVersionId},
                          {"projectType", projectType},
                          {"targetVersionId", targetVersionId},
                          {"isolated", isolated}};
}

nlohmann::json Installed(const std::wstring& gameDir, const std::string& targetVersionId,
                         bool isolated) {
    nlohmann::json out = nlohmann::json::object();
    for (const char* type : {"mod", "resourcepack", "shader"}) {
        const std::wstring dir =
            TargetDir(gameDir, type, isolated ? targetVersionId : std::string(), nullptr);
        nlohmann::json files = nlohmann::json::array();
        for (const auto& path : fs::ListFiles(dir, false)) {
            const std::wstring name = fs::BaseName(path);
            // 只认 .jar / .zip 这类可识别的内容文件
            const std::wstring lower = name;
            if (lower.size() < 4) continue;
            if (!fs::Exists(path)) continue;
            files.push_back(WideToUtf8(name));
        }
        out[type] = files;
    }
    return out;
}

nlohmann::json Icon(const nlohmann::json& settings, const nlohmann::json& payload) {
    const std::string iconUrl = payload.value("iconUrl", std::string());
    if (iconUrl.empty()) return nlohmann::json(nullptr);
    // 只允许 Modrinth 的 CDN，避免这个命令被当成任意 URL 抓取器
    if (iconUrl.find("https://cdn.modrinth.com/") != 0 &&
        iconUrl.find("https://cdn-raw.modrinth.com/") != 0) {
        return nlohmann::json(nullptr);
    }

    std::wstring error;
    const std::wstring temp = fs::UniqueTempPath(L"voxel-icon");
    if (!net::GetFile(Utf8ToWide(iconUrl), temp, settings, &error, [](uint64_t) {}, nullptr)) {
        return nlohmann::json(nullptr);
    }
    const std::string bytes = fs::ReadText(temp);
    DeleteFileW(temp.c_str());
    if (bytes.empty()) return nlohmann::json(nullptr);

    std::string mime = "image/png";
    if (bytes.size() > 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xD8) {
        mime = "image/jpeg";
    }
    return nlohmann::json("data:" + mime + ";base64," + crypto::Base64Encode(bytes));
}

}  // namespace voxel::mods
