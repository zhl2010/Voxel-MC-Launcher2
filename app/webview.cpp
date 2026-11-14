#include "webview.h"

#include <shlobj.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {

// WebView2 的异步回调投递到创建它的线程的消息队列。
// 这里既不能干等（死锁），也不能直接跑 GetMessage（会吞掉主循环的消息），
// 所以用 MsgWaitForMultipleObjects 等事件、顺手把消息泵干净。
bool PumpUntil(HANDLE ev, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        DWORD r = MsgWaitForMultipleObjects(1, &ev, FALSE, 50, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) return true;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return false;
}

}  // namespace

std::wstring WebViewHost::UserDataDir() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || !local) {
        if (local) CoTaskMemFree(local);
        return L"webview-data";
    }
    std::wstring dir = local;
    CoTaskMemFree(local);
    dir += L"\\Voxel-Launcher2\\WebView2";
    // 逐级创建，已存在时返回失败也无所谓
    std::wstring parent = dir.substr(0, dir.find_last_of(L'\\'));
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WebViewHost::MoveAside(const std::wstring& dir) {
    if (dir.empty()) return false;
    // 目录不存在就没什么可挪的：这种情况重试也不会变好。
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const std::wstring dest = dir + L".corrupt-" + std::to_wstring(GetTickCount64());
    return MoveFileW(dir.c_str(), dest.c_str()) != FALSE;
}

bool WebViewHost::Create(HWND parent,
                         const std::wstring& srcDir,
                         const std::wstring& virtualHost,
                         const std::wstring& startUrl) {
    lastError_ = S_OK;
    stage_.clear();

    const std::wstring userData = UserDataDir();
    if (CreateInternal(parent, srcDir, virtualHost, startUrl, userData)) return true;

    // 只对"目录相关"的两种失败做自愈重试：
    //   0x800700AA ERROR_BUSY      —— 上一次异常退出把数据目录写坏/占住
    //   0x8000FFFF E_UNEXPECTED    —— 数据目录半初始化，浏览器进程反复起不来
    // 其他错误（比如运行时真的没装）换目录也没用，直接如实上报。
    const HRESULT firstError = lastError_;
    const bool retryable = firstError == HRESULT_FROM_WIN32(ERROR_BUSY) ||
                           firstError == E_UNEXPECTED;
    if (!retryable) return false;
    if (!MoveAside(userData)) return false;

    stage_.assign(L"重建用户数据目录后重试");
    if (CreateInternal(parent, srcDir, virtualHost, startUrl, userData)) {
        lastError_ = S_OK;
        stage_.clear();
        return true;
    }
    // 重试仍失败：保留第一次的错误码——第二次多半是派生失败，参考价值更低。
    lastError_ = firstError;
    return false;
}

bool WebViewHost::CreateInternal(HWND parent,
                                 const std::wstring& srcDir,
                                 const std::wstring& virtualHost,
                                 const std::wstring& startUrl,
                                 const std::wstring& userData) {
    parent_ = parent;

    RECT rc{};
    GetClientRect(parent_, &rc);

    // 承载 WebView2 的子窗口：铺满客户区，无边框
    host_ = CreateWindowExW(0, L"STATIC", L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            0, 0, rc.right, rc.bottom,
                            parent_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host_) return false;

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // ---- 1. 创建环境 ----
    HRESULT envHr = E_PENDING;
    auto envHandler = Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [&](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            envHr = result;
            if (SUCCEEDED(result) && env) env_ = env;
            SetEvent(ev);
            return S_OK;
        });

    HRESULT start = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr, envHandler.Get());
    if (FAILED(start) || !PumpUntil(ev, 15000) || FAILED(envHr) || !env_) {
        stage_.assign(L"创建 WebView2 环境");
        lastError_ = FAILED(start) ? start
                                   : (envHr != E_PENDING ? envHr
                                                         : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        CloseHandle(ev);
        DestroyWindow(host_);
        host_ = nullptr;
        return false;
    }

    // ---- 2. 创建控制器 ----
    HRESULT ctrlHr = E_PENDING;
    auto ctrlHandler = Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [&](HRESULT result, ICoreWebView2Controller* ctrl) -> HRESULT {
            ctrlHr = result;
            if (SUCCEEDED(result) && ctrl) controller_ = ctrl;
            SetEvent(ev);
            return S_OK;
        });

    HRESULT started = env_->CreateCoreWebView2Controller(host_, ctrlHandler.Get());
    if (FAILED(started) || !PumpUntil(ev, 15000) || FAILED(ctrlHr) || !controller_) {
        stage_.assign(L"创建 WebView2 控制器");
        lastError_ = FAILED(started) ? started
                                     : (ctrlHr != E_PENDING ? ctrlHr
                                                            : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        CloseHandle(ev);
        DestroyWindow(host_);
        host_ = nullptr;
        return false;
    }

    controller_->put_IsVisible(TRUE);
    controller_->put_Bounds(rc);
    controller_->get_CoreWebView2(&web_);
    if (!web_) {
        CloseHandle(ev);
        controller_->Close();
        DestroyWindow(host_);
        host_ = nullptr;
        return false;
    }

    // ---- 3. 前端目录映射成 https 虚拟主机 ----
    // 这个 API 位于 ICoreWebView2_3（web 侧接口），只能在拿到 web 之后调用。
    std::wstring navigateTo = startUrl;
    Microsoft::WRL::ComPtr<ICoreWebView2_3> web3;
    if (SUCCEEDED(web_.As(&web3)) && web3) {
        web3->SetVirtualHostNameToFolderMapping(
            virtualHost.c_str(), srcDir.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    } else {
        // 运行时过旧：退回 file:///，至少界面能出来
        std::wstring fb = L"file:///" + srcDir + L"/index.html";
        for (auto& ch : fb) if (ch == L'\\') ch = L'/';
        navigateTo = fb;
    }

    // ---- 4. 打开非客户区支持，让 CSS 的 -webkit-app-region 生效 ----
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(web_->get_Settings(&settings)) && settings) {
        Microsoft::WRL::ComPtr<ICoreWebView2Settings9> s9;
        if (SUCCEEDED(settings.As(&s9)) && s9) {
            s9->put_IsNonClientRegionSupportEnabled(TRUE);
        }
    }

    // ---- 5. 加载页面 ----
    web_->Navigate(navigateTo.c_str());

    CloseHandle(ev);
    return true;
}

void WebViewHost::Resize() {
    if (!parent_) return;
    RECT rc{};
    GetClientRect(parent_, &rc);
    if (host_) MoveWindow(host_, 0, 0, rc.right, rc.bottom, TRUE);
    if (controller_) controller_->put_Bounds(rc);
}

void WebViewHost::Navigate(const std::wstring& url) {
    if (web_) web_->Navigate(url.c_str());
}

void WebViewHost::Shutdown() {
    if (controller_) {
        controller_->Close();
        controller_.Reset();
    }
    web_.Reset();
    env_.Reset();
    if (host_) {
        DestroyWindow(host_);
        host_ = nullptr;
    }
}
