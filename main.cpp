// Voxel MC Launcher 2 —— C++ 宿主入口
//
// 设计要点：
// 1. 进程级 DPI 感知用 PER_MONITOR_AWARE_V2，和旧 Electron 版一致，
//    避免 125%/150% 缩放下界面发虚。
// 2. WebView2 必须在 STA（单线程套间）线程创建，所以 CoInitializeEx 用
//    COINIT_APARTMENTTHREADED，且初始化期间的异步回调依赖主线程消息泵。
// 3. 单实例：用命名互斥体。多开会让两个进程同时写 %APPDATA% 配置而互相踩踏。
// 4. 后端分三层：app/（窗口、WebView2、IPC 桥）、core/（配置、清单、账户、安装器）、
//    launch/（进程管理），对应原 Rust 的 crate 划分。

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>

#include "app/bridge.h"
#include "app/webview.h"
#include "app/window.h"
#include "core/auth.h"
#include "core/config.h"
#include "core/crypto.h"
#include "core/dirs.h"
#include "core/fs.h"
#include "core/installer.h"
#include "core/loader.h"
#include "core/mods.h"
#include "core/oauth.h"
#include "core/skin.h"
#include "core/vmeta.h"
#include "core/util.h"
#include "core/versions.h"
#include "launch/launch.h"

#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kAppTitle[]    = L"Voxel Launcher";
constexpr wchar_t kMutexName[]   = L"VoxelMCLauncher2_SingleInstance";
constexpr wchar_t kVirtualHost[] = L"voxel.local";

// 前端目录：可执行文件同级的 src/；开发期退回到源码树里的 src/
std::wstring ResolveSrcDir() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring dir = exePath;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);

    const std::wstring candidates[] = {
        dir + L"\\src",                       // 打包后：exe 同级
        dir + L"\\..\\src",                   // 开发期：native/<配置>/ 的上一级
        dir + L"\\..\\..\\src",               // 开发期：native/build/ 往上两级
        dir + L"\\..\\..\\..\\src",           // 兜底
    };
    for (const auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return candidates[0];
}

// appInfo：字段名与旧 main.js 的 getInfo() 完全一致。
// electron / node 在 C++ 版不存在，填 "—" 让 UI 显示占位而不是 undefined。
// totalMemoryMB 是内存滑块的物理上限，modsDir 是模组页的保存目录 —— 旧实现漏了这两个，
// 前端会拿到 undefined。
nlohmann::json BuildAppInfo(const nlohmann::json& settings) {
    const std::wstring gameDir = voxel::ResolveGameDir(settings);
    const std::wstring dataDir = voxel::UserDataDir();

    std::wstring roaming;
    {
        wchar_t buf[MAX_PATH * 4]{};
        if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH * 4) > 0) roaming = buf;
    }
    const std::wstring defaultGameDir = roaming + L"\\.minecraft";

    int totalMemory = voxel::SystemMemoryMb();
    if (totalMemory <= 0) totalMemory = 8192;

    const voxel::JavaInfo java = voxel::SelectJava(settings, gameDir, 0, nullptr);

    return nlohmann::json{
        {"version", "1.2.0"},
        {"emlVersion", "native"},
        {"electron", "—"},
        {"chrome", "—"},
        {"node", "—"},
        {"platform", "win32"},
        {"arch", "x64"},
        {"gameDir", voxel::WideToUtf8(gameDir)},
        {"defaultGameDir", voxel::WideToUtf8(defaultGameDir)},
        {"gameDirIsCustom", !settings.value("gameDir", std::string()).empty()},
        {"versionsDir", voxel::WideToUtf8(gameDir + L"\\versions")},
        {"instanceDir", voxel::WideToUtf8(gameDir + L"\\versions")},
        {"dataDir", voxel::WideToUtf8(dataDir)},
        {"modsDir", voxel::WideToUtf8(gameDir + L"\\mods")},
        {"totalMemoryMB", totalMemory},
        {"javaPath", voxel::WideToUtf8(java.path)},
        {"javaVersion", voxel::WideToUtf8(java.version)},
        {"javaMajor", java.major},
    };
}

nlohmann::json EmitEnvelope(const std::string& type, const nlohmann::json& data) {
    return nlohmann::json{{"type", type}, {"data", data}};
}

// 把 WebView2 的失败码翻译成一条能照着做的建议。
// 之前无论什么错都提示"请安装运行时"，而实测最常见的 0x800700AA 恰恰不是没装。
const wchar_t* HrHint(HRESULT hr) {
    if (hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
        return L"WebView2 的浏览器进程被占用，或用户数据目录已损坏。\n"
               L"请先关闭其它启动器窗口；仍然失败就删除目录\n"
               L"%LOCALAPPDATA%\\Voxel-Launcher2\\WebView2 后重试。";
    }
    if (hr == E_UNEXPECTED) {
        return L"WebView2 运行时内部故障（能读到版本号，但浏览器进程起不来）。\n"
               L"请修复或重新安装 Microsoft Edge WebView2 Runtime。";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)) {
        return L"未找到 Microsoft Edge WebView2 Runtime，请先安装后重试\n"
               L"（Win10 1803+ 通常已自带，可从微软官网下载 Evergreen 安装包）。";
    }
    return L"启动器依赖 Microsoft Edge WebView2 Runtime。\n"
           L"请尝试重新安装该运行时后再启动。";
}

// 探测系统里有没有装 WebView2 运行时。
//
// GetAvailableCoreWebView2BrowserVersionString 会读 HKLM/HKCU 下的 EdgeUpdate 登记，
// 覆盖 system-level 与 per-user 两种安装；返回 S_OK 且版本串非空才算可用。
// 注意它只证明"登记存在"，不保证运行时没坏——真的起不来时 Create 阶段还会报错，
// 那条路径由下面的 HrHint 兜底。
bool ProbeWebView2Runtime(std::wstring* version) {
    LPWSTR text = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &text);
    bool ok = SUCCEEDED(hr) && text && text[0] != L'\0';
    if (ok && version) *version = text;
    if (text) CoTaskMemFree(text);
    return ok;
}

// 缺少运行时时给出的引导。
// 老版本只弹一句"请安装"，用户既不知道去哪下、也不知道该装哪种；
// 这里把官方页面直接给出来，并区分两种安装方式——
// 用 Evergreen Bootstrapper 会立刻返回"已安装"而不做修复，所以缺/坏时推荐独立离线包。
void OfferRuntimeInstall(HWND owner, const wchar_t* reason) {
    const wchar_t* kUrl = L"https://developer.microsoft.com/microsoft-edge/webview2/";
    wchar_t text[1200]{};
    swprintf_s(text,
               L"%s\n\n"
               L"这个启动器需要 Microsoft Edge WebView2 Runtime 才能显示界面。\n"
               L"Windows 10 1803 及以后通常已经自带；没装的机器请按下面操作：\n\n"
               L"1. 点【是】，在打开的页面里下载「Evergreen Standalone Installer」（x64，约 150 MB）\n"
               L"2. 运行下载好的安装包，装完重新打开启动器\n\n"
               L"提示：如果系统显示「已安装」但启动器仍打不开，请改用 Standalone Installer\n"
               L"修复安装（那个 1.9 MB 的 bootstrapper 只会检查版本号，不会修复）。\n\n"
               L"是否现在打开下载页面？",
               reason);

    const int choice = MessageBoxW(owner, text, kAppTitle, MB_YESNO | MB_ICONWARNING);
    if (choice == IDYES) {
        ShellExecuteW(nullptr, L"open", kUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // 单实例保护
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(nullptr, kAppTitle);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"COM 初始化失败，启动器无法运行。", kAppTitle, MB_ICONERROR);
        return 1;
    }

    MainWindow window;
    // 尺寸对齐旧版：1240x800，最小 1020x680；底色 #07110c
    if (!window.Create(hInstance, kAppTitle, 1240, 800, RGB(0x07, 0x11, 0x0c))) {
        MessageBoxW(nullptr, L"主窗口创建失败。", kAppTitle, MB_ICONERROR);
        CoUninitialize();
        return 2;
    }
    window.SetMinimumSize(1020, 680);

    // 先探测运行时再显示窗口：缺运行时是"没装组件"，不该让用户先看到一个空壳窗口。
    std::wstring runtimeVersion;
    if (!ProbeWebView2Runtime(&runtimeVersion)) {
        OfferRuntimeInstall(nullptr, L"未检测到 Microsoft Edge WebView2 Runtime。");
        DestroyWindow(window.Handle());
        CoUninitialize();
        if (mutex) CloseHandle(mutex);
        return 4;
    }

    window.Show(nCmdShow);

    WebViewHost webview;
    const std::wstring srcDir = ResolveSrcDir();
    const std::wstring startUrl = L"https://" + std::wstring(kVirtualHost) + L"/index.html";

    bool ok = webview.Create(window.Handle(), srcDir, kVirtualHost, startUrl);
    if (!ok) {
        // 带上真实错误码和出错阶段。只报"请安装运行时"会把用户和排查者
        // 一起带偏——实测最常见的 0x800700AA 是数据目录问题，不是缺运行时。
        const HRESULT hr = webview.LastError();

        // 登记存在但进程起不来（运行时装坏了）：这条与"没装"是两码事，
        // 单独引导用户去做修复安装。
        const bool runtimeBroken = hr == E_UNEXPECTED || hr == HRESULT_FROM_WIN32(ERROR_BUSY);
        if (runtimeBroken) {
            wchar_t reason[320]{};
            swprintf_s(reason,
                       L"WebView2 运行时异常（错误码 0x%08lX，阶段：%s）。\n"
                       L"检测到已安装版本 %s，但浏览器进程无法启动。",
                       static_cast<unsigned long>(hr),
                       webview.LastStage().empty() ? L"未知" : webview.LastStage().c_str(),
                       runtimeVersion.empty() ? L"未知" : runtimeVersion.c_str());
            OfferRuntimeInstall(window.Handle(), reason);
            DestroyWindow(window.Handle());
            CoUninitialize();
            if (mutex) CloseHandle(mutex);
            return 5;
        }

        wchar_t detail[640]{};
        swprintf_s(detail,
                   L"WebView2 初始化失败。\n\n"
                   L"阶段：%s\n"
                   L"错误码：0x%08lX\n\n"
                   L"%s",
                   webview.LastStage().empty() ? L"未知" : webview.LastStage().c_str(),
                   static_cast<unsigned long>(hr), HrHint(hr));
        MessageBoxW(window.Handle(), detail, kAppTitle, MB_ICONERROR);
        DestroyWindow(window.Handle());
        CoUninitialize();
        if (mutex) CloseHandle(mutex);
        return 3;
    }

    // 窗口尺寸变化时同步 WebView2 的边界，避免拖拽缩放时画面留白
    window.SetResizeHandler([&webview](int, int) { webview.Resize(); });

    // ---------------------- IPC 桥与命令表 ----------------------
    auto bridge = std::make_shared<Bridge>();

    // 设置读得频繁（几乎每个命令都要），写只在 settings_save；
    // 后台线程持有的是快照，真正改动时整份替换，避免数据竞争。
    std::mutex settingsMutex;
    nlohmann::json settings = voxel::LoadSettings();
    auto Snapshot = [&]() {
        std::lock_guard<std::mutex> lock(settingsMutex);
        return settings;
    };
    auto Replace = [&](const nlohmann::json& next) {
        std::lock_guard<std::mutex> lock(settingsMutex);
        settings = next;
    };

    auto emitGame = [bridge](const std::string& event, const nlohmann::json& payload) {
        bridge->Emit(event, payload);
    };

    // ---- 引导与设置 ----
    bridge->Register("app_bootstrap", [&](const nlohmann::json&) {
        const nlohmann::json current = Snapshot();
        voxel::Account account;
        nlohmann::json accountJson = nullptr;
        if (voxel::LoadAccount(account)) accountJson = account.ToJson();
        return nlohmann::json{
            {"settings", current},
            {"account", accountJson},
            {"gameState", voxel::Launcher::Instance().State()},
            {"appInfo", BuildAppInfo(current)},
        };
    });

    bridge->Register("app_info", [&](const nlohmann::json&) { return BuildAppInfo(Snapshot()); });

    bridge->Register("settings_save", [&](const nlohmann::json& args) {
        const nlohmann::json patch =
            args.contains("patch") ? args["patch"] : nlohmann::json::object();
        nlohmann::json next;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            next = voxel::MergeAllowed(patch, settings);
            if (!voxel::SaveSettings(next)) throw std::runtime_error("配置写入失败");
            settings = next;
        }
        return next;
    });

    // ---- 界面缩放 ----
    // WebView2 的缩放由宿主控制（ICoreWebView2Controller::put_ZoomFactor），
    // 页面里没有也做不到。原实现只把 uiScale 存进配置、从不真正应用，
    // 导致设置里的滑杆"看起来能拖，实际没反应"。
    auto ApplyUiScale = [&](int scale) {
        if (!webview.Controller()) return;
        int clamped = scale < 50 ? 50 : (scale > 200 ? 200 : scale);
        webview.Controller()->put_ZoomFactor(static_cast<double>(clamped) / 100.0);
    };

    bridge->Register("ui_set_scale", [&](const nlohmann::json& args) {
        const int value = args.value("value", 100);
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            settings["uiScale"] = value;
            voxel::SaveSettings(settings);
        }
        ApplyUiScale(value);
        return nlohmann::json(value);
    });

    // 启动时把已保存的缩放值应用上，否则重开启动器会回到 100%
    ApplyUiScale(settings.value("uiScale", 100));

    // ---- 模组加载器 ----
    bridge->RegisterAsync("loaders_list", [&](const nlohmann::json& args, Reply reply) {
        const std::string mcVersion = args.value("mcVersion", std::string());
        try {
            reply.Ok(voxel::loader::ListLoaders(Snapshot(), mcVersion));
        } catch (const std::exception& error) {
            reply.Fail(std::string("加载器列表获取失败：") + error.what());
        }
    });

    bridge->RegisterAsync("loader_install", [&](const nlohmann::json& args, Reply reply) {
        const nlohmann::json payload =
            args.contains("payload") ? args["payload"] : nlohmann::json::object();
        const std::string mcVersion = payload.value("gameVersion", std::string());
        const std::string loader = payload.value("loader", std::string());
        const std::string loaderVersion = payload.value("loaderVersion", std::string());
        const std::string customName = payload.value("customName", std::string());
        try {
            const nlohmann::json current = Snapshot();
            reply.Ok(voxel::loader::InstallLoader(current, voxel::ResolveGameDir(current),
                                                  mcVersion, loader, loaderVersion, customName));
        } catch (const std::exception& error) {
            reply.Fail(error.what());
        }
    });

    // ---- Modrinth 内容：模组 / 资源包 / 光影包 ----
    bridge->RegisterAsync("mods_search", [&](const nlohmann::json& args, Reply reply) {
        const nlohmann::json payload =
            args.contains("payload") ? args["payload"] : nlohmann::json::object();
        try {
            reply.Ok(voxel::mods::Search(Snapshot(), payload));
        } catch (const std::exception& error) {
            reply.Fail(error.what());
        }
    });

    bridge->RegisterAsync("mods_versions", [&](const nlohmann::json& args, Reply reply) {
        const nlohmann::json payload =
            args.contains("payload") ? args["payload"] : nlohmann::json::object();
        try {
            reply.Ok(voxel::mods::Versions(Snapshot(), payload));
        } catch (const std::exception& error) {
            reply.Fail(error.what());
        }
    });

    bridge->RegisterAsync("mods_install", [&](const nlohmann::json& args, Reply reply) {
        const nlohmann::json payload =
            args.contains("payload") ? args["payload"] : nlohmann::json::object();
        try {
            const nlohmann::json current = Snapshot();
            reply.Ok(voxel::mods::Install(current, voxel::ResolveGameDir(current), payload));
        } catch (const std::exception& error) {
            reply.Fail(error.what());
        }
    });

    bridge->RegisterAsync("mods_installed", [&](const nlohmann::json& args, Reply reply) {
        const std::string target = args.value("versionId", std::string());
        const bool isolated = !target.empty();
        try {
            const nlohmann::json current = Snapshot();
            reply.Ok(voxel::mods::Installed(voxel::ResolveGameDir(current), target, isolated));
        } catch (const std::exception& error) {
            reply.Fail(error.what());
        }
    });

    bridge->RegisterAsync("mods_icon", [&](const nlohmann::json& args, Reply reply) {
        const nlohmann::json payload =
            args.contains("payload") ? args["payload"] : nlohmann::json::object();
        try {
            reply.Ok(voxel::mods::Icon(Snapshot(), payload));
        } catch (const std::exception& error) {
            reply.Fail(error.what());
        }
    });

    // ---- 版本清单（要联网，走工作线程）----
    bridge->RegisterAsync("versions_list", [&](const nlohmann::json& args, Reply reply) {
        const bool force = args.value("force", false);
        try {
            nlohmann::json payload = voxel::ListVersions(Snapshot(), force);
            // 把自定义显示名 / 图标一并带上，前端不用再拉一次
            const nlohmann::json meta = voxel::vmeta::All();
            if (payload.contains("versions") && payload["versions"].is_array()) {
                for (auto& item : payload["versions"]) {
                    const std::string id = item.value("id", std::string());
                    if (!meta.contains(id)) continue;
                    const auto& m = meta[id];
                    const std::string name = m.value("displayName", std::string());
                    if (!name.empty()) {
                        item["displayName"] = name;
                    }
                    const std::string icon = m.value("icon", std::string());
                    if (!icon.empty()) item["icon"] = icon;
                }
            }
            reply.Ok(payload);
        } catch (const std::exception& error) {
            reply.Fail(std::string("版本清单获取失败：") + error.what());
        }
    });

    bridge->Register("versions_open_folder", [&](const nlohmann::json& args) {
        const std::string id = args.value("versionId", std::string());
        if (!voxel::SafeVersionId(id)) throw std::runtime_error("版本名非法");
        const std::wstring dir =
            voxel::fs::Join(voxel::fs::Join(voxel::ResolveGameDir(Snapshot()), L"versions"),
                        voxel::Utf8ToWide(id));
        if (!voxel::fs::Exists(dir)) throw std::runtime_error("该版本目录不存在");
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return nlohmann::json::object();
    });

    bridge->Register("versions_delete", [&](const nlohmann::json& args) {
        const std::string id = args.value("versionId", std::string());
        if (!voxel::SafeVersionId(id)) throw std::runtime_error("版本名非法");
        const std::wstring dir =
            voxel::fs::Join(voxel::fs::Join(voxel::ResolveGameDir(Snapshot()), L"versions"),
                        voxel::Utf8ToWide(id));
        if (!voxel::fs::Exists(dir)) throw std::runtime_error("该版本目录不存在");
        if (!voxel::fs::DeleteTree(dir)) throw std::runtime_error("删除失败，可能有文件被占用");
        return nlohmann::json{{"ok", true}};
    });

    // ---- 离线账户 ----
    bridge->Register("auth_offline", [&](const nlohmann::json& args) {
        const std::string name = args.value("username", std::string());
        if (!voxel::ValidOfflineName(name)) {
            throw std::runtime_error("用户名需为 3–16 位字母、数字或下划线");
        }
        const voxel::Account account = voxel::MakeOfflineAccount(name);
        if (!voxel::SaveAccount(account)) throw std::runtime_error("账户信息写入失败");
        return account.ToJson();
    });

    bridge->Register("auth_logout", [&](const nlohmann::json&) {
        voxel::ClearAccount();
        return nlohmann::json(nullptr);
    });

    // ---- 第三方登录 ----
    // Microsoft 设备码流程分两步：先取码给用户看，再在后台等授权。
    bridge->RegisterAsync("auth_microsoft", [&](const nlohmann::json&, Reply reply) {
        try {
            const nlohmann::json current = Snapshot();
            const voxel::auth::DeviceCode code =
                voxel::auth::StartMicrosoftDeviceCode(current);
            if (!code.ok) throw std::runtime_error(code.error);

            // 把验证码推到界面，用户需要照着在浏览器里输入
            reply.Event("game:event",
                        EmitEnvelope("log", {{"message", "请在浏览器打开 " + code.verifyUrl +
                                                              " 并输入代码 " + code.userCode},
                                             {"level", "INFO"}}));

            std::string error;
            const voxel::Account account = voxel::auth::CompleteMicrosoftLogin(
                current, code,
                [&](const std::string& status) {
                    reply.Event("game:event", EmitEnvelope("log", {{"message", status},
                                                                   {"level", "INFO"}}));
                },
                &error);
            if (account.IsEmpty()) throw std::runtime_error(error);
            reply.Ok(account.ToJson());
        } catch (const std::exception& e) {
            reply.Fail(e.what());
        }
    });

    bridge->RegisterAsync("auth_yggdrasil", [&](const nlohmann::json& args, Reply reply) {
        const nlohmann::json creds =
            args.contains("credentials") ? args["credentials"] : nlohmann::json::object();
        try {
            const nlohmann::json current = Snapshot();
            std::string error;
            const voxel::Account account = voxel::auth::YggdrasilLogin(
                current, creds.value("server", std::string()),
                creds.value("username", std::string()), creds.value("password", std::string()),
                &error);
            if (account.IsEmpty()) throw std::runtime_error(error);
            reply.Ok(account.ToJson());
        } catch (const std::exception& e) {
            reply.Fail(e.what());
        }
    });

    bridge->Register("skin_get", [&](const nlohmann::json&) {
        // 返回完整信息：自定义皮肤 + 当前模型（steve / alex）
        return voxel::SkinInfo();
    });

    bridge->Register("skin_set_model", [&](const nlohmann::json& args) {
        const std::string model = args.value("model", std::string());
        if (!voxel::SetSkinModel(model)) {
            throw std::runtime_error("皮肤模型只能是 steve 或 alex");
        }
        return voxel::SkinInfo();
    });

    bridge->RegisterAsync("skin_face", [&](const nlohmann::json&, Reply reply) {
        // 在线账户（Microsoft / Yggdrasil）的皮肤需要联网取，所以是异步命令。
        // 返回的是**整张皮肤**（data URL），由前端裁出 8×8 头部——
        // 后端没有图像解码能力，而前端 canvas 做这件事最直接。
        voxel::Account account;
        if (!voxel::LoadAccount(account) || account.IsEmpty() || account.type == "crack") {
            reply.Ok(nlohmann::json(nullptr));
            return;
        }
        try {
            const std::string data = voxel::skin::OnlineSkinDataUrl(Snapshot(), account);
            reply.Ok(data.empty() ? nlohmann::json(nullptr) : nlohmann::json(data));
        } catch (const std::exception&) {
            // 取皮肤失败不该打断界面，返回 null 让前端回退到默认皮肤
            reply.Ok(nlohmann::json(nullptr));
        }
    });

    bridge->Register("skin_upload", [&](const nlohmann::json&) {
        const std::wstring file = voxel::PickPngFile(window.Handle());
        if (file.empty()) return nlohmann::json(nullptr);
        std::string error;
        if (!voxel::ApplySkin(file, &error)) {
            throw std::runtime_error(error.empty() ? "皮肤文件无效" : error);
        }
        std::wstring name = file;
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        return nlohmann::json{{"name", voxel::WideToUtf8(name)},
                              {"dataUrl", voxel::SkinDataUrl()}};
    });

    bridge->Register("skin_clear", [&](const nlohmann::json&) {
        voxel::ClearSkin();
        return nlohmann::json{{"ok", true}};
    });

    // ---- 目录选择 ----
    bridge->Register("directories_choose_game", [&](const nlohmann::json&) {
        const std::wstring chosen =
            voxel::PickFolder(window.Handle(), voxel::ResolveGameDir(Snapshot()));
        return nlohmann::json(voxel::WideToUtf8(chosen));
    });

    bridge->Register("directories_set_game_dir", [&](const nlohmann::json& args) {
        const std::string raw = args.value("value", std::string());
        nlohmann::json next;
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            if (raw.empty()) {
                settings["gameDir"] = "";
            } else {
                const std::wstring dir = voxel::Utf8ToWide(raw);
                if (dir.empty()) throw std::runtime_error("路径无效");
                if (!voxel::fs::Exists(dir)) {
                    if (!voxel::fs::EnsureDir(dir)) throw std::runtime_error("无法创建该目录");
                } else if (!voxel::fs::IsDirectory(dir)) {
                    throw std::runtime_error("该路径不是文件夹");
                }
                settings["gameDir"] = raw;
            }
            if (!voxel::SaveSettings(settings)) throw std::runtime_error("配置写入失败");
            next = settings;
        }
        return nlohmann::json{{"settings", next}, {"appInfo", BuildAppInfo(next)}};
    });

    bridge->Register("dialog_choose_java", [&](const nlohmann::json&) {
        const std::wstring file = voxel::PickJavaFile(window.Handle());
        return nlohmann::json(voxel::WideToUtf8(file));
    });

    bridge->Register("mods_choose_dir", [&](const nlohmann::json& args) {
        const std::string current = args.value("currentDir", std::string());
        const std::wstring initial = current.empty()
                                         ? voxel::fs::Join(voxel::ResolveGameDir(Snapshot()), L"mods")
                                         : voxel::Utf8ToWide(current);
        const std::wstring chosen = voxel::PickFolder(window.Handle(), initial);
        return nlohmann::json(voxel::WideToUtf8(chosen));
    });

    bridge->Register("app_open_path", [&](const nlohmann::json& args) {
        std::string target = args.value("target", std::string());
        if (target.empty()) throw std::runtime_error("缺少目标路径");
        const std::wstring path = voxel::Utf8ToWide(target);
        if (!voxel::fs::Exists(path)) {
            if (!voxel::fs::EnsureDir(path)) throw std::runtime_error("目录不存在且无法创建");
        }
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return nlohmann::json::object();
    });

    // ---- 版本显示名与自定义图标 ----
    // 只覆盖"显示"，不改 versions/<id>/<id>.json 里的 id（那是继承链的权威标识）。
    bridge->Register("versions_meta", [&](const nlohmann::json&) {
        return voxel::vmeta::All();
    });

    bridge->Register("versions_rename", [&](const nlohmann::json& args) {
        const std::string id = args.value("versionId", std::string());
        if (!voxel::SafeVersionId(id)) throw std::runtime_error("版本名非法");
        voxel::vmeta::Meta meta = voxel::vmeta::Load(id);
        if (args.contains("displayName") && args["displayName"].is_string()) {
            std::string name = args["displayName"].get<std::string>();
            if (name.size() > 64) name.resize(64);
            meta.displayName = name;
        }
        if (args.contains("icon") && args["icon"].is_string()) {
            meta.icon = args["icon"].get<std::string>();
        }
        if (!voxel::vmeta::Save(id, meta)) throw std::runtime_error("保存失败");
        return nlohmann::json{{"versionId", id},
                              {"displayName", meta.displayName},
                              {"icon", meta.icon}};
    });

    bridge->Register("versions_set_icon", [&](const nlohmann::json& args) {
        const std::string id = args.value("versionId", std::string());
        if (!voxel::SafeVersionId(id)) throw std::runtime_error("版本名非法");
        const std::wstring file = voxel::PickIconFile(window.Handle());
        if (file.empty()) return nlohmann::json{{"cancelled", true}};
        const std::string bytes = voxel::fs::ReadText(file);
        std::string error;
        if (!voxel::vmeta::SaveIcon(id, bytes, &error)) {
            throw std::runtime_error(error.empty() ? "图标保存失败" : error);
        }
        return nlohmann::json{{"ok", true}, {"icon", "custom"}};
    });

    bridge->Register("versions_icon", [&](const nlohmann::json& args) {
        const std::string id = args.value("versionId", std::string());
        if (!voxel::SafeVersionId(id)) return nlohmann::json(nullptr);
        const std::string bytes = voxel::vmeta::LoadIcon(id);
        if (bytes.empty()) return nlohmann::json(nullptr);
        return nlohmann::json("data:image/png;base64," + voxel::crypto::Base64Encode(bytes));
    });

    // ---- 启动与校验 ----
    bridge->Register("game_state", [&](const nlohmann::json&) {
        return voxel::Launcher::Instance().State();
    });

    bridge->RegisterAsync("game_launch", [&](const nlohmann::json&, Reply reply) {
        voxel::Account account;
        if (!voxel::LoadAccount(account) || account.IsEmpty()) {
            reply.Fail("请先登录账户");
            return;
        }
        const nlohmann::json current = Snapshot();
        const std::wstring gameDir = voxel::ResolveGameDir(current);
        reply.Ok(voxel::Launcher::Instance().Start(current, account, gameDir, emitGame));
    });

    bridge->Register("game_cancel", [&](const nlohmann::json&) {
        voxel::Launcher::Instance().Cancel();
        return voxel::Launcher::Instance().State();
    });

    bridge->RegisterAsync("game_verify", [&](const nlohmann::json& args, Reply reply) {
        const std::string versionId = args.value("versionId", std::string());
        if (!voxel::SafeVersionId(versionId)) {
            reply.Fail("版本名非法");
            return;
        }
        const nlohmann::json current = Snapshot();
        const std::wstring gameDir = voxel::ResolveGameDir(current);
        std::atomic<bool> cancel{false};
        std::wstring error;
        auto progress = [&](const nlohmann::json& data) {
            bridge->Emit("game:event", EmitEnvelope("progress", data));
        };
        const nlohmann::json result =
            voxel::VerifyVersion(gameDir, current, versionId, &cancel, progress, &error);
        if (result.is_null()) {
            reply.Fail(error.empty() ? "校验失败" : voxel::WideToUtf8(error));
            return;
        }
        reply.Ok(result);
    });

    // ---- 窗口控制 ----
    bridge->Register("window_minimize", [&](const nlohmann::json&) {
        ShowWindow(window.Handle(), SW_MINIMIZE);
        return nlohmann::json::object();
    });

    bridge->Register("window_toggle_maximize", [&](const nlohmann::json&) {
        HWND h = window.Handle();
        ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
        const bool maximized = IsZoomed(h) != 0;
        bridge->Emit("window:state", nlohmann::json{{"maximized", maximized}});
        return nlohmann::json{{"maximized", maximized}};
    });

    bridge->Register("window_close", [&](const nlohmann::json&) {
        PostMessageW(window.Handle(), WM_CLOSE, 0, 0);
        return nlohmann::json::object();
    });

    bool fullscreen = false;
    RECT windowedRect{};
    bridge->Register("window_toggle_fullscreen", [&](const nlohmann::json&) {
        HWND h = window.Handle();
        if (!fullscreen) {
            GetWindowRect(h, &windowedRect);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER);
            fullscreen = true;
        } else {
            SetWindowPos(h, nullptr, windowedRect.left, windowedRect.top,
                         windowedRect.right - windowedRect.left,
                         windowedRect.bottom - windowedRect.top, SWP_NOZORDER);
            fullscreen = false;
        }
        bridge->Emit("window:state", nlohmann::json{{"fullscreen", fullscreen}});
        return nlohmann::json{{"fullscreen", fullscreen}};
    });

    if (!bridge->Attach(webview.Core())) {
        MessageBoxW(window.Handle(), L"IPC 桥挂载失败。", kAppTitle, MB_ICONERROR);
    }

    // 主消息循环
    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 先停游戏进程，再拆桥：否则工作线程还会往已销毁的 WebView2 上投消息
    voxel::Launcher::Instance().Shutdown();
    bridge->Detach();
    webview.Shutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
