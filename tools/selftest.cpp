// 无界面自检：把 core/ 与 launch/ 的整条链路跑一遍，不经过 WebView2。
// 用法：selftest.exe [版本 ID] [启动后观察秒数 0=不实际启动]
//
// 它的价值在于把"能编译"升级成"能跑"：清单联网、账户持久化、Java 选择、
// 文件校验修补、参数拼装与进程拉起，全部走真实代码路径与真实 %APPDATA% 数据。

#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/auth.h"
#include "core/config.h"
#include "core/dirs.h"
#include "core/fs.h"
#include "core/installer.h"
#include "core/util.h"
#include "core/versions.h"
#include "launch/launch.h"

namespace {

void Line(const std::string& text) { std::cout << text << std::endl; }

// nlohmann 的 value() 在字段存在但为 null 时会抛 type_error，自检里一律先判类型
std::string StrOf(const nlohmann::json& object, const std::string& key) {
    if (!object.is_object() || !object.contains(key)) return std::string();
    return object[key].is_string() ? object[key].get<std::string>() : std::string();
}

int IntOf(const nlohmann::json& object, const std::string& key) {
    if (!object.is_object() || !object.contains(key)) return 0;
    return object[key].is_number_integer() ? object[key].get<int>() : 0;
}

bool BoolOf(const nlohmann::json& object, const std::string& key) {
    if (!object.is_object() || !object.contains(key)) return false;
    return object[key].is_boolean() ? object[key].get<bool>() : false;
}

// 安全取子对象：result["summary"] 在 result 为 null 时会触发 nlohmann 的
// 断言（const operator[] 对 null 未定义），自检在"校验失败"路径上本来就会
// 拿到 null，必须走 contains 判断而不是直接下标。
const nlohmann::json& ChildOf(const nlohmann::json& object, const std::string& key) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    if (!object.is_object() || !object.contains(key)) return kEmpty;
    return object[key];
}

void TestManifest(const nlohmann::json& settings) {
    Line("[manifest] ListVersions(force=true) ...");
    const nlohmann::json result = voxel::ListVersions(settings, true);
    const std::string source = StrOf(result, "source");
    const size_t count = result.contains("versions") && result["versions"].is_array()
                             ? result["versions"].size()
                             : 0;
    Line("[manifest] source=" + source + " versions=" + std::to_string(count));
    if (!StrOf(result, "error").empty()) {
        Line("[manifest] error=" + result["error"].get<std::string>());
    }
    if (count > 0) {
        for (size_t i = 0; i < count && i < 5; ++i) {
            const nlohmann::json& item = result["versions"][i];
            Line("[manifest]   " + StrOf(item, "id") + " type=" +
                 StrOf(item, "type") + " installed=" +
                 (BoolOf(item, "installed") ? "yes" : "no"));
        }
    }
}

void TestAuth() {
    Line("[auth] offline account round-trip ...");
    // 自检会覆盖真实的账户文件，先备份、跑完再还原，避免把用户的登录态弄丢
    const std::wstring session = voxel::fs::Join(voxel::UserDataDir(), L"account-session.json");
    const std::string backup = voxel::fs::ReadText(session);
    const bool hadSession = !backup.empty();

    voxel::Account made = voxel::MakeOfflineAccount("Selftest");
    Line("[auth] name=" + made.name + " uuid=" + made.uuid + " type=" + made.type);
    bool ok = false;
    if (voxel::SaveAccount(made)) {
        voxel::Account loaded;
        if (voxel::LoadAccount(loaded)) {
            Line("[auth] reloaded name=" + loaded.name + " uuid=" + loaded.uuid +
                 " type=" + loaded.type);
            ok = loaded.uuid == made.uuid && loaded.name == made.name;
        }
    }
    Line(std::string("[auth] ") + (ok ? "OK" : "FAIL"));

    if (hadSession) {
        voxel::fs::WriteTextAtomic(session, backup);
        Line("[auth] 已还原原有账户文件");
    } else {
        voxel::ClearAccount();
        Line("[auth] 已清理自检账户");
    }
}

void TestJava(const nlohmann::json& settings, const std::wstring& gameDir) {
    const std::vector<voxel::JavaInfo> list = voxel::DetectJava(gameDir);
    Line("[java] detected " + std::to_string(list.size()));
    for (const auto& java : list) {
        Line("[java]   major=" + std::to_string(java.major) + " " + voxel::WideToUtf8(java.version) +
             " " + voxel::WideToUtf8(java.path));
    }
    std::wstring error;
    const voxel::JavaInfo selected = voxel::SelectJava(settings, gameDir, 21, &error);
    Line("[java] SelectJava(major>=21) => " + voxel::WideToUtf8(selected.path));
}

bool TestPlan(const nlohmann::json& settings, const std::wstring& gameDir,
              const std::string& versionId) {
    Line("[plan] " + versionId + " ...");
    std::wstring error;
    const voxel::ResolvedVersion version = voxel::ResolveVersion(gameDir, settings, versionId, &error);
    if (version.id.empty()) {
        Line("[plan] FAIL " + voxel::WideToUtf8(error));
        return false;
    }
    Line("[plan] id=" + version.id + " jarId=" + version.jarId +
         " javaMajor=" + std::to_string(version.javaMajor) +
         " levels=" + std::to_string(version.levels.size()));
    const voxel::VersionPlan plan = voxel::BuildPlan(version, settings, gameDir, true, true);
    Line("[plan] downloads=" + std::to_string(plan.downloads.size()) +
         " present=" + std::to_string(plan.present) + " missing=" + std::to_string(plan.missing) +
         " corrupt=" + std::to_string(plan.corrupt) +
         " natives=" + std::to_string(plan.nativeJars.size()));
    for (const auto& note : plan.notes) Line("[plan] note: " + voxel::WideToUtf8(note));
    size_t shown = 0;
    for (const auto& item : plan.downloads) {
        if (shown++ >= 10) break;
        Line("[plan]   " + item.type + " size=" + std::to_string(item.size) + " " +
             voxel::WideToUtf8(item.dest));
    }
    return true;
}

void TestVerify(const nlohmann::json& settings, const std::wstring& gameDir,
                const std::string& versionId) {
    Line("[verify] " + versionId + " ...");
    std::atomic<bool> cancel{false};
    std::wstring error;
    const auto started = std::chrono::steady_clock::now();
    const nlohmann::json result = voxel::VerifyVersion(
        gameDir, settings, versionId, &cancel,
        [](const nlohmann::json& progress) {
            if (progress.contains("percent")) {
                std::cout << "\r[verify] " << progress.value("percent", 0) << "% "
                          << progress.value("typeLabel", std::string()) << "    " << std::flush;
            }
        },
        &error);
    std::cout << std::endl;
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (result.is_null()) {
        Line("[verify] FAIL " + voxel::WideToUtf8(error));
        return;
    }
    Line("[verify] ok=" + std::string(BoolOf(result, "ok") ? "true" : "false") +
         " missing=" + std::to_string(IntOf(ChildOf(result, "summary"), "missing")) +
         " corrupt=" + std::to_string(IntOf(ChildOf(result, "summary"), "corrupt")) +
         " repaired=" + std::to_string(IntOf(result, "repaired")) +
         " remaining=" + std::to_string(IntOf(result, "remaining")) + " in " +
         std::to_string(static_cast<int>(seconds)) + "s");
    if (result.contains("notes") && result["notes"].is_array()) {
        for (const auto& note : result["notes"]) {
            if (note.is_string()) Line("[verify] note: " + note.get<std::string>());
        }
    }
}

void TestLaunch(const nlohmann::json& settings, const std::wstring& gameDir,
                const std::string& versionId, int seconds) {
    Line("[launch] " + versionId + " ...");
    voxel::Account account = voxel::MakeOfflineAccount("Selftest");
    nlohmann::json runSettings = settings;
    runSettings["version"] = versionId;
    runSettings["fullscreen"] = false;  // 自检不要抢全屏
    runSettings["versionIsolation"] = true;

    auto emit = [](const std::string& event, const nlohmann::json& payload) {
        if (event != "game:event") return;
        const std::string type = StrOf(payload, "type");
        const nlohmann::json& data = payload.contains("data") ? payload["data"] : nlohmann::json::object();
        if (type == "state") {
            Line("[event] state=" + StrOf(data, "status"));
        } else if (type == "phase") {
            Line("[event] phase step=" + std::to_string(data.value("step", 0)) + " " +
                 StrOf(data, "label") + " " + StrOf(data, "detail"));
        } else if (type == "progress") {
            std::cout << "\r[event] download " << data.value("percent", 0) << "% "
                      << data.value("typeLabel", std::string()) << "    " << std::flush;
        } else if (type == "log") {
            Line("[game] " + StrOf(data, "message"));
        } else if (type == "error" || type == "crash") {
            Line("[event] " + type + " " + StrOf(data, "message"));
        } else if (type == "closed" || type == "cancelled") {
            Line("[event] " + type + " code=" + std::to_string(data.value("code", 0)));
        }
    };

    try {
        const nlohmann::json state = voxel::Launcher::Instance().Start(runSettings, account, gameDir, emit);
        Line("[launch] start status=" + state.value("status", std::string()));
    } catch (const std::exception& error) {
        Line(std::string("[launch] FAIL ") + error.what());
        return;
    }

    if (seconds <= 0) {
        voxel::Launcher::Instance().Cancel();
        voxel::Launcher::Instance().Shutdown();
        Line("[launch] cancelled (dry run)");
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!voxel::Launcher::Instance().Busy()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (voxel::Launcher::Instance().Busy()) {
        Line("[launch] still busy after " + std::to_string(seconds) + "s, cancelling");
        voxel::Launcher::Instance().Cancel();
    }
    voxel::Launcher::Instance().Shutdown();
    Line("[launch] final state=" +
         voxel::Launcher::Instance().State().value("status", std::string("?")));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::string versionId = "1.21.1-fabric-0.20.1";
    int seconds = 25;
    if (argc > 1) versionId = voxel::WideToUtf8(argv[1]);
    if (argc > 2) seconds = _wtoi(argv[2]);

    const nlohmann::json settings = voxel::LoadSettings();
    const std::wstring gameDir = voxel::ResolveGameDir(settings);
    Line("[env] gameDir=" + voxel::WideToUtf8(gameDir) + " version=" + versionId);

    auto stage = [&](const char* name, auto&& fn) {
        try {
            fn();
        } catch (const std::exception& error) {
            Line(std::string("[") + name + "] EXCEPTION " + error.what());
        } catch (...) {
            Line(std::string("[") + name + "] EXCEPTION unknown");
        }
    };

    stage("manifest", [&]() { TestManifest(settings); });
    stage("auth", []() { TestAuth(); });
    stage("java", [&]() { TestJava(settings, gameDir); });
    // 版本解析失败时后面的校验/启动都没有意义，直接跳过并说明原因，
    // 否则只会在下游拿空结果继续跑，把失败原因冲淡。
    bool planOk = false;
    stage("plan", [&]() { planOk = TestPlan(settings, gameDir, versionId); });
    if (planOk) {
        stage("verify", [&]() { TestVerify(settings, gameDir, versionId); });
        stage("launch", [&]() { TestLaunch(settings, gameDir, versionId, seconds); });
    } else {
        Line("[verify] skipped：版本无法解析");
        Line("[launch] skipped：版本无法解析");
    }
    Line("[done]");
    return 0;
}
