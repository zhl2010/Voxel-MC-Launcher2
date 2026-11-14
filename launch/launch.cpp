#include "launch.h"

#include "core/config.h"
#include "core/dirs.h"
#include "core/fs.h"
#include "core/installer.h"
#include "core/proc.h"
#include "core/util.h"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace voxel {
namespace {

constexpr long long kWindowsCommandLimit = 30000;

std::string NowIso() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char buffer[64]{};
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.wYear, utc.wMonth,
             utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
    return buffer;
}

bool UsableArgument(const std::string& value) {
    if (value.find_first_not_of(" \t") == std::string::npos) return false;
    if (value.find("${") != std::string::npos) return false;
    return true;
}

std::vector<std::string> SplitArgs(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    char quoteChar = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quoted) {
            if (ch == quoteChar) {
                quoted = false;
                continue;
            }
            current.push_back(ch);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quoted = true;
            quoteChar = ch;
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string Substitute(const std::string& input, const std::map<std::string, std::string>& values) {
    std::string out;
    out.reserve(input.size());
    size_t index = 0;
    while (index < input.size()) {
        if (input.compare(index, 2, "${") == 0) {
            const size_t close = input.find('}', index + 2);
            if (close != std::string::npos) {
                const std::string key = input.substr(index + 2, close - index - 2);
                const auto it = values.find(key);
                if (it != values.end()) {
                    out += it->second;
                } else {
                    out.append(input, index, close - index + 1);  // 保留原样，交给 UsableArgument 丢弃
                }
                index = close + 1;
                continue;
            }
        }
        out.push_back(input[index]);
        ++index;
    }
    return out;
}

std::vector<std::string> CollectArguments(const nlohmann::json& entries,
                                          const std::map<std::string, std::string>& values) {
    std::vector<std::string> out;
    if (!entries.is_array()) return out;
    for (const auto& entry : entries) {
        if (entry.is_string()) {
            const std::string resolved = Substitute(entry.get<std::string>(), values);
            if (UsableArgument(resolved)) out.push_back(resolved);
            continue;
        }
        if (!entry.is_object()) continue;
        if (entry.contains("rules") && !RulesAllowJson(entry["rules"])) continue;

        std::vector<std::string> raw;
        if (entry.contains("value") && entry["value"].is_string()) {
            raw.push_back(entry["value"].get<std::string>());
        } else if (entry.contains("value") && entry["value"].is_array()) {
            for (const auto& item : entry["value"]) {
                if (item.is_string()) raw.push_back(item.get<std::string>());
            }
        }
        std::vector<std::string> resolved;
        bool usable = true;
        for (const auto& item : raw) {
            const std::string value = Substitute(item, values);
            if (!UsableArgument(value)) {
                usable = false;
                break;
            }
            resolved.push_back(value);
        }
        if (!usable) continue;  // 整条丢弃，避免留下一个悬空的 flag
        out.insert(out.end(), resolved.begin(), resolved.end());
    }
    return out;
}

// 清单里的 -cp / -Djava.class.path 必须清掉：类路径由启动器统一生成，
// 否则命令行里会出现两个 -cp，Java 直接用空的那一个。
std::vector<std::string> SanitizeJvmArguments(const std::vector<std::string>& input) {
    std::vector<std::string> out;
    for (size_t index = 0; index < input.size(); ++index) {
        const std::string value = input[index];
        if (value.empty()) continue;
        if (value == "-cp" || value == "-classpath" || value == "--class-path") {
            if (index + 1 < input.size()) ++index;
            continue;
        }
        if (value.rfind("-cp=", 0) == 0 || value.rfind("-classpath=", 0) == 0) continue;
        if (value.rfind("-Djava.class.path", 0) == 0) continue;
        if (value.find("${") != std::string::npos) continue;
        out.push_back(value);
    }
    return out;
}

std::vector<std::string> SanitizeGameArguments(const std::vector<std::string>& input,
                                               const std::string& mainClass) {
    std::vector<std::string> out;
    for (const auto& value : input) {
        if (value.empty()) continue;
        if (value == "-cp" || value == "-classpath") continue;
        if (value.rfind("-Djava.class.path", 0) == 0) continue;
        if (!mainClass.empty() && value == mainClass) continue;
        out.push_back(value);
    }
    return out;
}

std::string LevelOf(const std::string& line) {
    if (line.find("ERROR") != std::string::npos || line.find("Exception") != std::string::npos ||
        line.find("FATAL") != std::string::npos) {
        return "ERROR";
    }
    if (line.find("WARN") != std::string::npos) return "WARN";
    return "INFO";
}

std::wstring LogFile() {
    const std::wstring dir = fs::Join(UserDataDir(), L"logs");
    fs::EnsureDir(dir);
    return fs::Join(dir, L"latest.log");
}

struct BuiltArguments {
    std::vector<std::wstring> commandLine;
    std::vector<std::wstring> classpath;
    std::wstring mainClass;
    std::vector<std::wstring> notes;
    std::wstring error;
};

bool FileEx(const std::wstring& path) { return fs::Exists(path); }

BuiltArguments BuildArguments(const ResolvedVersion& version, const nlohmann::json& settings,
                              const Account& account, const std::wstring& gameDir,
                              const std::wstring& runtimeDir) {
    BuiltArguments built;
    const nlohmann::json& manifest = version.manifest;

    const std::wstring nativesDir = fs::Join(fs::Join(gameDir, L"bin"), Utf8ToWide(version.id));
    fs::EnsureDir(nativesDir);
    const std::wstring librariesDir = fs::Join(gameDir, L"libraries");
    const std::wstring assetsRoot = fs::Join(gameDir, L"assets");
    const std::wstring clientJar =
        fs::Join(fs::Join(gameDir, L"versions"), fs::Join(Utf8ToWide(version.jarId),
                                                          Utf8ToWide(version.jarId) + L".jar"));

    const std::string assetIndexId =
        manifest.contains("assetIndex") && manifest["assetIndex"].is_object()
            ? manifest["assetIndex"].value("id", std::string("legacy"))
            : manifest.value("assets", std::string("legacy"));
    const std::string assetsVersion = manifest.value("assets", assetIndexId);
    const bool legacyAssets = assetsVersion == "pre-1.6" || assetsVersion == "legacy";
    const std::wstring gameAssetsDir = legacyAssets ? fs::Join(runtimeDir, L"resources") : assetsRoot;

    // ---- 类路径：库 artifact + 客户端 jar 收尾 ----
    std::vector<std::wstring> classpath;
    {
        std::vector<std::wstring> seen;
        if (manifest.contains("libraries") && manifest["libraries"].is_array()) {
            for (const auto& library : manifest["libraries"]) {
                if (!library.is_object()) continue;
                if (library.contains("rules") && !RulesAllowJson(library["rules"])) continue;

                std::wstring path;
                if (library.contains("downloads") && library["downloads"].is_object() &&
                    library["downloads"].contains("artifact") &&
                    library["downloads"]["artifact"].is_object() &&
                    library["downloads"]["artifact"].contains("path")) {
                    const std::string relative =
                        library["downloads"]["artifact"]["path"].get<std::string>();
                    std::wstring current = librariesDir;
                    std::string part;
                    for (size_t i = 0; i <= relative.size(); ++i) {
                        if (i == relative.size() || relative[i] == '/') {
                            if (!part.empty()) {
                                current = fs::Join(current, Utf8ToWide(part));
                                part.clear();
                            }
                            continue;
                        }
                        part.push_back(relative[i]);
                    }
                    path = current;
                } else {
                    // 只有 name 的老式库：libraries/group/name/version/name-version.jar
                    const std::string name = library.value("name", std::string());
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
                    if (parts.size() >= 3) {
                        std::wstring relative;
                        for (size_t i = 0; i < parts[0].size(); ++i) {
                            relative.push_back(parts[0][i] == '.' ? L'/' : parts[0][i]);
                        }
                        path = fs::Join(librariesDir, relative);
                        path = fs::Join(path, Utf8ToWide(parts[1]));
                        path = fs::Join(path, Utf8ToWide(parts[2]));
                        path = fs::Join(path, Utf8ToWide(parts[1] + "-" + parts[2] +
                                                         (parts.size() > 3 ? "-" + parts[3] : "") +
                                                         ".jar"));
                    }
                }
                if (path.empty() || !FileEx(path)) continue;
                const std::wstring key = path;
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
                seen.push_back(key);
                classpath.push_back(path);
            }
        }
    }
    if (FileEx(clientJar)) classpath.push_back(clientJar);
    else built.notes.push_back(L"客户端 jar 缺失：" + clientJar);

    if (classpath.empty()) {
        built.error = L"没有任何可用的运行库（classpath 为空），请先校验并修补游戏文件";
        return built;
    }

    // ---- 占位符 ----
    const std::string uuid = account.uuid;
    const std::string token = account.accessToken.empty() ? "0" : account.accessToken;
    const std::string userType = account.type == "msa" ? "Xbox"
                                 : account.type == "yggdrasil" ? "Mojang" : "legacy";

    std::map<std::string, std::string> values;
    values["auth_player_name"] = account.name;
    values["version_name"] = version.id;
    values["game_directory"] = WideToUtf8(runtimeDir);
    values["assets_root"] = WideToUtf8(gameAssetsDir);
    values["game_assets"] = WideToUtf8(gameAssetsDir);
    values["assets_index_name"] = assetIndexId;
    values["auth_uuid"] = uuid;
    values["uuid"] = uuid;
    values["auth_access_token"] = token;
    values["access_token"] = token;
    values["auth_session"] = "token:" + token + ":" + uuid;
    values["auth_xuid"] = "0";
    values["clientid"] = "0";
    values["user_type"] = userType;
    values["user_properties"] = "{}";
    values["natives_directory"] = WideToUtf8(nativesDir);
    values["launcher_name"] = "VoxelLauncher";
    values["launcher_version"] = "1.2";
    values["classpath_separator"] = ";";
    values["resolution_width"] = std::to_string(settings.value("windowWidth", 1280));
    values["resolution_height"] = std::to_string(settings.value("windowHeight", 720));
    values["library_directory"] = WideToUtf8(librariesDir);
    values["primary_jar"] = WideToUtf8(clientJar);

    // ---- 参数 ----
    std::vector<std::string> jvmRaw;
    if (manifest.contains("arguments") && manifest["arguments"].is_object() &&
        manifest["arguments"].contains("jvm")) {
        jvmRaw = CollectArguments(manifest["arguments"]["jvm"], values);
    }
    std::vector<std::string> gameRaw;
    if (manifest.contains("arguments") && manifest["arguments"].is_object() &&
        manifest["arguments"].contains("game")) {
        gameRaw = CollectArguments(manifest["arguments"]["game"], values);
    }
    if (gameRaw.empty() && manifest.contains("minecraftArguments") &&
        manifest["minecraftArguments"].is_string()) {
        for (const auto& arg : SplitArgs(manifest["minecraftArguments"].get<std::string>())) {
            gameRaw.push_back(Substitute(arg, values));
        }
    }
    if (gameRaw.empty()) built.notes.push_back(L"版本 JSON 没有游戏参数");

    const std::vector<std::string> cleanJvm = SanitizeJvmArguments(jvmRaw);
    const std::string mainClass = manifest.value("mainClass", std::string("net.minecraft.client.main.Main"));
    const std::vector<std::string> cleanGame = SanitizeGameArguments(gameRaw, mainClass);

    std::vector<std::string> options = cleanJvm;
    options.push_back("-Xmx" + std::to_string(settings.value("maxMemory", 4096)) + "M");
    options.push_back("-Xms" + std::to_string(settings.value("minMemory", 1024)) + "M");

    const std::string userJvm = settings.value("jvmArgs", std::string());
    if (!userJvm.empty()) {
        const std::vector<std::string> parsed = SanitizeJvmArguments(SplitArgs(userJvm));
        options.insert(options.end(), parsed.begin(), parsed.end());
    }

    bool hasLibraryPath = false;
    for (const auto& option : options) {
        if (option.rfind("-Djava.library.path=", 0) == 0) hasLibraryPath = true;
    }
    if (!hasLibraryPath) {
        options.insert(options.begin(), "-Djava.library.path=" + WideToUtf8(nativesDir));
        built.notes.push_back(L"已注入 -Djava.library.path=" + nativesDir);
    }

    // log4j 配置：没有它，现代版本会在控制台刷一堆 "No appenders" 警告
    if (manifest.contains("logging") && manifest["logging"].is_object() &&
        manifest["logging"].contains("client") && manifest["logging"]["client"].is_object() &&
        manifest["logging"]["client"].contains("file") &&
        manifest["logging"]["client"]["file"].is_object()) {
        const std::string id =
            manifest["logging"]["client"]["file"].value("id", std::string());
        if (!id.empty()) {
            const std::wstring config = fs::Join(fs::Join(gameDir, L"assets"),
                                                 fs::Join(L"log_configs", Utf8ToWide(id)));
            if (FileEx(config)) {
                options.push_back("-Dlog4j.configurationFile=" + WideToUtf8(config));
            }
        }
    }

    std::wstring joinedClasspath;
    for (size_t i = 0; i < classpath.size(); ++i) {
        if (i) joinedClasspath += L";";
        joinedClasspath += classpath[i];
    }

    built.classpath = classpath;
    built.mainClass = Utf8ToWide(mainClass);

    std::vector<std::wstring> args;
    for (const auto& option : options) args.push_back(Utf8ToWide(option));
    args.push_back(L"-cp");
    args.push_back(joinedClasspath);
    args.push_back(built.mainClass);
    for (const auto& arg : cleanGame) args.push_back(Utf8ToWide(arg));

    if (settings.value("fullscreen", false)) {
        bool already = false;
        for (const auto& arg : args) {
            if (arg == L"--fullscreen") already = true;
        }
        if (!already) args.push_back(L"--fullscreen");
    }

    built.commandLine = args;
    return built;
}

std::wstring MaskedCommand(const std::wstring& javaPath, const std::vector<std::wstring>& args) {
    std::wstring text = proc::Quote(javaPath);
    bool maskNext = false;
    for (const auto& arg : args) {
        if (maskNext) {
            text += L" **********";
            maskNext = false;
            continue;
        }
        text += L" " + proc::Quote(arg);
        if (arg == L"--accessToken") maskNext = true;
    }
    return text;
}

}  // namespace

Launcher& Launcher::Instance() {
    static Launcher launcher;
    return launcher;
}

void Launcher::SetStatus(const std::string& status, const nlohmann::json& current) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
    current_ = current;
}

void Launcher::EmitGame(const EmitFn& emit, const std::string& type, const nlohmann::json& data) {
    if (!emit) return;
    emit("game:event", nlohmann::json{{"type", type}, {"data", data}});
}

void Launcher::EmitState(const EmitFn& emit) {
    if (!emit) return;
    nlohmann::json state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = nlohmann::json{{"status", status_}, {"current", current_}, {"account", accountJson_}};
    }
    EmitGame(emit, "state", state);
}

bool Launcher::Busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_ != "idle";
}

nlohmann::json Launcher::State() {
    std::lock_guard<std::mutex> lock(mutex_);
    return nlohmann::json{{"status", status_}, {"current", current_}, {"account", accountJson_}};
}

nlohmann::json Launcher::Start(const nlohmann::json& settings, const Account& account,
                               const std::wstring& gameDir, EmitFn emit) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ != "idle") throw std::runtime_error("游戏已在启动或运行中");
    }
    if (account.name.empty()) throw std::runtime_error("请先登录账户");
    const std::string versionId = settings.value("version", std::string());
    if (versionId.empty()) throw std::runtime_error("请先选择一个 Minecraft 版本");

    if (worker_.joinable()) worker_.join();  // 上一次已经结束，回收线程

    cancel_ = false;
    SetStatus("launching", nlohmann::json{{"id", versionId},
                                          {"loader", settings.value("loader", std::string("vanilla"))}});
    accountJson_ = account.ToJson();
    worker_ = std::thread(&Launcher::Run, this, settings, account, gameDir, emit);
    return State();
}

void Launcher::Cancel() {
    cancel_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_ == "launching" || status_ == "running") status_ = "cancelling";
    }
    // 已经拉起 java 时直接结束进程；还没拉起就靠 Run 里的 cancel_ 检查退出
    std::lock_guard<std::mutex> lock(mutex_);
    if (process_) {
        TerminateProcess(process_, 1);
    }
}

void Launcher::Shutdown() {
    Cancel();
    if (worker_.joinable()) worker_.join();
}

void Launcher::Run(nlohmann::json settings, Account account, std::wstring gameDir, EmitFn emit) {
    auto finishIdle = [&]() {
        SetStatus("idle", nullptr);
        EmitState(emit);
    };
    auto fail = [&](const std::wstring& message) {
        EmitGame(emit, "error", nlohmann::json{{"message", WideToUtf8(message)}});
        finishIdle();
    };

    EmitState(emit);
    EmitGame(emit, "phase", nlohmann::json{{"key", "prepare"},
                                           {"step", 0},
                                           {"label", "正在准备启动"},
                                           {"detail", settings.value("version", std::string())},
                                           {"done", false}});

    const std::string versionId = settings.value("version", std::string());
    std::wstring error;
    ResolvedVersion version = ResolveVersion(gameDir, settings, versionId, &error);
    if (version.manifest.is_null() || version.id.empty()) {
        fail(error.empty() ? L"版本解析失败" : error);
        return;
    }
    if (cancel_.load()) {
        EmitGame(emit, "cancelled", nlohmann::json::object());
        finishIdle();
        return;
    }

    // 版本隔离时游戏目录落在 instances/<id>，库与资源仍在公共根目录
    const bool isolation = settings.value("versionIsolation", false);
    const std::wstring runtimeDir =
        isolation ? fs::Join(fs::Join(gameDir, L"versions"), Utf8ToWide(version.id)) : gameDir;
    fs::EnsureDir(runtimeDir);

    EmitGame(emit, "phase", nlohmann::json{{"key", "download"},
                                           {"step", 1},
                                           {"label", "正在检查游戏文件"},
                                           {"detail", version.id},
                                           {"done", false}});

    VersionPlan plan;
    const bool installed =
        EnsureVersion(version, settings, gameDir, &cancel_,
                      [&](const nlohmann::json& data) { EmitGame(emit, "progress", data); }, &error,
                      &plan);
    for (const auto& note : plan.notes) {
        EmitGame(emit, "log", nlohmann::json{{"level", "WARN"},
                                             {"message", WideToUtf8(note)},
                                             {"time", NowIso()}});
    }
    if (!installed) {
        if (cancel_.load()) {
            EmitGame(emit, "cancelled", nlohmann::json::object());
            finishIdle();
        } else {
            fail(error.empty() ? L"游戏文件不完整" : error);
        }
        return;
    }
    if (cancel_.load()) {
        EmitGame(emit, "cancelled", nlohmann::json::object());
        finishIdle();
        return;
    }

    EmitGame(emit, "phase", nlohmann::json{{"key", "install"},
                                           {"step", 2},
                                           {"label", "正在准备运行环境"},
                                           {"detail", "匹配 Java 运行时"},
                                           {"done", false}});

    std::wstring javaError;
    JavaInfo java = SelectJava(settings, gameDir, version.javaMajor, &javaError);
    if (java.path.empty()) {
        fail(javaError.empty() ? L"未找到可用的 Java" : javaError);
        return;
    }
    EmitGame(emit, "log", nlohmann::json{
                              {"level", "INFO"},
                              {"message", "使用 Java：" + WideToUtf8(java.path) +
                                              (java.version.empty() ? "" : "（" + WideToUtf8(java.version) + "）")},
                              {"time", NowIso()}});

    BuiltArguments built = BuildArguments(version, settings, account, gameDir, runtimeDir);
    for (const auto& note : built.notes) {
        EmitGame(emit, "log", nlohmann::json{{"level", "WARN"},
                                             {"message", WideToUtf8(note)},
                                             {"time", NowIso()}});
    }
    if (!built.error.empty()) {
        fail(built.error);
        return;
    }

    std::wstring commandLine = proc::Quote(java.path);
    for (const auto& arg : built.commandLine) commandLine += L" " + proc::Quote(arg);

    // Windows 命令行上限 32767；超了就把参数写进 @argfile（Java 9+ 支持）
    std::wstring argFile;
    if (static_cast<long long>(commandLine.size()) > kWindowsCommandLimit) {
        if (java.major > 0 && java.major < 9) {
            fail(L"启动命令超过 Windows 命令行长度上限，请减少模组数量或改用 Java 9 以上版本");
            return;
        }
        argFile = fs::Join(fs::TempDir(), L"voxel-args-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
        std::ostringstream content;
        for (const auto& arg : built.commandLine) {
            std::wstring safe = arg;
            // argfile 里反斜杠是转义符，统一换成正斜杠（Java 在 Windows 上同样可用）
            std::replace(safe.begin(), safe.end(), L'\\', L'/');
            content << WideToUtf8(proc::Quote(safe)) << "\n";
        }
        if (!fs::WriteText(argFile, content.str())) {
            fail(L"无法写入启动参数文件");
            return;
        }
        commandLine = proc::Quote(java.path) + L" @" + proc::Quote(argFile);
    }

    EmitGame(emit, "log", nlohmann::json{{"level", "INFO"},
                                         {"message", WideToUtf8(MaskedCommand(java.path, built.commandLine))},
                                         {"time", NowIso()}});

    EmitGame(emit, "phase", nlohmann::json{{"key", "launch"},
                                           {"step", 3},
                                           {"label", "正在启动游戏"},
                                           {"detail", version.id},
                                           {"done", false}});

    if (cancel_.load()) {
        EmitGame(emit, "cancelled", nlohmann::json::object());
        finishIdle();
        return;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) {
        fail(L"无法创建输出管道");
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        java.path.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, runtimeDir.c_str(), &startup, &process);
    CloseHandle(writePipe);

    if (!created) {
        CloseHandle(readPipe);
        fail(L"无法启动 Java（错误码 " + std::to_wstring(GetLastError()) + L"）");
        return;
    }
    CloseHandle(process.hThread);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = process.hProcess;
        status_ = "running";
        current_ = nlohmann::json{{"id", version.id},
                                  {"loader", settings.value("loader", std::string("vanilla"))},
                                  {"pid", static_cast<int>(process.dwProcessId)}};
    }
    EmitState(emit);
    EmitGame(emit, "phase", nlohmann::json{{"key", "launch"},
                                           {"step", 3},
                                           {"label", "游戏已启动"},
                                           {"detail", version.id},
                                           {"done", true}});

    std::ofstream logStream(LogFile(), std::ios::binary | std::ios::trunc);
    auto writeLog = [&](const std::string& line, const std::string& level) {
        const std::string iso = NowIso();
        EmitGame(emit, "log", nlohmann::json{{"level", level}, {"message", line}, {"time", iso}});
        if (logStream) logStream << "[" << iso << "] [" << level << "] " << line << "\r\n";
    };

    // 逐行转发游戏控制台输出
    std::string buffer;
    char chunk[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
        buffer.append(chunk, read);
        for (;;) {
            const size_t newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            writeLog(line, LevelOf(line));
        }
        if (buffer.size() > 65536) {  // 极端情况下不让缓冲无限增长
            writeLog(buffer, "INFO");
            buffer.clear();
        }
    }
    if (!buffer.empty()) {
        if (buffer.back() == '\r') buffer.pop_back();
        if (!buffer.empty()) writeLog(buffer, LevelOf(buffer));
    }
    CloseHandle(readPipe);

    WaitForSingleObject(process.hProcess, 5000);
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = nullptr;
    }
    CloseHandle(process.hProcess);
    if (!argFile.empty()) DeleteFileW(argFile.c_str());

    if (cancel_.load()) {
        EmitGame(emit, "cancelled", nlohmann::json::object());
        finishIdle();
        return;
    }

    const int code = static_cast<int>(exitCode);
    if (code != 0) {
        EmitGame(emit, "crash", nlohmann::json{{"code", code},
                                               {"message", "游戏进程异常退出（代码 " +
                                                               std::to_string(code) + "）"}});
    }
    EmitGame(emit, "closed", nlohmann::json{{"code", code}});
    finishIdle();
}

}  // namespace voxel
