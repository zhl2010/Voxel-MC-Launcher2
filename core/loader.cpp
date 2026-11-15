#include "loader.h"

#include "crypto.h"
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

// Forge / NeoForge：从官方 installer jar 里取出 version.json 作为版本元数据。
//
// 这条路避开了"跑 processor"的重活——installer 里的 version.json 已经是
// 完整的版本描述（mainClass、libraries、arguments），inheritsFrom 指回原版，
// 和 Fabric 一样可以直接落地成一个可启动的实例。
//
// processor 只负责把原版 jar 反混淆成 srg 名字，1.17+ 的 Forge/NeoForge
// 由各自的 bootstrap 在启动时处理，所以元数据落地即可用。
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
        loaderVersion = best;
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
    fs::DeleteTree(tempDir);
    if (raw.empty()) {
        throw std::runtime_error(source.loader + " 安装器里没有 version.json 或 install_profile.json");
    }

    nlohmann::json profile = nlohmann::json::parse(raw, nullptr, false);
    if (profile.is_discarded() || !profile.is_object()) {
        throw std::runtime_error(source.loader + " 的安装信息格式异常");
    }
    if (oldStyle) {
        if (!profile.contains("versionInfo") || !profile["versionInfo"].is_object()) {
            throw std::runtime_error(source.loader + " 安装器缺少 versionInfo，无法生成版本");
        }
        profile = profile["versionInfo"];
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

// NeoForge 的版本号前两位由 Minecraft 版本推出：1.21.1 → 21.1.
std::string NeoForgePrefixFor(const std::string& mc) {
    // "1.21.1" → major=1, minor=21, patch=1
    int major = 0, minor = 0, patch = 0;
    char extra = 0;
    if (std::sscanf(mc.c_str(), "%d.%d.%d%c", &major, &minor, &patch, &extra) < 2) return "";
    if (major != 1) return "";
    // 1.21 → 21.0.，1.21.1 → 21.1.
    return std::to_string(minor) + "." + std::to_string(patch) + ".";
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
                neoforge.latestVersion = best;
                neoforge.detail = "最新 " + best;
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
                                        ? InstanceIdFor(mcVersion, "fabric", loaderVersion)
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
            InstallFabric(settings, gameDir, mcVersion, loaderVersion, customName);
        return nlohmann::json{{"id", installed.value("id", std::string())},
                              {"loader", "fabric"},
                              {"loaderVersion", installed.value("loaderVersion", std::string())},
                              {"alreadyInstalled", false},
                              {"librariesDownloaded", installed.value("librariesDownloaded", 0)}};
    }

    if (loader == "forge" || loader == "neoforge") {
        const bool isNeo = loader == "neoforge";
        if (!loaderVersion.empty()) {
            const std::string id = customName.empty()
                                        ? InstanceIdFor(mcVersion, loader, loaderVersion)
                                        : customName;
            if (fs::Exists(fs::Join(VersionDir(gameDir, id), Utf8ToWide(id) + L".json"))) {
                return nlohmann::json{{"id", id},
                                      {"loader", loader},
                                      {"loaderVersion", loaderVersion},
                                      {"alreadyInstalled", true},
                                      {"librariesDownloaded", 0}};
            }
        }

        NeoLikeSource source = isNeo ? NeoForgeSource() : ForgeSource();
        if (isNeo) {
            source.versionPrefix = NeoForgePrefixFor(mcVersion);
            if (source.versionPrefix.empty()) {
                throw std::runtime_error("无法从 Minecraft " + mcVersion +
                                         " 推断 NeoForge 版本号（需要 1.x.y 形式）");
            }
        }

        const nlohmann::json installed =
            InstallNeoLike(settings, gameDir, mcVersion, source, loaderVersion, customName);
        return nlohmann::json{{"id", installed.value("id", std::string())},
                              {"loader", loader},
                              {"loaderVersion", installed.value("loaderVersion", std::string())},
                              {"alreadyInstalled", false},
                              {"librariesDownloaded", installed.value("librariesDownloaded", 0)}};
    }

    throw std::runtime_error("未知的加载器：" + loader);
}

}  // namespace voxel::loader
