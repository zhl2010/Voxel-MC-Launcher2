#include "http.h"

#include "../core/fs.h"
#include "../core/util.h"

#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace voxel::net {
namespace {

constexpr int ATTEMPTS_PER_CANDIDATE = 2;
constexpr int CONNECT_TIMEOUT_MS = 8000;
constexpr int SEND_TIMEOUT_MS = 15000;
constexpr int RECEIVE_TIMEOUT_MS = 30000;

const wchar_t kBmcl[] = L"https://bmclapi2.bangbang93.com";

struct UrlParts {
    std::wstring scheme;
    std::wstring host;
    std::wstring path;  // 含 query
    int port = 80;
    bool secure = false;
};

bool ParseUrl(const std::wstring& url, UrlParts& out) {
    const size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos) return false;

    out.scheme = url.substr(0, schemeEnd);
    out.secure = _wcsicmp(out.scheme.c_str(), L"https") == 0;
    out.port = out.secure ? 443 : 80;

    const size_t hostStart = schemeEnd + 3;
    const size_t pathStart = url.find(L'/', hostStart);
    std::wstring authority =
        pathStart == std::wstring::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
    out.path = pathStart == std::wstring::npos ? L"/" : url.substr(pathStart);
    if (out.path.empty()) out.path = L"/";

    const size_t colon = authority.find(L':');
    if (colon != std::wstring::npos) {
        out.host = authority.substr(0, colon);
        out.port = _wtoi(authority.substr(colon + 1).c_str());
        if (out.port <= 0) out.port = out.secure ? 443 : 80;
    } else {
        out.host = authority;
    }
    return !out.host.empty();
}

std::wstring Lower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& ch : out) ch = static_cast<wchar_t>(::towlower(ch));
    return out;
}

// 代理串只接受主机:端口，scheme 由 WinHTTP 自己决定（https 代理不支持，按 http 处理）
std::wstring NormalizeProxy(const std::string& proxyUrl) {
    std::wstring raw = Utf8ToWide(proxyUrl);
    while (!raw.empty() && iswspace(raw.front())) raw.erase(raw.begin());
    while (!raw.empty() && iswspace(raw.back())) raw.pop_back();
    if (raw.empty()) return std::wstring();

    const size_t schemeEnd = raw.find(L"://");
    if (schemeEnd != std::wstring::npos) raw = raw.substr(schemeEnd + 3);
    while (!raw.empty() && raw.back() == L'/') raw.pop_back();
    return raw;
}

// 一次完整请求。body 写到 outText（内存）或 outStream（文件）。
bool RequestOnce(const std::wstring& url, const std::string& proxy, std::string* outText,
                 std::ostream* outStream, const std::function<void(uint64_t)>* onChunk,
                 const std::atomic<bool>* cancel, std::wstring* error) {
    UrlParts parts;
    if (!ParseUrl(url, parts)) {
        if (error) *error = L"URL 无效：" + url;
        return false;
    }

    const std::wstring proxyText = NormalizeProxy(proxy);
    HINTERNET session = WinHttpOpen(
        L"VoxelLauncher/1.2 (Windows)",
        proxyText.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxyText.empty() ? WINHTTP_NO_PROXY_NAME : proxyText.c_str(),
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        // 自动代理在某些精简系统上不可用，退回到直连
        session = WinHttpOpen(L"VoxelLauncher/1.2 (Windows)", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) {
        if (error) *error = L"WinHTTP 会话创建失败";
        return false;
    }

    WinHttpSetTimeouts(session, CONNECT_TIMEOUT_MS, CONNECT_TIMEOUT_MS, SEND_TIMEOUT_MS,
                       RECEIVE_TIMEOUT_MS);

    HINTERNET connect = WinHttpConnect(session, parts.host.c_str(),
                                       static_cast<INTERNET_PORT>(parts.port), 0);
    if (!connect) {
        if (error) *error = L"无法连接 " + parts.host;
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        if (error) *error = L"请求创建失败";
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    // 跟随重定向（BMCLAPI 的下载节点会 302 到签名地址），但禁止 https → http 降级。
    // 原来用 ALWAYS，等于允许镜像把我们 302 到明文地址；没有 sha1 的条目就没兜底。
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                          sizeof(redirectPolicy))) {
        if (error) *error = L"无法设置重定向策略";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    // 允许 gzip/deflate，省带宽也避免个别镜像只给压缩体
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0 &&
              WinHttpReceiveResponse(request, nullptr) != 0;

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok) {
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX) != 0;
    }

    if (!ok) {
        const DWORD lastError = GetLastError();
        if (error) {
            *error = lastError == ERROR_WINHTTP_TIMEOUT
                         ? L"网络超时"
                         : L"HTTP 请求失败（错误码 " + std::to_wstring(lastError) + L"）";
        }
    } else if (status < 200 || status >= 300) {
        if (error) *error = L"HTTP " + std::to_wstring(status);
        ok = false;
    } else if (status == 204) {
        ok = true;  // 无内容
    } else {
        std::string buffer(64 * 1024, '\0');
        for (;;) {
            if (cancel && cancel->load()) {
                if (error) *error = L"已取消";
                ok = false;
                break;
            }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                if (error) *error = L"读取响应失败";
                ok = false;
                break;
            }
            if (available == 0) break;  // 读完

            DWORD read = 0;
            DWORD want = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
            if (!WinHttpReadData(request, buffer.data(), want, &read)) {
                if (error) *error = L"读取响应失败";
                ok = false;
                break;
            }
            if (read == 0) break;

            if (outStream) {
                outStream->write(buffer.data(), static_cast<std::streamsize>(read));
                if (!*outStream) {
                    if (error) *error = L"写入本地文件失败";
                    ok = false;
                    break;
                }
            }
            if (outText) outText->append(buffer.data(), read);
            if (onChunk && *onChunk) (*onChunk)(read);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

bool LooksLikeMetaDocument(const std::wstring& url) {
    UrlParts parts;
    if (!ParseUrl(url, parts)) return false;
    return Lower(parts.host) == L"piston-meta.mojang.com";
}

}  // namespace

std::wstring MirrorUrl(const std::wstring& url, const nlohmann::json& settings) {
    UrlParts parts;
    if (!ParseUrl(url, parts)) return std::wstring();

    const std::wstring host = Lower(parts.host);
    const std::wstring base = std::wstring(kBmcl);
    const bool mirrorMojang = settings.value("mirrorMojang", true);
    const bool mirrorFabric = settings.value("mirrorFabric", true);
    const bool mirrorForge = settings.value("mirrorForge", true);
    const bool mirrorNeoForge = settings.value("mirrorNeoForge", true);

    if (mirrorMojang) {
        if (host == L"piston-meta.mojang.com" || host == L"piston-data.mojang.com" ||
            host == L"launchermeta.mojang.com") {
            return base + parts.path;
        }
        if (host == L"resources.download.minecraft.net") return base + L"/assets" + parts.path;
        if (host == L"libraries.minecraft.net") return base + L"/maven" + parts.path;
    }
    if (mirrorFabric) {
        if (host == L"meta.fabricmc.net") return base + L"/fabric-meta" + parts.path;
        if (host == L"maven.fabricmc.net") return base + L"/maven" + parts.path;
    }
    if (mirrorForge) {
        if (host == L"files.minecraftforge.net" || host == L"maven.minecraftforge.net") {
            return base + L"/maven" + parts.path;
        }
    }
    if (mirrorNeoForge && host == L"maven.neoforged.net") {
        std::wstring path = parts.path;
        const std::wstring releases = L"/releases";
        if (path.rfind(releases, 0) == 0) path = path.substr(releases.size());
        return base + L"/maven" + path;
    }
    return std::wstring();
}

std::vector<std::wstring> Candidates(const std::wstring& url, const nlohmann::json& settings) {
    std::vector<std::wstring> out;
    const std::wstring mirror = MirrorUrl(url, settings);
    if (mirror.empty() || mirror == url) {
        out.push_back(url);
        return out;
    }
    if (LooksLikeMetaDocument(url)) {
        out.push_back(url);
        out.push_back(mirror);
    } else {
        out.push_back(mirror);
        out.push_back(url);
    }
    return out;
}

bool GetText(const std::wstring& url, const nlohmann::json& settings, std::string& out,
             std::wstring* error) {
    const std::string proxy = settings.value("proxyUrl", std::string());
    const std::vector<std::wstring> candidates = Candidates(url, settings);
    std::wstring lastError;

    for (const auto& candidate : candidates) {
        for (int attempt = 0; attempt < ATTEMPTS_PER_CANDIDATE; ++attempt) {
            std::string body;
            if (RequestOnce(candidate, proxy, &body, nullptr, nullptr, nullptr, &lastError)) {
                out = std::move(body);
                return true;
            }
        }
    }
    if (error) *error = lastError;
    return false;
}

bool GetFileOnce(const std::wstring& url, const std::wstring& dest, const std::string& proxy,
                 std::wstring* error, const std::function<void(uint64_t)>& onChunk,
                 const std::atomic<bool>* cancel) {
    fs::EnsureDir(fs::ParentDir(dest));
    const std::wstring part = dest + L".part";
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = L"无法写入 " + part;
        return false;
    }
    bool ok = RequestOnce(url, proxy, nullptr, &out, &onChunk, cancel, error);
    out.close();
    if (!ok) {
        DeleteFileW(part.c_str());
        return false;
    }
    // 下载完整后才替换目标，半个文件不会被下次启动当成"已存在"而跳过
    if (!MoveFileExW(part.c_str(), dest.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = L"无法保存文件 " + dest;
        DeleteFileW(part.c_str());
        return false;
    }
    return true;
}

bool GetFile(const std::wstring& url, const std::wstring& dest, const nlohmann::json& settings,
             std::wstring* error, const std::function<void(uint64_t)>& onChunk,
             const std::atomic<bool>* cancel) {
    const std::string proxy = settings.value("proxyUrl", std::string());
    const std::vector<std::wstring> candidates = Candidates(url, settings);
    std::wstring lastError;

    for (const auto& candidate : candidates) {
        for (int attempt = 0; attempt < ATTEMPTS_PER_CANDIDATE; ++attempt) {
            if (cancel && cancel->load()) {
                if (error) *error = L"已取消";
                return false;
            }
            if (GetFileOnce(candidate, dest, proxy, &lastError, onChunk, cancel)) return true;
            if (cancel && cancel->load()) {
                if (error) *error = L"已取消";
                return false;
            }
        }
    }
    if (error) *error = lastError.empty() ? L"下载失败：" + url : lastError;
    return false;
}

}  // namespace voxel::net
