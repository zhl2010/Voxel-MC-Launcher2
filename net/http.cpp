#include "http.h"

#include "../core/fs.h"
#include "../core/util.h"

#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace voxel::net {
namespace {

constexpr int ATTEMPTS_PER_CANDIDATE = 2;
constexpr int CONNECT_TIMEOUT_MS = 8000;
constexpr int SEND_TIMEOUT_MS = 15000;
constexpr int RECEIVE_TIMEOUT_MS = 30000;

// 分段下载：小于这个体积的文件没必要切（握手/探测开销占比太高）
constexpr uint64_t kParallelMinBytes = 1ull * 1024 * 1024;
// 单段最多重试次数；每次从已写入位置续传
constexpr int kSegmentAttempts = 3;

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

// 连接复用：原实现每个文件都 WinHttpOpen + WinHttpConnect，等于每个文件重做一次
// TLS 握手。实测单文件的握手开销约 1 秒，5000 个资源文件在 8 并发下要 10 分钟以上，
// 而真正传数据的时间占比很小。
//
// 这里按 (proxy, host, port) 缓存 WinHTTP 会话与连接句柄，后续请求直接复用。
// 句柄在进程生命周期内不释放（数量极少：资源站、maven、meta 各一个），
// 由进程退出统一回收。
std::mutex g_handleMutex;
std::map<std::wstring, HINTERNET> g_sessions;
std::map<std::wstring, HINTERNET> g_connections;

// 镜像/官方源健康度。源不可用时逐文件重试会把整体拖垮：BMCLAPI 的 assets
// 端点会 302 到第三方节点（open-bmcl-api.lunar.cc），实测 15 秒才回应且 SSL
// 连接失败；几千个资源文件每个白等两次，安装时间从两分钟变成几十分钟。
// 连续失败到阈值后本次运行直接换源，成功一次即恢复。
constexpr int SOURCE_FAILURE_LIMIT = 2;

struct SourceHealth {
    int mirrorFails = 0;
    int originFails = 0;
};

std::mutex g_sourceMutex;
std::map<std::wstring, SourceHealth> g_sourceHealth;

bool SourceBroken(const std::wstring& host, bool mirror) {
    if (host.empty()) return false;
    std::lock_guard<std::mutex> lock(g_sourceMutex);
    auto it = g_sourceHealth.find(host);
    if (it == g_sourceHealth.end()) return false;
    return (mirror ? it->second.mirrorFails : it->second.originFails) >= SOURCE_FAILURE_LIMIT;
}

void NoteSourceResult(const std::wstring& host, bool mirror, bool ok) {
    if (host.empty()) return;
    std::lock_guard<std::mutex> lock(g_sourceMutex);
    int& fails = mirror ? g_sourceHealth[host].mirrorFails : g_sourceHealth[host].originFails;
    fails = ok ? 0 : fails + 1;
}

HINTERNET GetSession(const std::wstring& proxyText) {
    std::lock_guard<std::mutex> lock(g_handleMutex);
    const std::wstring key = proxyText;
    auto it = g_sessions.find(key);
    if (it != g_sessions.end()) return it->second;

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
    if (!session) return nullptr;

    WinHttpSetTimeouts(session, CONNECT_TIMEOUT_MS, CONNECT_TIMEOUT_MS, SEND_TIMEOUT_MS,
                       RECEIVE_TIMEOUT_MS);
    // WinHTTP 默认每个主机只保留少量连接；不抬高这个值，32 个下载线程会挤在
    // 少数连接上排队，等于没有并发。
    DWORD maxPerServer = 32;
    WinHttpSetOption(session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &maxPerServer,
                     sizeof(maxPerServer));
    WinHttpSetOption(session, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &maxPerServer,
                     sizeof(maxPerServer));

    g_sessions[key] = session;
    return session;
}

HINTERNET GetConnection(HINTERNET session, const std::wstring& proxyText,
                        const UrlParts& parts) {
    std::lock_guard<std::mutex> lock(g_handleMutex);
    // 句柄必须按线程隔离：多个线程同时往同一个 WinHttpConnect 句柄上发请求会被
    // 串行化到同一条 TCP 连接，实测 8 线程共享只有 0.88MB/s，各自持有能到 6.7MB/s。
    // 加线程 ID 后既保留"同一线程复用连接、不重复 TLS 握手"，又不牺牲并发。
    const std::wstring key = proxyText + L"|" + parts.host + L"|" + std::to_wstring(parts.port) +
                             L"|" + std::to_wstring(GetCurrentThreadId());
    auto it = g_connections.find(key);
    if (it != g_connections.end()) return it->second;

    HINTERNET connect = WinHttpConnect(session, parts.host.c_str(),
                                       static_cast<INTERNET_PORT>(parts.port), 0);
    if (!connect) return nullptr;
    g_connections[key] = connect;
    return connect;
}

// 解析 Content-Range: bytes 0-0/28476894 里的总长度
uint64_t TotalFromContentRange(const std::wstring& value) {
    const size_t slash = value.rfind(L'/');
    if (slash == std::wstring::npos || slash + 1 >= value.size()) return 0;
    const std::wstring digits = value.substr(slash + 1);
    if (digits == L"*") return 0;
    return static_cast<uint64_t>(_wcstoui64(digits.c_str(), nullptr, 10));
}

// 一次完整请求。body 写到 outText（内存）或 outStream（文件）。
// rangeStart >= 0 时带 Range 头；若服务器不返回 206（不支持分段）按失败处理，
// 避免把整份文件写进某一段。totalOut 回填 Content-Range 里的总长度。
bool RequestOnce(const std::wstring& url, const std::string& proxy, std::string* outText,
                 std::ostream* outStream, const std::function<void(uint64_t)>* onChunk,
                 const std::atomic<bool>* cancel, std::wstring* error,
                 int64_t rangeStart = -1, int64_t rangeEnd = -1,
                 uint64_t* totalOut = nullptr) {
    UrlParts parts;
    if (!ParseUrl(url, parts)) {
        if (error) *error = L"URL 无效：" + url;
        return false;
    }

    const std::wstring proxyText = NormalizeProxy(proxy);
    HINTERNET session = GetSession(proxyText);
    if (!session) {
        if (error) *error = L"WinHTTP 会话创建失败";
        return false;
    }

    HINTERNET connect = GetConnection(session, proxyText, parts);
    if (!connect) {
        if (error) *error = L"无法连接 " + parts.host;
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        if (error) *error = L"请求创建失败";
        // connect / session 是缓存句柄，不能关
        // session 由缓存持有，进程退出时统一回收
        return false;
    }

    // 跟随重定向（BMCLAPI 的下载节点会 302 到签名地址），但禁止 https → http 降级。
    // 原来用 ALWAYS，等于允许镜像把我们 302 到明文地址；没有 sha1 的条目就没兜底。
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                          sizeof(redirectPolicy))) {
        if (error) *error = L"无法设置重定向策略";
        WinHttpCloseHandle(request);
        // connect / session 是缓存句柄，不能关
        // session 由缓存持有，进程退出时统一回收
        return false;
    }

    // 允许 gzip/deflate，省带宽也避免个别镜像只给压缩体
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));

    std::wstring headers;
    if (rangeStart >= 0) {
        headers = L"Range: bytes=" + std::to_wstring(rangeStart) + L"-";
        if (rangeEnd >= 0) headers += std::to_wstring(rangeEnd);
        headers += L"\r\n";
    }

    bool ok = WinHttpSendRequest(request,
                                 headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                 headers.empty() ? 0 : static_cast<DWORD>(-1),
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
    } else if (rangeStart >= 0 && status != 206) {
        // 服务器忽略了 Range（回整份 200）：不能把全量写进某个分段，直接判失败
        if (error) *error = L"服务器不支持分段下载";
        ok = false;
    } else if (status == 204) {
        ok = true;  // 无内容
    } else {
        if (totalOut) {
            wchar_t range[128]{};
            DWORD rangeSize = sizeof(range);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Content-Range", range,
                                    &rangeSize, WINHTTP_NO_HEADER_INDEX)) {
                *totalOut = TotalFromContentRange(range);
            }
        }
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
    // connect / session 复用，不在此关闭
    // session 复用，不在此关闭
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

    UrlParts parts;
    const std::wstring host = ParseUrl(url, parts) ? Lower(parts.host) : std::wstring();
    const bool mirrorBroken = SourceBroken(host, true);
    const bool originBroken = SourceBroken(host, false);

    // 资源对象（assets）优先官方 CDN：镜像要把请求 302 到第三方节点，
    // 实测单文件 15 秒且经常失败，而官方 CDN 只要 0.35 秒。
    // meta 文档也优先官方，保证版本清单权威；其余（maven 等）仍镜像优先。
    const bool preferOrigin =
        host == L"resources.download.minecraft.net" || LooksLikeMetaDocument(url);

    const bool originFirst = mirrorBroken || (!originBroken && preferOrigin);
    if (originFirst) {
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

    UrlParts origin;
    const std::wstring originHost = ParseUrl(url, origin) ? Lower(origin.host) : std::wstring();

    for (const auto& candidate : candidates) {
        const bool isMirror = candidate != url;
        // 镜像只试一次：它一旦不健康，每次重试都要等满超时，代价远高于收益
        const int attempts = isMirror ? 1 : ATTEMPTS_PER_CANDIDATE;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            std::string body;
            const bool ok =
                RequestOnce(candidate, proxy, &body, nullptr, nullptr, nullptr, &lastError);
            NoteSourceResult(originHost, isMirror, ok);
            if (ok) {
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

    UrlParts origin;
    const std::wstring originHost = ParseUrl(url, origin) ? Lower(origin.host) : std::wstring();

    for (const auto& candidate : candidates) {
        const bool isMirror = candidate != url;
        // 镜像只试一次：它一旦不健康，每次重试都要等满超时，代价远高于收益
        const int attempts = isMirror ? 1 : ATTEMPTS_PER_CANDIDATE;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            if (cancel && cancel->load()) {
                if (error) *error = L"已取消";
                return false;
            }
            const bool ok =
                GetFileOnce(candidate, dest, proxy, &lastError, onChunk, cancel);
            NoteSourceResult(originHost, isMirror, ok);
            if (ok) return true;
            if (cancel && cancel->load()) {
                if (error) *error = L"已取消";
                return false;
            }
        }
    }
    if (error) *error = lastError.empty() ? L"下载失败：" + url : lastError;
    return false;
}

bool GetFileParallel(const std::wstring& url, const std::wstring& dest,
                     const nlohmann::json& settings, std::wstring* error,
                     const std::function<void(uint64_t)>& onChunk,
                     const std::atomic<bool>* cancel) {
    const std::string proxy = settings.value("proxyUrl", std::string());

    // 先用 1 字节的 Range 请求探出总长度；服务器不支持 Range 或文件太小就退回单连接。
    // Modrinth 的 CDN 实测单连接速度波动极大（同一文件 200KB/s 到 5MB/s），
    // 而分段请求通常能稳定在 1-2MB/s，把大文件切开并发拉明显更快。
    std::wstring target;
    uint64_t total = 0;
    for (const auto& candidate : Candidates(url, settings)) {
        std::wstring probeError;
        uint64_t size = 0;
        if (RequestOnce(candidate, proxy, nullptr, nullptr, nullptr, nullptr, &probeError, 0, 0,
                        &size) &&
            size >= kParallelMinBytes) {
            target = candidate;
            total = size;
            break;
        }
    }

    if (target.empty() || total < kParallelMinBytes) {
        return GetFile(url, dest, settings, error, onChunk, cancel);
    }

    fs::EnsureDir(fs::ParentDir(dest));
    const std::wstring part = dest + L".part";

    int segmentCount = 3;
    // 实测这条线路对「单条连接」限得很死：同样 27MB，1 条连接 0.08MB/s，
    // 8 条并发能跑到 9MB/s（3 秒下完），所以大文件要切得够碎。
    if (total >= 6ull * 1024 * 1024) {
        segmentCount = 8;
    } else if (total >= 3ull * 1024 * 1024) {
        segmentCount = 6;
    }
    const uint64_t chunk = (total + segmentCount - 1) / segmentCount;

    std::vector<std::wstring> segPaths;
    for (int index = 0; index < segmentCount; ++index) {
        segPaths.push_back(part + L".seg" + std::to_wstring(index));
        DeleteFileW(segPaths.back().c_str());
    }

    std::atomic<bool> failed{false};
    std::mutex errorMutex;
    std::wstring firstError;
    std::vector<std::thread> workers;
    for (int index = 0; index < segmentCount; ++index) {
        workers.emplace_back([&, index]() {
            const uint64_t from = static_cast<uint64_t>(index) * chunk;
            if (from >= total) return;
            uint64_t to = from + chunk - 1;
            if (to >= total) to = total - 1;
            const std::wstring& segPath = segPaths[index];
            uint64_t offset = from;
            std::wstring segError;
            // 断了就从已写入的位置续传，不重下整段
            for (int attempt = 0; attempt < kSegmentAttempts && offset <= to; ++attempt) {
                if ((cancel && cancel->load()) || failed.load()) return;
                std::ofstream out(segPath,
                                  std::ios::binary | (attempt == 0 ? std::ios::trunc : std::ios::app));
                if (!out) {
                    segError = L"无法写入临时文件";
                    break;
                }
                const bool ok = RequestOnce(target, proxy, nullptr, &out, &onChunk, cancel,
                                            &segError, static_cast<int64_t>(offset),
                                            static_cast<int64_t>(to), nullptr);
                out.close();
                if (ok) return;
                if (cancel && cancel->load()) return;
                offset = from + fs::FileSize(segPath);
            }
            if (offset > to) return;
            {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (firstError.empty()) {
                    firstError = segError.empty() ? L"分段下载中断" : segError;
                }
            }
            failed = true;
        });
    }
    for (auto& worker : workers) worker.join();

    bool ok = !failed.load();
    if (ok) {
        std::ofstream merged(part, std::ios::binary | std::ios::trunc);
        ok = static_cast<bool>(merged);
        for (const auto& segPath : segPaths) {
            if (!ok) break;
            std::ifstream in(segPath, std::ios::binary);
            if (!in) {
                ok = false;
                break;
            }
            merged << in.rdbuf();
            if (!merged) ok = false;
        }
        if (merged.is_open()) merged.close();
    }
    for (const auto& segPath : segPaths) DeleteFileW(segPath.c_str());

    if (!ok) {
        DeleteFileW(part.c_str());
        if (error) {
            if (cancel && cancel->load()) {
                *error = L"已取消";
            } else {
                *error = firstError.empty() ? L"下载失败" : firstError;
            }
        }
        return false;
    }
    if (!MoveFileExW(part.c_str(), dest.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = L"无法保存文件 " + dest;
        DeleteFileW(part.c_str());
        return false;
    }
    return true;
}

}  // namespace voxel::net
