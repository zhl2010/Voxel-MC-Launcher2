#include "webview.h"

#include "assets.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")

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

// https://voxel.local/a/b.css?x=1 → "a/b.css"；空路径视为 index.html
std::wstring AssetNameFromUri(const std::wstring& uri) {
    const size_t scheme = uri.find(L"://");
    const size_t cursor = scheme == std::wstring::npos ? 0 : scheme + 3;
    const size_t slash = uri.find(L'/', cursor);
    if (slash == std::wstring::npos) return L"index.html";

    std::wstring path = uri.substr(slash + 1);
    const size_t cut = path.find_first_of(L"?#");
    if (cut != std::wstring::npos) path.resize(cut);
    if (path.empty()) return L"index.html";
    return path;
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

    // ---- 3. 前端资源：优先用编进 exe 的内嵌副本，从内存应答 ----
    // 这样发布产物是单个 exe，不依赖旁边的 src/ 目录；只有内嵌缺失
    // （或运行时太旧、拿不到 WebResourceRequested）时才退回目录映射。
    std::wstring navigateTo = startUrl;
    bool servedFromMemory = false;
    if (HasEmbeddedAssets()) {
        const std::wstring pattern = L"https://" + virtualHost + L"/*";
        web_->AddWebResourceRequestedFilter(pattern.c_str(),
                                            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        auto resourceHandler =
            Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2* /*sender*/,
                       ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    return ServeEmbedded(args);
                });
        if (resourceHandler &&
            SUCCEEDED(web_->add_WebResourceRequested(resourceHandler.Get(), &resourceToken_))) {
            servedFromMemory = true;
        }
    }

    if (!servedFromMemory) {
        // 兜底路径：目录映射 → 再不行退回 file:///
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
    }

    // ---- 4. 打开非客户区支持，让 CSS 的 -webkit-app-region 生效 ----
    // 顺带关掉 devtools：前端没有调试需求，留着它等于给注入者一个免费调试器。
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(web_->get_Settings(&settings)) && settings) {
        settings->put_AreDevToolsEnabled(FALSE);
        Microsoft::WRL::ComPtr<ICoreWebView2Settings9> s9;
        if (SUCCEEDED(settings.As(&s9)) && s9) {
            s9->put_IsNonClientRegionSupportEnabled(TRUE);
        }
    }

    // ---- 4b. 导航与弹窗全部收紧：虚拟主机内的跳转放行，外部链接交给系统浏览器 ----
    // 没有这道闸，页面里一个 <a href> 或 window.open 就能在 WebView2 内加载任意网站，
    // 那个页面随后发不出 IPC（TrustedSource 拦住了），但渲染进程、Cookie、缓存都暴露了。
    const std::wstring trustedPrefix = L"https://" + virtualHost + L"/";
    navHandler_ = Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
        [trustedPrefix](ICoreWebView2* /*sender*/,
                        ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            std::wstring url;
            if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                url = uri;
                CoTaskMemFree(uri);
            }
            if (url.rfind(trustedPrefix, 0) == 0) return S_OK;
            if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0) {
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            args->put_Cancel(TRUE);
            return S_OK;
        });
    if (navHandler_) web_->add_NavigationStarting(navHandler_.Get(), &navToken_);

    newWindowHandler_ = Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [](ICoreWebView2* /*sender*/, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
                CoTaskMemFree(uri);
            }
            args->put_Handled(TRUE);
            return S_OK;
        });
    if (newWindowHandler_) web_->add_NewWindowRequested(newWindowHandler_.Get(), &newWindowToken_);

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
    if (web_ && resourceToken_.value != 0) {
        web_->remove_WebResourceRequested(resourceToken_);
        resourceToken_ = EventRegistrationToken{};
    }
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

HRESULT WebViewHost::ServeEmbedded(ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (!args || !env_) return E_FAIL;

    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || !request) return E_FAIL;

    LPWSTR uri = nullptr;
    if (FAILED(request->get_Uri(&uri)) || !uri) return E_FAIL;
    const std::wstring name = AssetNameFromUri(uri);
    CoTaskMemFree(uri);

    std::vector<unsigned char> data;
    std::wstring mime;
    const bool found = EmbeddedAsset(name, data, mime);

    IStream* stream = nullptr;
    int status = 200;
    std::wstring reason = L"OK";
    std::wstring headers;
    if (found) {
        if (!data.empty()) {
            stream = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
        }
        headers = L"Content-Type: " + mime + L"\r\nCache-Control: no-store\r\n";
    } else {
        static const char kNotFound[] = "404 not found";
        stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(kNotFound),
                                   static_cast<UINT>(sizeof(kNotFound) - 1));
        status = 404;
        reason = L"Not Found";
        headers = L"Content-Type: text/plain; charset=utf-8\r\n";
    }
    if (!stream) return E_FAIL;

    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
    const HRESULT hr = env_->CreateWebResourceResponse(stream, status, reason.c_str(),
                                                       headers.c_str(), &response);
    stream->Release();
    if (FAILED(hr) || !response) return hr;
    return args->put_Response(response.Get());
}
