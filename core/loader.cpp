#include "loader.h"

#include "crypto.h"
#include "dirs.h"
#include "fs.h"
#include "installer.h"
#include "proc.h"
#include "util.h"

#include "../net/http.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxel::loader {
namespace {

// Fabric：官方 meta 端点给出 game 版本对应的 loader 列表
constexpr wchar_t kFabricGame[] =
    L"https://meta.fabricmc.net/v2/versions/loader/{mc}";
constexpr wchar_t kFabricProfile[] =
    L"https://meta.fabricmc.net/v2/versions/loader/{mc}/{loader}/profile/json";

// Quilt 与 Fabric 同构，先不接入（前端只展示四种加载器）

std::wstring ReplaceAll(std::wstring text, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return text;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

// 从 JSON 数组里取第一项的某个字符串字段
std::string FirstField(const nlohmann::json& array, const char* key) {
    if (!array.is_array() || array.empty()) return std::string();
    const auto& item = array.front();
    if (!item.is_object() || !item.contains(key) || !item[key].is_string()) return std::string();
    return item[key].get<std::string>();
}

// 解析 Fabric 的 loader 列表：数组元素形如
//   { "loader": { "version": "0.16.9", "stable": true }, "intermediary": {...}, "launcherMeta": {...} }
std::vector<std::string> FabricLoaderVersions(const nlohmann::json& list, bool stableOnly) {
    std::vector<std::string> out;
    if (!list.is_array()) return out;
    for (const auto& entry : list) {
        if (!entry.is_object() || !entry.contains("loader") || !entry["loader"].is_object()) continue;
        const auto& loader = entry["loader"];
        const std::string version = loader.value("version", std::string());
        if (version.empty()) continue;
        if (stableOnly && !loader.value("stable", false)) continue;
        out.push_back(version);
    }
    return out;
}

// 把加载器 profile 的 id 改成我们要用的实例名，并补上 inheritsFrom。
//
// 官方 profile 的 id 通常是 "fabric-loader-0.16.9-1.21.1"，我们用更易读的
// "<mc>-fabric-<loader>"；同时保留 profile 自带的 libraries 与 mainClass，
// 原版内容由 inheritsFrom 在解析阶段合并，不必复制一份。
nlohmann::json RewriteProfile(nlohmann::json profile, const std::string& instanceId,
                              const std::string& mcVersion) {
    profile["id"] = instanceId;
    profile["inheritsFrom"] = mcVersion;
    if (!profile.contains("type") || !profile["type"].is_string()) profile["type"] = "release";
    if (!profile.contains("libraries") || !profile["libraries"].is_array()) {
        profile["libraries"] = nlohmann::json::array();
    }
    return profile;
}

std::string InstanceIdFor(const std::string& mc, const std::string& loader,
                          const std::string& loaderVersion) {
    return mc + "-" + loader + "-" + loaderVersion;
}

// 按点分段比较版本号（"52.1.10" > "52.1.9"）；非数字段退化为字典序
int CompareDotted(const std::string& a, const std::string& b) {
    size_t ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        size_t ea = a.find('.', ia);
        size_t eb = b.find('.', ib);
        if (ea == std::string::npos) ea = a.size();
        if (eb == std::string::npos) eb = b.size();
        const std::string sa = a.substr(ia, ea - ia);
        const std::string sb = b.substr(ib, eb - ib);

        const bool na = !sa.empty() && std::all_of(sa.begin(), sa.end(), ::isdigit);
        const bool nb = !sb.empty() && std::all_of(sb.begin(), sb.end(), ::isdigit);
        if (na && nb) {
            const long long va = std::stoll(sa);
            const long long vb = std::stoll(sb);
            if (va != vb) return va < vb ? -1 : 1;
        } else if (sa != sb) {
            return sa < sb ? -1 : 1;
        }
        ia = ea + 1;
        ib = eb + 1;
    }
    return 0;
}

std::wstring VersionDir(const std::wstring& gameDir, const std::string& id) {
    return fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(id));
}

// 加载器实例必须挂在原版版本下面。前端可能把"当前选中的版本"当 gameVersion
// 传进来，而它本身可能是另一个加载器实例（例如已经装过一次 Forge）。直接拿它
// 当 inheritsFrom 会让实例嵌套继承实例，删除父实例时被依赖检查拦住。
// 这里沿 inheritsFrom 回溯到最底层的原版版本；本地找不到 JSON 就按原样返回。
std::string BaseMinecraftVersion(const std::wstring& gameDir, const std::string& version) {
    std::string current = version;
    for (int depth = 0; depth < 8; ++depth) {
        const std::wstring file =
            fs::Join(VersionDir(gameDir, current), Utf8ToWide(current) + L".json");
        const std::string raw = fs::ReadText(file);
        if (raw.empty()) return current;
        const nlohmann::json json = nlohmann::json::parse(raw, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return current;
        const std::string parent = json.value("inheritsFrom", std::string());
        if (parent.empty()) return current;
        current = parent;
    }
    return current;
}

// 写入 <versions>/<id>/<id>.json，返回实例 id
std::string WriteInstance(const std::wstring& gameDir, const std::string& id,
                          const nlohmann::json& profileJson) {
    if (!SafeVersionId(id)) throw std::runtime_error("生成的版本名非法：" + id);
    const std::wstring dir = VersionDir(gameDir, id);
    if (!fs::EnsureDir(dir)) throw std::runtime_error("无法创建版本目录：" + WideToUtf8(dir));
    const std::wstring file = fs::Join(dir, Utf8ToWide(id) + L".json");
    if (!fs::WriteTextAtomic(file, profileJson.dump(2))) {
        throw std::runtime_error("写入版本 JSON 失败：" + WideToUtf8(file));
    }
    return id;
}

// Fabric 安装：先查列表确认版本存在，再取 profile/json 落地
nlohmann::json InstallFabric(const nlohmann::json& settings, const std::wstring& gameDir,
                             const std::string& mc, std::string loaderVersion,
                             const std::string& customName) {
    std::wstring error;
    std::string body;

    const std::wstring listUrl = ReplaceAll(kFabricGame, L"{mc}", Utf8ToWide(mc));
    if (!net::GetText(listUrl, settings, body, &error)) {
        throw std::runtime_error("无法获取 Fabric 版本列表" + (error.empty() ? "" : "：" + WideToUtf8(error)));
    }
    nlohmann::json list = nlohmann::json::parse(body, nullptr, false);
    if (list.is_discarded()) throw std::runtime_error("Fabric 版本列表格式异常");

    // 未指定版本就用最新的稳定版
    if (loaderVersion.empty()) {
        auto stable = FabricLoaderVersions(list, true);
        if (stable.empty()) stable = FabricLoaderVersions(list, false);
        if (stable.empty()) throw std::runtime_error("该 Minecraft 版本没有可用的 Fabric 加载器");
        loaderVersion = stable.front();
    } else {
        const auto all = FabricLoaderVersions(list, false);
        if (std::find(all.begin(), all.end(), loaderVersion) == all.end()) {
            throw std::runtime_error("Fabric " + loaderVersion + " 不支持 Minecraft " + mc);
        }
    }

    const std::wstring profileUrl = ReplaceAll(
        ReplaceAll(kFabricProfile, L"{mc}", Utf8ToWide(mc)), L"{loader}", Utf8ToWide(loaderVersion));
    body.clear();
    if (!net::GetText(profileUrl, settings, body, &error)) {
        throw std::runtime_error("无法获取 Fabric 安装配置" +
                                 (error.empty() ? "" : "：" + WideToUtf8(error)));
    }
    nlohmann::json profile = nlohmann::json::parse(body, nullptr, false);
    if (profile.is_discarded() || !profile.is_object()) {
        throw std::runtime_error("Fabric 安装配置格式异常");
    }

    const std::string id = customName.empty()
                                ? InstanceIdFor(mc, "fabric", loaderVersion)
                                : customName;
    const int libraries = profile.contains("libraries") && profile["libraries"].is_array()
                              ? static_cast<int>(profile["libraries"].size())
                              : 0;
    WriteInstance(gameDir, id, RewriteProfile(profile, id, mc));

    return nlohmann::json{{"id", id},
                          {"loaderVersion", loaderVersion},
                          {"librariesDownloaded", libraries}};
}

}  // namespace

// ---------------------------------------------------------------------------
// NeoForge 26.x 的运行前提
//
// 官方安装器除了写版本 JSON，还会把两样东西落到 libraries：
//   1. 安装包里自带的 neoforge-<ver>-universal.jar
//   2. installertools 用安装包里的 binpatch 处理过的客户端 jar
//      （FML 按 net.minecraft:client:<mc>-<neoform>:srg 这个老路径查找）
// 这两步以前被跳过了，于是启动时报
// "Your NeoForge installation is corrupted" / "The NeoForge jar is missing"。
// ---------------------------------------------------------------------------
struct MavenPart {
    std::string group;
    std::string artifact;
    std::string version;
    std::string classifier;
};

MavenPart ParseMaven(const std::string& name) {
    MavenPart part;
    std::vector<std::string> pieces;
    std::string token;
    for (char ch : name) {
        if (ch == ':') {
            pieces.push_back(token);
            token.clear();
        } else {
            token.push_back(ch);
        }
    }
    pieces.push_back(token);
    if (pieces.size() < 3) return part;
    part.group = pieces[0];
    part.artifact = pieces[1];
    part.version = pieces[2];
    if (pieces.size() > 3) part.classifier = pieces[3];
    return part;
}

std::wstring MavenRelPath(const MavenPart& part) {
    if (part.artifact.empty()) return std::wstring();
    std::wstring group = Utf8ToWide(part.group);
    for (auto& ch : group) {
        if (ch == L'.') ch = L'/';
    }
    std::wstring file = Utf8ToWide(part.artifact) + L"-" + Utf8ToWide(part.version);
    if (!part.classifier.empty()) file += L"-" + Utf8ToWide(part.classifier);
    return group + L"/" + Utf8ToWide(part.artifact) + L"/" + Utf8ToWide(part.version) + L"/" + file +
           L".jar";
}

// 从安装包（zip）里解出一个成员到独立目录；成功返回 true。
bool ExtractMember(const std::wstring& archive, const std::wstring& member,
                   const std::wstring& dest) {
    fs::DeleteTree(dest);
    if (!fs::EnsureDir(dest)) return false;
    int code = 0;
    std::wstring error;
    return proc::RunCapture(TarExe(), {L"-xf", archive, L"-C", dest, member}, dest, 120000, &code,
                            &error);
}

// 保证 maven 构件在 libraries 里（缺了就从 BMCL 镜像下载）。
std::wstring EnsureMaven(const nlohmann::json& settings, const std::wstring& libraries,
                         const MavenPart& part, std::wstring* error) {
    const std::wstring rel = MavenRelPath(part);
    if (rel.empty()) return std::wstring();
    const std::wstring dest = fs::Join(libraries, rel);
    if (fs::Exists(dest)) return dest;
    if (!fs::EnsureDir(fs::ParentDir(dest))) {
        if (error) *error = L"无法创建目录：" + fs::ParentDir(dest);
        return std::wstring();
    }
    std::wstring downloadError;
    const std::wstring url = L"https://maven.neoforged.net/releases/" + rel;
    if (!net::GetFile(url, dest, settings, &downloadError, [](uint64_t) {}, nullptr)) {
        if (error) *error = downloadError.empty() ? L"下载失败：" + rel : downloadError;
        return std::wstring();
    }
    return dest;
}

std::string StripQuotes(std::string text) {
    while (!text.empty() && (text.front() == '\'' || text.front() == '"')) text.erase(text.begin());
    while (!text.empty() && (text.back() == '\'' || text.back() == '"')) text.pop_back();
    return text;
}

// NeoForge 安装完之后补齐 FML 需要的两个文件：universal jar 与 srg 客户端 jar。
bool EnsureNeoForgeRuntime(const nlohmann::json& settings, const std::wstring& gameDir,
                           const std::wstring& installerJar, const std::string& mc,
                           const std::string& loaderVersion, std::wstring* error) {
    const std::wstring libraries = fs::Join(gameDir, L"libraries");
    const std::wstring temp = fs::UniqueTempPath(L"voxel-neoforge");
    if (!fs::EnsureDir(temp)) {
        if (error) *error = L"无法创建临时目录";
        return false;
    }

    bool ok = false;
    do {
        // 1) universal jar：安装包内路径是 maven/net/neoforged/neoforge/<ver>/...
        const std::wstring universalRel = L"net/neoforged/neoforge/" +
                                          Utf8ToWide(loaderVersion) + L"/neoforge-" +
                                          Utf8ToWide(loaderVersion) + L"-universal.jar";
        if (!fs::Exists(fs::Join(libraries, universalRel))) {
            const std::wstring unpack = fs::Join(temp, L"universal");
            if (!ExtractMember(installerJar, L"maven/" + universalRel, unpack)) break;
            const std::wstring src = fs::Join(unpack, L"maven/" + universalRel);
            const std::wstring dest = fs::Join(libraries, universalRel);
            if (!fs::Exists(src) || !fs::EnsureDir(fs::ParentDir(dest))) break;
            if (!CopyFileW(src.c_str(), dest.c_str(), FALSE)) break;
        }

        // 2) install_profile.json：拿 installertools 坐标与 NeoForm 版本号
        const std::wstring profileDir = fs::Join(temp, L"profile");
        if (!ExtractMember(installerJar, L"install_profile.json", profileDir)) break;
        const std::string rawProfile = fs::ReadText(fs::Join(profileDir, L"install_profile.json"));
        const nlohmann::json profile = nlohmann::json::parse(rawProfile, nullptr, false);
        if (profile.is_discarded() || !profile.is_object()) break;

        std::string mcpVersion;
        if (profile.contains("data") && profile["data"].is_object() &&
            profile["data"].contains("MCP_VERSION")) {
            const auto& mcp = profile["data"]["MCP_VERSION"];
            if (mcp.is_object()) {
                mcpVersion = StripQuotes(mcp.value("client", std::string()));
            } else if (mcp.is_string()) {
                mcpVersion = StripQuotes(mcp.get<std::string>());
            }
        }
        if (mcpVersion.empty()) break;

        // 找到客户端要跑的 processor（PROCESS_MINECRAFT_JAR）
        MavenPart tool;
        if (profile.contains("processors") && profile["processors"].is_array()) {
            for (const auto& processor : profile["processors"]) {
                if (!processor.is_object()) continue;
                if (processor.contains("sides") && processor["sides"].is_array()) {
                    bool serverOnly = true;
                    for (const auto& side : processor["sides"]) {
                        if (side.is_string() && side.get<std::string>() != "server") {
                            serverOnly = false;
                        }
                    }
                    if (serverOnly) continue;
                }
                if (processor["args"].dump().find("PROCESS_MINECRAFT_JAR") == std::string::npos) {
                    continue;
                }
                tool = ParseMaven(processor.value("jar", std::string()));
                break;
            }
        }
        if (tool.artifact.empty()) break;

        std::wstring detail;
        const std::wstring toolJar = EnsureMaven(settings, libraries, tool, &detail);
        if (toolJar.empty()) {
            if (error) *error = detail;
            break;
        }

        // 3) 原版客户端 jar（processor 的输入）
        ResolvedVersion base = ResolveVersion(gameDir, settings, mc, &detail);
        std::wstring clientJar = fs::Join(
            fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(base.jarId)),
            Utf8ToWide(base.jarId) + L".jar");
        if (!fs::Exists(clientJar)) {
            std::string clientUrl;
            for (const auto& level : base.levels) {
                if (!level.json.contains("downloads") || !level.json["downloads"].is_object()) {
                    continue;
                }
                const auto& downloads = level.json["downloads"];
                if (!downloads.contains("client") || !downloads["client"].is_object()) continue;
                clientUrl = downloads["client"].value("url", std::string());
                if (!clientUrl.empty()) {
                    clientJar = fs::Join(
                        fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(level.id)),
                        Utf8ToWide(level.id) + L".jar");
                    break;
                }
            }
            if (clientUrl.empty() || !fs::EnsureDir(fs::ParentDir(clientJar))) {
                if (error) *error = L"找不到 Minecraft " + Utf8ToWide(mc) + L" 的客户端 jar";
                break;
            }
            if (!net::GetFile(Utf8ToWide(clientUrl), clientJar, settings, &detail,
                              [](uint64_t) {}, nullptr)) {
                if (error) *error = detail;
                break;
            }
        }

        // 4) binpatch
        const std::wstring binpatchDir = fs::Join(temp, L"binpatch");
        if (!ExtractMember(installerJar, L"data/client.lzma", binpatchDir)) break;
        const std::wstring binpatch = fs::Join(binpatchDir, L"data/client.lzma");

        // 5) 跑 processor：生成 patched 客户端，同时落到 FML 期望的 srg 路径
        const std::wstring patchedRel = L"net/neoforged/minecraft-client-patched/" +
                                        Utf8ToWide(loaderVersion) +
                                        L"/minecraft-client-patched-" + Utf8ToWide(loaderVersion) +
                                        L".jar";
        const std::wstring srgRel = L"net/minecraft/client/" + Utf8ToWide(mcpVersion) +
                                    L"/client-" + Utf8ToWide(mcpVersion) + L"-srg.jar";
        const std::wstring patched = fs::Join(libraries, patchedRel);
        const std::wstring srg = fs::Join(libraries, srgRel);
        if (!fs::EnsureDir(fs::ParentDir(patched)) || !fs::EnsureDir(fs::ParentDir(srg))) break;

        std::wstring javaError;
        const JavaInfo java = SelectJava(settings, gameDir, base.javaMajor, &javaError);
        if (java.path.empty()) {
            if (error) *error = javaError.empty() ? L"找不到可用的 Java" : javaError;
            break;
        }

        int code = 0;
        std::wstring runError;
        const std::vector<std::wstring> args{L"-jar",           toolJar,
                                             L"--task",         L"PROCESS_MINECRAFT_JAR",
                                             L"--no-mod-manifest", L"--input", clientJar,
                                             L"--output",       patched,
                                             L"--extract-libraries-to", libraries + L"\\",
                                             L"--apply-patches", binpatch};
        if (!proc::RunCapture(java.path, args, temp, 900000, &code, &runError) || code != 0 ||
            !fs::Exists(patched)) {
            if (error) {
                *error = L"NeoForge processor 执行失败" +
                         (runError.empty() ? L"" : L"：" + runError);
            }
            break;
        }
        CopyFileW(patched.c_str(), srg.c_str(), FALSE);
        if (!fs::Exists(srg)) break;
        ok = true;
    } while (false);

    fs::DeleteTree(temp);
    if (!ok && error && error->empty()) *error = L"NeoForge 运行时文件准备失败";
    return ok;
}

// Forge / NeoForge：从官方 installer jar 里取出 version.json 作为版本元数据。
//
// 这条路避开了"跑 processor"的重活——installer 里的 version.json 已经是
// 完整的版本描述（mainClass、libraries、arguments），inheritsFrom 指回原版，
// 和 Fabric 一样可以直接落地成一个可启动的实例。
struct NeoLikeSource {
    std::string loader;        // forge / neoforge
    std::wstring metaUrl;      // maven-metadata.xml（走 BMCL 镜像）
    std::wstring artifactBase; // installer jar 所在目录前缀
    std::string versionPrefix; // 版本号前缀，如 "1.21.1-" 或 "21.1."
};

nlohmann::json InstallNeoLike(const nlohmann::json& settings, const std::wstring& gameDir,
                              const std::string& mc, const NeoLikeSource& source,
                              std::string loaderVersion, const std::string& customName) {
    const bool isNeo = source.loader == "neoforge";

    // 1) 确定版本号：没指定就取该 Minecraft 版本对应的最新构建
    if (loaderVersion.empty()) {
        std::string body;
        std::wstring error;
        if (!net::GetText(source.metaUrl, settings, body, &error)) {
            throw std::runtime_error("无法获取 " + source.loader + " 版本列表" +
                                     (error.empty() ? "" : "：" + WideToUtf8(error)));
        }
        std::string best;
        size_t pos = 0;
        while ((pos = body.find("<version>", pos)) != std::string::npos) {
            const size_t end = body.find("</version>", pos);
            if (end == std::string::npos) break;
            const std::string v = body.substr(pos + 9, end - pos - 9);
            pos = end + 1;
            if (v.rfind(source.versionPrefix, 0) != 0) continue;
            const std::string tail = v.substr(source.versionPrefix.size());
            if (best.empty() || CompareDotted(tail, best) > 0) best = tail;
        }
        if (best.empty()) {
            throw std::runtime_error(source.loader + " 没有支持 Minecraft " + mc + " 的版本");
        }
        // best 是去掉前缀的构建号；NeoForge 的 maven 路径要完整版本号（21.1.266）。
        loaderVersion = source.versionPrefix + best;
    } else if (isNeo && loaderVersion.rfind(source.versionPrefix, 0) != 0) {
        loaderVersion = source.versionPrefix + loaderVersion;
    }

    // 2) 下载 installer 并取出 version.json
    const std::wstring full =
        Utf8ToWide(isNeo ? loaderVersion : (mc + "-" + loaderVersion));
    const std::wstring loaderW = Utf8ToWide(source.loader);
    const std::wstring jarUrl = source.artifactBase + L"/" + full + L"/" + loaderW + L"-" +
                                full + L"-installer.jar";

    const std::wstring tempDir = fs::UniqueTempPath(L"voxel-loader");
    if (!fs::EnsureDir(tempDir)) throw std::runtime_error("无法创建临时目录");
    const std::wstring jarPath = fs::Join(tempDir, L"installer.jar");

    std::wstring error;
    if (!net::GetFile(jarUrl, jarPath, settings, &error, [](uint64_t) {}, nullptr)) {
        fs::DeleteTree(tempDir);
        throw std::runtime_error(source.loader + " 安装包下载失败" +
                                 (error.empty() ? "" : "：" + WideToUtf8(error)));
    }

    // 镜像源上的 jar 有被替换的风险：maven artifact 都带 .sha1 旁文件，取得到就校验。
    // 取不到不拦（镜像不保证每个文件都有），别让一次镜像抖动挡住安装。
    {
        std::string expected;
        std::wstring shaError;
        if (net::GetText(jarUrl + L".sha1", settings, expected, &shaError)) {
            const size_t sp = expected.find_first_of(" \t\r\n");
            std::string hex = sp == std::string::npos ? expected : expected.substr(0, sp);
            for (auto& ch : hex) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
            if (hex.size() == 40 && crypto::Sha1FileHex(jarPath) != hex) {
                fs::DeleteTree(tempDir);
                throw std::runtime_error(source.loader + " 安装包校验失败（sha1 不符），请重试");
            }
        }
    }

    // 3) 用 bsdtar 解出 profile（installer 是 zip）
    //    1.13+ 的安装器根目录有 version.json；1.7.10～1.12.2 只有 install_profile.json，
    //    版本描述嵌在它的 versionInfo 字段里。只认 version.json 会让老版本 Forge 报
    //    "tar.exe 返回错误码 1"（成员不存在）。
    //    注意必须传 tar.exe 的完整路径：CreateProcessW 的第一个参数是程序路径，
    //    只写 "tar" 不会去 PATH 里找，会直接返回 ERROR_FILE_NOT_FOUND（错误码 2）。
    int exitCode = 0;
    std::wstring tarError;
    bool oldStyle = false;
    if (!proc::RunCapture(TarExe(), {L"-xf", jarPath, L"-C", tempDir, L"version.json"}, tempDir,
                          120000, &exitCode, &tarError)) {
        oldStyle = true;
        tarError.clear();
        if (!proc::RunCapture(TarExe(),
                              {L"-xf", jarPath, L"-C", tempDir, L"install_profile.json"}, tempDir,
                              120000, &exitCode, &tarError)) {
            fs::DeleteTree(tempDir);
            throw std::runtime_error("无法解包 " + source.loader + " 安装器" +
                                     (tarError.empty() ? "" : "：" + WideToUtf8(tarError)));
        }
    }

    const std::string raw =
        fs::ReadText(fs::Join(tempDir, oldStyle ? L"install_profile.json" : L"version.json"));
    if (raw.empty()) {
        fs::DeleteTree(tempDir);
        throw std::runtime_error(source.loader + " 安装器里没有 version.json 或 install_profile.json");
    }

    nlohmann::json profile = nlohmann::json::parse(raw, nullptr, false);
    if (profile.is_discarded() || !profile.is_object()) {
        fs::DeleteTree(tempDir);
        throw std::runtime_error(source.loader + " 的安装信息格式异常");
    }
    if (oldStyle) {
        if (!profile.contains("versionInfo") || !profile["versionInfo"].is_object()) {
            fs::DeleteTree(tempDir);
            throw std::runtime_error(source.loader + " 安装器缺少 versionInfo，无法生成版本");
        }
        profile = profile["versionInfo"];
    }

    // NeoForge 26.x：FML 启动时还要 universal jar 和 srg 客户端 jar，
    // 这两样由安装器的 processor 生成，必须在这里补上；否则版本能写出来但一启动就报损坏。
    if (isNeo && !oldStyle) {
        std::wstring runtimeError;
        const bool runtimeOk =
            EnsureNeoForgeRuntime(settings, gameDir, jarPath, mc, loaderVersion, &runtimeError);
        fs::DeleteTree(tempDir);
        if (!runtimeOk) {
            throw std::runtime_error(runtimeError.empty()
                                         ? std::string("NeoForge 运行时文件准备失败")
                                         : WideToUtf8(runtimeError));
        }
    } else {
        fs::DeleteTree(tempDir);
    }

    // 4) 落地成独立实例
    const std::string id = customName.empty()
                                ? InstanceIdFor(mc, source.loader, loaderVersion)
                                : customName;
    const int libraries = profile.contains("libraries") && profile["libraries"].is_array()
                              ? static_cast<int>(profile["libraries"].size())
                              : 0;
    WriteInstance(gameDir, id, RewriteProfile(profile, id, mc));

    return nlohmann::json{{"id", id},
                          {"loader", source.loader},
                          {"loaderVersion", loaderVersion},
                          {"librariesDownloaded", libraries}};
}

// Forge：版本号带 Minecraft 前缀（1.21.1-52.1.0）
NeoLikeSource ForgeSource() {
    return NeoLikeSource{
        "forge",
        L"https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/maven-metadata.xml",
        L"https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge",
        ""};
}

// NeoForge：版本号是 21.1.x 形式，maven 路径不带 /releases 前缀。
// 官方 maven.neoforged.net 在本机被阻断（TCP 443 超时），只有 BMCL 镜像可达。
NeoLikeSource NeoForgeSource() {
    return NeoLikeSource{
        "neoforge",
        L"https://bmclapi2.bangbang93.com/maven/net/neoforged/neoforge/maven-metadata.xml",
        L"https://bmclapi2.bangbang93.com/maven/net/neoforged/neoforge",
        ""};
}

// NeoForge 的版本号由 Minecraft 版本推出，两种规则：
//   1.x.y 时代：去掉开头的 "1." → 1.21 → 21.0.，1.21.1 → 21.1.
//   26.x 时代：直接沿用完整版本号 → 26.1.2.126、26.3.0.x
std::string NeoForgePrefixFor(const std::string& mc) {
    // 只接受纯数字 + 点的版本号，避免把 26w01a 这类快照当成版本
    for (char ch : mc) {
        if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '.') return "";
    }
    // "1.21.1" → major=1, minor=21, patch=1
    int major = 0, minor = 0, patch = 0;
    if (std::sscanf(mc.c_str(), "%d.%d.%d", &major, &minor, &patch) < 2) return "";
    if (major == 1) return std::to_string(minor) + "." + std::to_string(patch) + ".";
    return mc + ".";
}

nlohmann::json ListLoaders(const nlohmann::json& settings, const std::string& mcVersion) {
    nlohmann::json result = nlohmann::json::object();

    // 原版始终可用
    result["vanilla"] = LoaderOption{"vanilla", "原版", mcVersion, "纯净的 Minecraft 体验", true}.ToJson();

    // Fabric：查得到的第一个稳定版就是"最新"
    LoaderOption fabric{"fabric", "Fabric", "", "轻量、更新快的模组加载器", false};
    if (!mcVersion.empty()) {
        std::wstring error;
        std::string body;
        const std::wstring url = ReplaceAll(kFabricGame, L"{mc}", Utf8ToWide(mcVersion));
        if (net::GetText(url, settings, body, &error)) {
            nlohmann::json list = nlohmann::json::parse(body, nullptr, false);
            auto stable = FabricLoaderVersions(list, true);
            if (stable.empty()) stable = FabricLoaderVersions(list, false);
            if (!stable.empty()) {
                fabric.latestVersion = stable.front();
                fabric.detail = "最新 " + fabric.latestVersion;
                fabric.available = true;
            } else {
                fabric.detail = "该版本暂不支持 Fabric";
            }
        } else {
            fabric.detail = "无法获取 Fabric 版本，请检查网络或镜像设置";
        }
    } else {
        fabric.detail = "请先选择 Minecraft 版本";
    }
    result["fabric"] = fabric.ToJson();

    // Forge：从 maven-metadata 里挑该 Minecraft 版本对应的最新 Forge
    LoaderOption forge{"forge", "Forge", "", "老牌模组加载器，生态最广", false};
    if (!mcVersion.empty()) {
        std::wstring error;
        std::string body;
        if (net::GetText(ForgeSource().metaUrl, settings, body, &error)) {
            const std::string prefix = mcVersion + "-";
            std::string best;
            size_t pos = 0;
            while ((pos = body.find("<version>", pos)) != std::string::npos) {
                const size_t end = body.find("</version>", pos);
                if (end == std::string::npos) break;
                const std::string v = body.substr(pos + 9, end - pos - 9);
                pos = end + 1;
                if (v.rfind(prefix, 0) != 0) continue;
                const std::string tail = v.substr(prefix.size());
                if (best.empty() || CompareDotted(tail, best) > 0) best = tail;
            }
            if (!best.empty()) {
                forge.latestVersion = best;
                forge.detail = "最新 " + best;
                forge.available = true;
            } else {
                forge.detail = "该版本暂无 Forge";
            }
        } else {
            forge.detail = "无法获取 Forge 版本，请检查网络";
        }
    } else {
        forge.detail = "请先选择 Minecraft 版本";
    }
    result["forge"] = forge.ToJson();

    // NeoForge：版本号形如 21.1.266，前两段由 Minecraft 版本推出（1.21.1 → 21.1.）
    LoaderOption neoforge{"neoforge", "NeoForge", "", "Forge 的社区分支，更新更快", false};
    const std::string neoPrefix = NeoForgePrefixFor(mcVersion);
    if (!mcVersion.empty() && !neoPrefix.empty()) {
        std::wstring error;
        std::string body;
        if (net::GetText(NeoForgeSource().metaUrl, settings, body, &error)) {
            std::string best;
            size_t pos = 0;
            while ((pos = body.find("<version>", pos)) != std::string::npos) {
                const size_t end = body.find("</version>", pos);
                if (end == std::string::npos) break;
                const std::string v = body.substr(pos + 9, end - pos - 9);
                pos = end + 1;
                if (v.rfind(neoPrefix, 0) != 0) continue;
                const std::string tail = v.substr(neoPrefix.size());
                if (best.empty() || CompareDotted(tail, best) > 0) best = tail;
            }
            if (!best.empty()) {
                // 必须回传完整版本号（26.1.2.126），不能只给构建号（126）：
                // 安装时直接拿它拼 maven 路径，缺前缀就是 404。
                neoforge.latestVersion = neoPrefix + best;
                neoforge.detail = "最新 " + neoforge.latestVersion;
                neoforge.available = true;
            } else {
                neoforge.detail = "该版本暂无 NeoForge";
            }
        } else {
            neoforge.detail = "无法获取 NeoForge 版本，请检查网络";
        }
    } else if (!mcVersion.empty()) {
        neoforge.detail = "该 Minecraft 版本暂不支持 NeoForge";
    } else {
        neoforge.detail = "请先选择 Minecraft 版本";
    }
    result["neoforge"] = neoforge.ToJson();

    return result;
}

nlohmann::json InstallLoader(const nlohmann::json& settings, const std::wstring& gameDir,
                             const std::string& mcVersion, const std::string& loader,
                             const std::string& loaderVersion, const std::string& customName) {
    if (mcVersion.empty()) throw std::runtime_error("请先选择一个 Minecraft 版本");
    if (!SafeVersionId(mcVersion)) throw std::runtime_error("Minecraft 版本名非法");
    // 传进来的可能本身就是加载器实例：统一回溯到原版，避免实例嵌套继承。
    const std::string baseVersion = BaseMinecraftVersion(gameDir, mcVersion);
    if (loader.empty() || loader == "vanilla") {
        throw std::runtime_error("原版无需安装加载器");
    }

    // 用户自定义的实例名：为空时用默认的 <mc>-<loader>-<ver>。
    // 一旦给名字，就直接当目录名——同一个游戏版本 + 同一个加载器想装两份时，
    // 靠它区分，否则两者会写进同一个目录互相覆盖。
    if (!customName.empty()) {
        if (!SafeVersionId(customName)) {
            throw std::runtime_error("实例名不能包含 \\ / : * ? \" < > | 等字符");
        }
        const std::wstring dir = VersionDir(gameDir, customName);
        if (fs::Exists(fs::Join(dir, Utf8ToWide(customName) + L".json"))) {
            throw std::runtime_error("实例 " + customName + " 已存在，请换一个名字");
        }
    }

    if (loader == "fabric") {
        // 已经装过同一个组合时不重复下载：先看目标版本目录是否已存在。
        // 指定了版本就直接判；没指定则先把最新稳定版解析出来再判。
        if (!loaderVersion.empty()) {
            const std::string id = customName.empty()
                                        ? InstanceIdFor(baseVersion, "fabric", loaderVersion)
                                        : customName;
            if (fs::Exists(fs::Join(VersionDir(gameDir, id), Utf8ToWide(id) + L".json"))) {
                return nlohmann::json{{"id", id},
                                      {"loader", "fabric"},
                                      {"loaderVersion", loaderVersion},
                                      {"alreadyInstalled", true},
                                      {"librariesDownloaded", 0}};
            }
        }

        const nlohmann::json installed =
            InstallFabric(settings, gameDir, baseVersion, loaderVersion, customName);
        return nlohmann::json{{"id", installed.value("id", std::string())},
                              {"loader", "fabric"},
                              {"loaderVersion", installed.value("loaderVersion", std::string())},
                              {"alreadyInstalled", false},
                              {"librariesDownloaded", installed.value("librariesDownloaded", 0)}};
    }

    if (loader == "forge" || loader == "neoforge") {
        const bool isNeo = loader == "neoforge";
        NeoLikeSource source = isNeo ? NeoForgeSource() : ForgeSource();
        std::string resolvedVersion = loaderVersion;
        if (isNeo) {
            source.versionPrefix = NeoForgePrefixFor(baseVersion);
            if (source.versionPrefix.empty()) {
                throw std::runtime_error("无法从 Minecraft " + baseVersion +
                                         " 推断 NeoForge 版本号（需要 1.21.1 或 26.1.2 这类数字版本）");
            }
            // 前端可能只传来构建号（266 / 126）：补回完整版本号再拼 maven 路径。
            if (!resolvedVersion.empty() && resolvedVersion.rfind(source.versionPrefix, 0) != 0) {
                resolvedVersion = source.versionPrefix + resolvedVersion;
            }
        }

        if (!resolvedVersion.empty()) {
            const std::string id = customName.empty()
                                        ? InstanceIdFor(baseVersion, loader, resolvedVersion)
                                        : customName;
            if (fs::Exists(fs::Join(VersionDir(gameDir, id), Utf8ToWide(id) + L".json"))) {
                return nlohmann::json{{"id", id},
                                      {"loader", loader},
                                      {"loaderVersion", resolvedVersion},
                                      {"alreadyInstalled", true},
                                      {"librariesDownloaded", 0}};
            }
        }

        const nlohmann::json installed =
            InstallNeoLike(settings, gameDir, baseVersion, source, resolvedVersion, customName);
        return nlohmann::json{{"id", installed.value("id", std::string())},
                              {"loader", loader},
                              {"loaderVersion", installed.value("loaderVersion", std::string())},
                              {"alreadyInstalled", false},
                              {"librariesDownloaded", installed.value("librariesDownloaded", 0)}};
    }

    throw std::runtime_error("未知的加载器：" + loader);
}

}  // namespace voxel::loader
