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
#include "core/dirs.h"
#include "core/fs.h"
#include "core/installer.h"
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
    window.Show(nCmdShow);

    WebViewHost webview;
    const std::wstring srcDir = ResolveSrcDir();
    const std::wstring startUrl = L"https://" + std::wstring(kVirtualHost) + L"/index.html";

    bool ok = webview.Create(window.Handle(), srcDir, kVirtualHost, startUrl);
    if (!ok) {
        // 带上真实错误码和出错阶段。只报"请安装运行时"会把用户和排查者
        // 一起带偏——实测最常见的 0x800700AA 是数据目录问题，不是缺运行时。
        const HRESULT hr = webview.LastError();
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

    bridge->Register("ui_set_scale", [&](const nlohmann::json& args) {
        const int value = args.value("value", 100);
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            settings["uiScale"] = value;
            voxel::SaveSettings(settings);
        }
        return nlohmann::json(value);
    });

    // ---- 版本清单（要联网，走工作线程）----
    bridge->RegisterAsync("versions_list", [&](const nlohmann::json& args, Reply reply) {
        const bool force = args.value("force", false);
        try {
            reply.Ok(voxel::ListVersions(Snapshot(), force));
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

    bridge->Register("skin_get", [&](const nlohmann::json&) {
        const std::string data = voxel::SkinDataUrl();
        if (data.empty()) return nlohmann::json(nullptr);
        return nlohmann::json(data);
    });

    bridge->Register("skin_face", [&](const nlohmann::json&) {
        // 离线账户没有在线皮肤，交给前端画像素头像
        return nlohmann::json(nullptr);
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
