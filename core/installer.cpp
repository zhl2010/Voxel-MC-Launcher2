#include "installer.h"

#include "config.h"
#include "crypto.h"
#include "fs.h"
#include "manifest.h"
#include "proc.h"
#include "util.h"

#include "../net/http.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace voxel {
namespace {

// 并发下载数。资源文件大多很小（几 KB～几十 KB），瓶颈在往返延迟而不是带宽；
// 连接复用之后单个请求约 30–50ms，32 并发能把这部分开销摊平。
constexpr int PARALLEL_DOWNLOADS = 32;

bool UnsafeIdChar(char ch) {
    return ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
           ch == '<' || ch == '>' || ch == '|' || ch == '\r' || ch == '\n';
}

std::wstring LowerKey(const std::wstring& text) {
    std::wstring out = text;
    for (auto& ch : out) ch = static_cast<wchar_t>(::towlower(ch));
    return out;
}

// manifest 是外部输入：artifact 的 path 一旦带上 ".." 或盘符，就能把文件写到
// 游戏目录之外，形成任意位置写入。所以逐段校验，任何可疑分段整体拒绝。
bool SafeRelativeSegment(const std::string& part) {
    if (part.empty()) return false;
    if (part == "." || part == "..") return false;
    for (char ch : part) {
        if (ch == ':' || ch == '\\' || ch == '\0') return false;
        if (static_cast<unsigned char>(ch) < 0x20) return false;
    }
    return true;
}

// 返回空串表示路径非法（绝对路径、盘符、穿越），调用方必须跳过该条目。
std::wstring FromSlashPath(const std::wstring& base, const std::string& relative) {
    if (relative.empty()) return std::wstring();
    if (relative[0] == '/' || relative[0] == '\\') return std::wstring();       // 绝对路径
    if (relative.size() >= 2 && relative[1] == ':') return std::wstring();      // 盘符

    std::wstring current = base;
    size_t index = 0;
    while (index < relative.size()) {
        const size_t next = relative.find_first_of("/\\", index);
        const size_t end = next == std::string::npos ? relative.size() : next;
        const std::string part = relative.substr(index, end - index);
        if (!part.empty()) {
            if (!SafeRelativeSegment(part)) return std::wstring();
            current = fs::Join(current, Utf8ToWide(part));
        }
        if (next == std::string::npos) break;
        index = next + 1;
    }
    return current;
}

bool RuleMatches(const nlohmann::json& rule) {
    if (!rule.is_object()) return false;
    if (!rule.contains("os") || !rule["os"].is_object()) return true;
    const nlohmann::json& os = rule["os"];
    const std::string name = os.value("name", std::string());
    if (!name.empty() && name != "windows") return false;
    const std::string arch = os.value("arch", std::string());
    if (!arch.empty() && arch != "x64") return false;
    if (os.contains("version") && os["version"].is_string()) {
        // "^10." 这类规则：只比较主版本号，Win10/11 都算满足
        const std::string version = os["version"].get<std::string>();
        size_t index = 0;
        while (index < version.size() && !isdigit(static_cast<unsigned char>(version[index]))) ++index;
        int required = 0;
        while (index < version.size() && isdigit(static_cast<unsigned char>(version[index]))) {
            required = required * 10 + (version[index] - '0');
            ++index;
        }
        if (required > 0 && required > 10) return false;
    }
    return true;
}

bool RulesAllow(const nlohmann::json& rules) {
    if (!rules.is_array() || rules.empty()) return true;
    bool allowed = false;
    for (const auto& rule : rules) {
        if (!RuleMatches(rule)) continue;
        allowed = rule.value("action", std::string("allow")) == "allow";
    }
    return allowed;
}

struct ArtifactRef {
    std::string path;
    std::string url;
    std::string sha1;
    uint64_t size = 0;
    bool valid = false;
};

ArtifactRef ReadArtifact(const nlohmann::json& node) {
    ArtifactRef artifact;
    if (!node.is_object()) return artifact;
    if (!node.contains("path") || !node["path"].is_string()) return artifact;
    artifact.path = node["path"].get<std::string>();
    artifact.url = node.value("url", std::string());
    artifact.sha1 = node.value("sha1", std::string());
    artifact.size = node.value("size", static_cast<uint64_t>(0));
    artifact.valid = true;
    return artifact;
}

ArtifactRef GuessArtifact(const std::string& name) {
    // group:artifact:version[:classifier] → libraries/group/path/artifact/version/...
    ArtifactRef artifact;
    std::vector<std::string> parts;
    std::string token;
    for (char ch : name) {
        if (ch == ':') {
            parts.push_back(token);
            token.clear();
        } else {
            token.push_back(ch);
        }
    }
    parts.push_back(token);
    if (parts.size() < 3) return artifact;

    std::string relative;
    for (size_t i = 0; i < parts[0].size(); ++i) {
        char ch = parts[0][i];
        relative.push_back(ch == '.' ? '/' : ch);
    }
    const std::string file = parts[1] + "-" + parts[2] +
                             (parts.size() > 3 ? "-" + parts[3] : "") + ".jar";
    artifact.path = relative + "/" + parts[1] + "/" + parts[2] + "/" + file;
    artifact.valid = true;  // 没有 URL，只能检查本地是否存在
    return artifact;
}

class PlanBuilder {
  public:
    PlanBuilder(VersionPlan& plan) : plan_(plan) {}

    void AddFile(const std::string& type, const std::wstring& dest, const std::string& url,
                 const std::string& sha1, uint64_t size, bool checkHash) {
        const std::wstring key = LowerKey(dest);
        if (!seen_.insert(key).second) return;

        PlanItem item;
        item.dest = dest;
        item.url = Utf8ToWide(url);
        item.sha1 = sha1;
        item.size = size;
        item.type = type;
        item.checkHash = checkHash;

        if (fs::Exists(dest)) {
            const bool sizeOk = size == 0 || fs::FileSize(dest) == size;
            bool hashOk = true;
            if (checkHash && !sha1.empty() && sizeOk) {
                hashOk = crypto::Sha1FileHex(dest) == sha1;
            }
            if (sizeOk && hashOk) {
                ++plan_.present;
                return;
            }
            // 尺寸/哈希不符的文件在磁盘上留着只会让下次启动继续跳过，直接重下
            ++plan_.corrupt;
        } else {
            ++plan_.missing;
        }

        if (item.url.empty()) {
            // 没有下载地址：只能报缺失，不能凭空生成
            plan_.notes.push_back(L"缺少下载地址，无法修复：" + dest);
            return;
        }
        plan_.downloads.push_back(item);
        plan_.totalBytes += size;
    }

  private:
    VersionPlan& plan_;
    std::unordered_set<std::wstring> seen_;
};

nlohmann::json MergeVersionJson(const nlohmann::json& parent, const nlohmann::json& child) {
    nlohmann::json merged = parent;
    for (auto it = child.begin(); it != child.end(); ++it) {
        merged[it.key()] = it.value();
    }
    merged["id"] = child.value("id", parent.value("id", std::string()));

    nlohmann::json libraries = nlohmann::json::array();
    if (parent.contains("libraries") && parent["libraries"].is_array()) {
        for (const auto& entry : parent["libraries"]) libraries.push_back(entry);
    }
    if (child.contains("libraries") && child["libraries"].is_array()) {
        for (const auto& entry : child["libraries"]) libraries.push_back(entry);
    }
    merged["libraries"] = libraries;

    if (parent.contains("arguments") && child.contains("arguments")) {
        nlohmann::json arguments = child["arguments"];
        for (const char* key : {"game", "jvm"}) {
            nlohmann::json list = nlohmann::json::array();
            if (parent["arguments"].contains(key) && parent["arguments"][key].is_array()) {
                for (const auto& entry : parent["arguments"][key]) list.push_back(entry);
            }
            if (child["arguments"].contains(key) && child["arguments"][key].is_array()) {
                for (const auto& entry : child["arguments"][key]) list.push_back(entry);
            }
            arguments[key] = list;
        }
        merged["arguments"] = arguments;
    }
    if (!child.contains("minecraftArguments") && parent.contains("minecraftArguments")) {
        merged["minecraftArguments"] = parent["minecraftArguments"];
    }
    if (!child.contains("assetIndex") && parent.contains("assetIndex")) {
        merged["assetIndex"] = parent["assetIndex"];
    }
    if (!child.contains("assets") && parent.contains("assets")) merged["assets"] = parent["assets"];
    if (!child.contains("javaVersion") && parent.contains("javaVersion")) {
        merged["javaVersion"] = parent["javaVersion"];
    }
    if (!child.contains("downloads") && parent.contains("downloads")) {
        merged["downloads"] = parent["downloads"];
    } else if (child.contains("downloads") && parent.contains("downloads") &&
               child["downloads"].is_object() && parent["downloads"].is_object()) {
        nlohmann::json downloads = parent["downloads"];
        for (auto it = child["downloads"].begin(); it != child["downloads"].end(); ++it) {
            downloads[it.key()] = it.value();
        }
        merged["downloads"] = downloads;
    }
    merged.erase("inheritsFrom");
    return merged;
}

bool LoadVersionJsonFromDisk(const std::wstring& gameDir, const std::string& id,
                             nlohmann::json& out) {
    const std::wstring path =
        fs::Join(fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(id)), Utf8ToWide(id) + L".json");
    const std::string raw = fs::ReadText(path);
    if (raw.empty()) return false;
    out = nlohmann::json::parse(raw, nullptr, false);
    return !out.is_discarded() && out.is_object();
}

// 找资产索引文件；assetIndex.id 是索引文件名（如 35），不是版本号
bool ReadAssetIndex(const ResolvedVersion& version, const std::wstring& gameDir, nlohmann::json& out) {
    const nlohmann::json& manifest = version.manifest;
    if (!manifest.contains("assetIndex") || !manifest["assetIndex"].is_object()) return false;
    const nlohmann::json& index = manifest["assetIndex"];
    const std::string id = index.value("id", std::string());
    if (id.empty()) return false;
    const std::wstring path = fs::Join(fs::Join(gameDir, L"assets"), fs::Join(L"indexes", Utf8ToWide(id) + L".json"));
    const std::string raw = fs::ReadText(path);
    if (raw.empty()) return false;
    out = nlohmann::json::parse(raw, nullptr, false);
    return !out.is_discarded() && out.is_object();
}

std::wstring NativesDir(const std::wstring& gameDir, const std::string& id) {
    return fs::Join(fs::Join(gameDir, L"bin"), Utf8ToWide(id));
}

bool CopyFlat(const std::wstring& sourceDir, const std::wstring& targetDir) {
    bool ok = true;
    for (const std::wstring& file : fs::ListFiles(sourceDir, true)) {
        if (LowerKey(file).find(L"\\meta-inf\\") != std::wstring::npos) continue;
        const std::wstring name = fs::BaseName(file);
        if (name.rfind(L".natives-", 0) == 0) continue;
        const std::wstring dest = fs::Join(targetDir, name);
        if (!CopyFileW(file.c_str(), dest.c_str(), FALSE)) {
            // 已存在的同名 dll 直接覆盖（可能是旧版本残留）
            if (!CopyFileW(file.c_str(), dest.c_str(), TRUE)) ok = false;
        }
    }
    return ok;
}

}  // namespace

// 系统自带 bsdtar 的完整路径。CreateProcessW 的第一个参数是程序路径，
// 传 "tar" 不会去 PATH 里找，会直接 ERROR_FILE_NOT_FOUND。
std::wstring TarExe() {
    wchar_t systemDir[MAX_PATH]{};
    if (GetSystemDirectoryW(systemDir, MAX_PATH) > 0) {
        const std::wstring candidate = fs::Join(systemDir, L"tar.exe");
        if (fs::Exists(candidate)) return candidate;
    }
    return L"tar.exe";
}

bool SafeVersionId(const std::string& id) {
    if (id.empty() || id == "." || id == "..") return false;
    for (char ch : id) {
        if (UnsafeIdChar(ch)) return false;
    }
    return true;
}

bool RulesAllowJson(const nlohmann::json& rules) { return RulesAllow(rules); }

ResolvedVersion ResolveVersion(const std::wstring& gameDir, const nlohmann::json& settings,
                               const std::string& id, std::wstring* error) {
    ResolvedVersion result;
    result.id = id;
    result.versionDir = fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(id));

    if (!SafeVersionId(id)) {
        if (error) *error = L"版本 id 非法：" + Utf8ToWide(id);
        return result;
    }

    std::vector<VersionLevel> chain;  // 叶子在前
    std::unordered_set<std::string> seen;
    std::string current = id;

    while (!current.empty()) {
        if (seen.count(current)) {
            if (error) *error = L"版本 " + Utf8ToWide(current) + L" 的 inheritsFrom 存在循环";
            return ResolvedVersion{};
        }
        seen.insert(current);

        nlohmann::json json;
        bool local = LoadVersionJsonFromDisk(gameDir, current, json);
        if (!local) {
            // 本地没有（典型是 Fabric/Forge 实例的原版父版本），按需联网取一份
            std::wstring netError;
            json = FetchVersionJson(settings, current, &netError);
            if (json.is_null() || json.empty()) {
                if (error) {
                    *error = L"无法解析版本 " + Utf8ToWide(id) + L"：缺少父版本 " +
                             Utf8ToWide(current) +
                             (netError.empty() ? L"" : L"（" + netError + L"）");
                }
                return ResolvedVersion{};
            }
            local = false;
        }

        VersionLevel level;
        level.id = current;
        level.json = json;
        level.local = local;
        chain.push_back(level);

        current = json.value("inheritsFrom", std::string());
    }

    // 父版本在前，逐层合并
    nlohmann::json merged;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        merged = merged.is_null() || merged.empty() ? it->json : MergeVersionJson(merged, it->json);
        result.levels.push_back(*it);
    }
    std::reverse(result.levels.begin(), result.levels.end());

    merged["id"] = id;
    result.manifest = merged;

    // 客户端 jar 属于"提供 downloads.client 的最底层"，也就是原版那一层
    result.jarId = id;
    for (const auto& level : result.levels) {
        if (level.json.contains("downloads") && level.json["downloads"].is_object() &&
            level.json["downloads"].contains("client")) {
            result.jarId = level.id;
            break;
        }
    }
    if (result.manifest.contains("jar") && result.manifest["jar"].is_string()) {
        result.jarId = result.manifest["jar"].get<std::string>();
    }

    result.javaMajor = 8;
    if (merged.contains("javaVersion") && merged["javaVersion"].is_object()) {
        result.javaMajor = merged["javaVersion"].value("majorVersion", 8);
    }
    return result;
}

VersionPlan BuildPlan(const ResolvedVersion& version, const nlohmann::json& settings,
                      const std::wstring& gameDir, bool verifyHashes, bool includeAssets) {
    (void)settings;
    VersionPlan plan;
    PlanBuilder builder(plan);

    // 1. 各层版本的客户端 jar
    for (const auto& level : version.levels) {
        if (!level.json.contains("downloads") || !level.json["downloads"].is_object()) continue;
        if (!level.json["downloads"].contains("client")) continue;
        const ArtifactRef client = ReadArtifact(level.json["downloads"]["client"]);
        if (!client.valid) continue;
        const std::wstring dest =
            fs::Join(fs::Join(gameDir, L"versions"), fs::Join(Utf8ToWide(level.id),
                                                              Utf8ToWide(level.id) + L".jar"));
        builder.AddFile("游戏本体", dest, client.url, client.sha1, client.size, verifyHashes);
    }

    // 2. 运行库（规则过滤 + natives 分类器）
    std::string nativeKey = "windows";
    if (version.manifest.contains("libraries") && version.manifest["libraries"].is_array()) {
        for (const auto& library : version.manifest["libraries"]) {
            if (!library.is_object()) continue;
            if (library.contains("rules") && !RulesAllow(library["rules"])) continue;

            const std::string name = library.value("name", std::string());
            const nlohmann::json downloads =
                library.contains("downloads") ? library["downloads"] : nlohmann::json::object();

            bool plannedArtifact = false;
            if (downloads.is_object() && downloads.contains("artifact")) {
                const ArtifactRef artifact = ReadArtifact(downloads["artifact"]);
                if (artifact.valid) {
                    const std::wstring dest =
                        FromSlashPath(fs::Join(gameDir, L"libraries"), artifact.path);
                    if (!dest.empty()) {
                        builder.AddFile("运行库", dest, artifact.url, artifact.sha1, artifact.size,
                                        verifyHashes);
                        plannedArtifact = true;
                    }
                }
            }

            if (library.contains("natives") && library["natives"].is_object() &&
                library["natives"].contains(nativeKey)) {
                std::string key = library["natives"][nativeKey].get<std::string>();
                const std::string archToken = "${arch}";
                const size_t pos = key.find(archToken);
                if (pos != std::string::npos) key.replace(pos, archToken.size(), "64");

                ArtifactRef native;
                if (downloads.is_object() && downloads.contains("classifiers") &&
                    downloads["classifiers"].is_object() &&
                    downloads["classifiers"].contains(key)) {
                    native = ReadArtifact(downloads["classifiers"][key]);
                }
                if (native.valid) {
                    const std::wstring dest =
                        FromSlashPath(fs::Join(gameDir, L"libraries"), native.path);
                    if (!dest.empty()) {
                        builder.AddFile("运行库", dest, native.url, native.sha1, native.size,
                                        verifyHashes);
                        plan.nativeJars.push_back(dest);
                    }
                } else {
                    plan.notes.push_back(L"无法定位 native 包：" + Utf8ToWide(name));
                }
            }

            if (!plannedArtifact && !name.empty() && !library.contains("natives")) {
                // Fabric 的 profile JSON 不带 downloads 块，库条目只有 name + url（maven 根）
                // + 可选 sha1/size：按 maven 坐标布局拼出下载地址。拼不出地址才退回
                // "只检查本地是否存在"的老路径。
                const ArtifactRef guessed = GuessArtifact(name);
                if (guessed.valid) {
                    const std::wstring dest =
                        FromSlashPath(fs::Join(gameDir, L"libraries"), guessed.path);
                    if (!dest.empty()) {
                        std::string mavenRoot = library.value("url", std::string());
                        if (!mavenRoot.empty()) {
                            if (mavenRoot.back() != '/') mavenRoot.push_back('/');
                            builder.AddFile("运行库", dest, mavenRoot + guessed.path,
                                            library.value("sha1", std::string()),
                                            library.value("size", uint64_t{0}), verifyHashes);
                        } else {
                            builder.AddFile("运行库", dest, std::string(), std::string(), 0, false);
                        }
                    }
                }
            }
        }
    }

    // 3. 资源索引本身
    if (version.manifest.contains("assetIndex") && version.manifest["assetIndex"].is_object()) {
        const nlohmann::json& index = version.manifest["assetIndex"];
        const std::string indexId = index.value("id", std::string());
        if (!indexId.empty()) {
            const std::wstring dest = fs::Join(fs::Join(gameDir, L"assets"),
                                               fs::Join(L"indexes", Utf8ToWide(indexId) + L".json"));
            builder.AddFile("配置文件", dest, index.value("url", std::string()),
                            index.value("sha1", std::string()), index.value("size", uint64_t{0}),
                            verifyHashes);
        }
    }

    // 4. 日志配置（log4j2.xml），没有它现代版本会刷红字警告
    if (version.manifest.contains("logging") && version.manifest["logging"].is_object() &&
        version.manifest["logging"].contains("client") &&
        version.manifest["logging"]["client"].is_object() &&
        version.manifest["logging"]["client"].contains("file")) {
        const nlohmann::json& file = version.manifest["logging"]["client"]["file"];
        const std::string fileId = file.value("id", std::string());
        if (!fileId.empty()) {
            const std::wstring dest = fs::Join(fs::Join(gameDir, L"assets"),
                                               fs::Join(L"log_configs", Utf8ToWide(fileId)));
            builder.AddFile("配置文件", dest, file.value("url", std::string()),
                            file.value("sha1", std::string()), file.value("size", uint64_t{0}),
                            verifyHashes);
        }
    }

    // 5. 资源对象（依赖索引已在本地）
    if (includeAssets) {
        nlohmann::json index;
        if (ReadAssetIndex(version, gameDir, index)) {
            if (index.value("map_to_resources", false)) {
                plan.notes.push_back(L"该版本使用旧版资源目录（pre-1.6），仅按 objects 安装");
            }
            if (index.contains("objects") && index["objects"].is_object()) {
                const std::wstring objectsRoot = fs::Join(fs::Join(gameDir, L"assets"), L"objects");
                for (auto it = index["objects"].begin(); it != index["objects"].end(); ++it) {
                    const nlohmann::json& object = it.value();
                    const std::string hash = object.value("hash", std::string());
                    if (hash.size() < 4) continue;
                    // hash 直接拼进本地路径和下载 URL：只认 hex 字符，
                    // 别让索引里的 "../"、盘符之类内容写穿目录
                    bool hexOk = true;
                    for (char ch : hash) {
                        const bool digit = ch >= '0' && ch <= '9';
                        const bool alpha = (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
                        if (!digit && !alpha) {
                            hexOk = false;
                            break;
                        }
                    }
                    if (!hexOk) continue;
                    const std::string prefix = hash.substr(0, 2);
                    const std::wstring dest = fs::Join(fs::Join(objectsRoot, Utf8ToWide(prefix)),
                                                       Utf8ToWide(hash));
                    // 资源文件以 hash 命名，尺寸命中就够了；再对 5000+ 文件做 SHA1 会让每次启动多花几秒
                    builder.AddFile("资源文件", dest,
                                    "https://resources.download.minecraft.net/" + prefix + "/" + hash,
                                    std::string(), object.value("size", uint64_t{0}), false);
                }
            }
        } else {
            plan.notes.push_back(L"资源索引缺失，无法检查 assets");
        }
    }

    return plan;
}

bool DownloadPlan(const VersionPlan& plan, const nlohmann::json& settings,
                  std::atomic<bool>* cancel, const ProgressFn& progress, std::wstring* error) {
    if (plan.downloads.empty()) {
        if (progress) {
            progress(nlohmann::json{{"stage", "download"}, {"percent", 100}, {"finished", true},
                                    {"overallTotalBytes", 0}, {"overallDownloadedBytes", 0},
                                    {"overallTotalFiles", 0}, {"overallDownloadedFiles", 0}});
        }
        return true;
    }

    const int totalFiles = static_cast<int>(plan.downloads.size());
    const uint64_t totalBytes = plan.totalBytes;

    // 类型标签取字节数最多的那一类，UI 上显示"正在下载 运行库"更具体
    std::string dominantType;
    {
        std::map<std::string, uint64_t> bytesByType;
        for (const auto& item : plan.downloads) bytesByType[item.type] += item.size;
        uint64_t best = 0;
        for (const auto& entry : bytesByType) {
            if (entry.second > best) {
                best = entry.second;
                dominantType = entry.first;
            }
        }
        if (bytesByType.size() > 1) dominantType = "游戏组件";
    }

    struct Shared {
        std::atomic<size_t> next{0};
        std::atomic<uint64_t> bytes{0};
        std::atomic<int> done{0};
        std::atomic<bool> failed{false};
        std::mutex errorMutex;
        std::wstring error;
    } shared;

    auto recordError = [&shared](const std::wstring& message) {
        std::lock_guard<std::mutex> lock(shared.errorMutex);
        if (shared.error.empty()) shared.error = message;
        shared.failed = true;
    };

    const int workerCount =
        std::min<int>(PARALLEL_DOWNLOADS, static_cast<int>(plan.downloads.size()));
    std::vector<std::thread> workers;
    for (int index = 0; index < workerCount; ++index) {
        workers.emplace_back([&]() {
            for (;;) {
                if (shared.failed.load()) return;
                if (cancel && cancel->load()) {
                    recordError(L"已取消");
                    return;
                }
                const size_t taskIndex = shared.next.fetch_add(1);
                if (taskIndex >= plan.downloads.size()) return;

                const PlanItem& item = plan.downloads[taskIndex];
                std::wstring localError;
                bool ok = net::GetFile(
                    item.url, item.dest, settings, &localError,
                    [&shared](uint64_t bytes) { shared.bytes.fetch_add(bytes); }, cancel);
                if (!ok) {
                    const std::wstring message =
                        L"下载失败：" + fs::BaseName(item.dest) +
                        (localError.empty() ? L"" : L"（" + localError + L"）");
                    recordError(message);
                    return;
                }

                if (item.checkHash && !item.sha1.empty()) {
                    const std::string actual = crypto::Sha1FileHex(item.dest);
                    if (actual != item.sha1) {
                        DeleteFileW(item.dest.c_str());
                        recordError(L"文件校验失败：" + fs::BaseName(item.dest));
                        return;
                    }
                }
                shared.done.fetch_add(1);
            }
        });
    }

    const DWORD started = GetTickCount();
    DWORD lastTick = started;
    uint64_t lastBytes = 0;
    for (;;) {
        const bool finished = shared.done.load() >= totalFiles;
        const bool failed = shared.failed.load();
        const DWORD now = GetTickCount();

        if (progress && now - lastTick >= 100) {
            const uint64_t bytes = shared.bytes.load();
            const uint64_t deltaBytes = bytes - lastBytes;
            const uint32_t deltaMs = now - lastTick;
            const double speed = deltaMs > 0 ? (deltaBytes * 1000.0 / deltaMs) : 0.0;
            const double percent =
                totalBytes > 0 ? std::min(99.0, bytes * 100.0 / static_cast<double>(totalBytes)) : 99.0;
            progress(nlohmann::json{
                {"stage", "download"},
                {"percent", percent},
                {"typeLabel", dominantType},
                {"downloadedBytes", bytes},
                {"totalBytes", totalBytes},
                {"overallDownloadedBytes", bytes},
                {"overallTotalBytes", totalBytes},
                {"overallDownloadedFiles", shared.done.load()},
                {"overallTotalFiles", totalFiles},
                {"speed", static_cast<uint64_t>(speed)},
                {"finished", false},
            });
            lastTick = now;
            lastBytes = bytes;
        }

        if (failed) break;
        if (finished) break;
        if (cancel && cancel->load()) {
            recordError(L"已取消");
            break;
        }
        Sleep(50);
    }

    for (auto& worker : workers) worker.join();

    if (shared.failed.load()) {
        if (error) *error = shared.error.empty() ? L"下载中断" : shared.error;
        return false;
    }

    if (progress) {
        const uint64_t bytes = shared.bytes.load();
        progress(nlohmann::json{
            {"stage", "download"},
            {"percent", 100},
            {"typeLabel", dominantType},
            {"downloadedBytes", bytes},
            {"totalBytes", totalBytes},
            {"overallDownloadedBytes", bytes},
            {"overallTotalBytes", totalBytes},
            {"overallDownloadedFiles", totalFiles},
            {"overallTotalFiles", totalFiles},
            {"speed", 0},
            {"finished", true},
        });
    }
    return true;
}

bool ExtractNatives(const VersionPlan& plan, const std::wstring& gameDir, const std::string& id,
                    std::vector<std::wstring>* notes, std::atomic<bool>* cancel) {
    if (plan.nativeJars.empty()) return true;

    const std::wstring nativesDir = NativesDir(gameDir, id);
    if (!fs::EnsureDir(nativesDir)) {
        if (notes) notes->push_back(L"无法创建 natives 目录：" + nativesDir);
        return false;
    }

    const std::wstring tar = TarExe();
    bool allOk = true;

    for (const std::wstring& jar : plan.nativeJars) {
        if (cancel && cancel->load()) return false;
        if (!fs::Exists(jar)) {
            if (notes) notes->push_back(L"native 包缺失：" + fs::BaseName(jar));
            allOk = false;
            continue;
        }

        // 标记文件记住 jar 的尺寸+mtime，未变化就不重复解压（大型整合包能省上千次小文件 IO）
        const std::wstring marker =
            fs::Join(nativesDir, L".natives-" + fs::BaseName(jar) + L".done");
        std::string fingerprint = std::to_string(fs::FileSize(jar));
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(jar.c_str(), GetFileExInfoStandard, &data)) {
                fingerprint += ":" + std::to_string(data.ftLastWriteTime.dwLowDateTime);
            }
        }
        if (fs::Exists(marker) && fs::ReadText(marker) == fingerprint) continue;

        const std::wstring temp = fs::UniqueTempPath(L"voxel-natives");
        if (!fs::EnsureDir(temp)) {
            if (notes) notes->push_back(L"无法创建临时目录，跳过 " + fs::BaseName(jar));
            allOk = false;
            continue;
        }

        int exitCode = -1;
        std::wstring tarError;
        const bool extracted =
            proc::RunCapture(tar, {L"-xf", jar, L"-C", temp}, temp, 120000, &exitCode, &tarError);

        bool copied = false;
        if (extracted) copied = CopyFlat(temp, nativesDir);
        fs::DeleteTree(temp);

        if (!extracted || !copied) {
            if (notes) notes->push_back(L"解压 native 失败：" + fs::BaseName(jar) +
                                        (tarError.empty() ? L"" : L"（" + tarError + L"）"));
            allOk = false;
            continue;
        }
        fs::WriteText(marker, fingerprint);
    }
    return allOk;
}

bool EnsureVersion(const ResolvedVersion& version, const nlohmann::json& settings,
                   const std::wstring& gameDir, std::atomic<bool>* cancel,
                   const ProgressFn& progress, std::wstring* error, VersionPlan* outPlan) {
    // 版本 JSON：父版本可能是联网临时取到的，必须落盘，否则下次启动又要联网
    for (const auto& level : version.levels) {
        const std::wstring dir = fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(level.id));
        const std::wstring jsonPath = fs::Join(dir, Utf8ToWide(level.id) + L".json");
        if (fs::Exists(jsonPath)) continue;
        if (!fs::EnsureDir(dir)) {
            if (error) *error = L"无法创建版本目录：" + dir;
            return false;
        }
        if (!fs::WriteTextAtomic(jsonPath, level.json.dump(2))) {
            if (error) *error = L"版本 JSON 写入失败：" + jsonPath;
            return false;
        }
    }

    // 核心文件：jar / 库 / 索引 / 日志配置
    VersionPlan core = BuildPlan(version, settings, gameDir, true, false);
    if (outPlan) *outPlan = core;
    if (!DownloadPlan(core, settings, cancel, progress, error)) return false;

    // 资源对象：索引刚刚可能才下载好，这一步重新读盘
    VersionPlan assets = BuildPlan(version, settings, gameDir, false, true);
    if (outPlan) *outPlan = assets;
    if (!DownloadPlan(assets, settings, cancel, progress, error)) return false;

    if (!ExtractNatives(core, gameDir, version.id, nullptr, cancel)) {
        // natives 解压失败不是致命错误：Java 会报 UnsatisfiedLinkError，
        // 用明确的错误信息比静默启动失败更好排查
        if (error) *error = L"natives 解压失败（Java 启动可能缺少本地库）";
        return false;
    }
    return true;
}

nlohmann::json VerifyVersion(const std::wstring& gameDir, const nlohmann::json& settings,
                             const std::string& id, std::atomic<bool>* cancel,
                             const ProgressFn& progress, std::wstring* error) {
    ResolvedVersion version = ResolveVersion(gameDir, settings, id, error);
    if (version.id.empty() || version.manifest.is_null()) {
        return nlohmann::json{{"ok", false}, {"error", error ? WideToUtf8(*error) : "版本解析失败"}};
    }

    // 版本 JSON 必须先落盘：BuildPlan 只负责算"缺哪些文件"，
    // 它不会写 versions/<id>/<id>.json。原来这里直接 DownloadPlan(before)，
    // 结果文件都下好了、版本目录却不存在，界面仍判定为"未安装"。
    for (const auto& level : version.levels) {
        const std::wstring dir = fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(level.id));
        const std::wstring jsonPath = fs::Join(dir, Utf8ToWide(level.id) + L".json");
        if (fs::Exists(jsonPath)) continue;
        if (!fs::EnsureDir(dir)) {
            if (error) *error = L"无法创建版本目录：" + dir;
            return nlohmann::json{{"ok", false},
                                  {"id", id},
                                  {"error", WideToUtf8(*error)}};
        }
        if (!fs::WriteTextAtomic(jsonPath, level.json.dump(2))) {
            if (error) *error = L"版本 JSON 写入失败：" + jsonPath;
            return nlohmann::json{{"ok", false},
                                  {"id", id},
                                  {"error", WideToUtf8(*error)}};
        }
    }

    VersionPlan before = BuildPlan(version, settings, gameDir, true, true);
    const int missingBefore = before.missing;
    const int corruptBefore = before.corrupt;

    int repaired = 0;
    if (!before.downloads.empty()) {
        const int count = static_cast<int>(before.downloads.size());
        if (!DownloadPlan(before, settings, cancel, progress, error)) {
            return nlohmann::json{
                {"ok", false},
                {"summary", {{"missing", missingBefore}, {"corrupt", corruptBefore},
                             {"present", before.present}}},
                {"repaired", 0},
                {"remaining", count},
                {"id", id},
                {"error", error ? WideToUtf8(*error) : "修补失败"},
            };
        }
        repaired = count;
        ExtractNatives(before, gameDir, version.id, nullptr, cancel);
    }

    // 资源对象：before 的 BuildPlan 是在索引还没落盘时算的，那一步 assets 是空的。
    // 核心文件下完后索引已就位，这里必须重新算一次才能真正补齐 assets，
    // 否则会出现"进度 100% 但 remaining 还有几千个资源"的假完成。
    {
        VersionPlan assets = BuildPlan(version, settings, gameDir, false, true);
        if (!assets.downloads.empty()) {
            if (!DownloadPlan(assets, settings, cancel, progress, error)) {
                return nlohmann::json{
                    {"ok", false},
                    {"summary", {{"missing", missingBefore}, {"corrupt", corruptBefore},
                                 {"present", before.present}}},
                    {"repaired", repaired},
                    {"remaining", static_cast<int>(assets.downloads.size())},
                    {"id", id},
                    {"error", error ? WideToUtf8(*error) : "资源文件下载失败"},
                };
            }
            repaired += static_cast<int>(assets.downloads.size());
        }
    }

    VersionPlan after = BuildPlan(version, settings, gameDir, true, true);
    const int remaining = static_cast<int>(after.downloads.size());

    return nlohmann::json{
        {"ok", remaining == 0},
        {"summary", {{"missing", missingBefore}, {"corrupt", corruptBefore},
                     {"present", before.present}, {"remainingMissing", after.missing}}},
        {"repaired", repaired},
        {"remaining", remaining},
        {"id", id},
        {"notes", before.notes},
    };
}

}  // namespace voxel
