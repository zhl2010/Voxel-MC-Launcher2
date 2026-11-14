// WebView2 宿主
//
// 三个关键决定：
// 1. 用 SetVirtualHostNameToFolderMapping 把前端目录映射成 https://voxel.local/。
//    这和 Tauri 的 http://tauri.localhost 是同一思路：页面有正常的 https 源，
//    CSP、相对路径、localStorage 全部按原点处理，不会踩 file:// 的跨域坑。
// 2. WebView2 的异步初始化回调依赖宿主线程的消息泵，所以等待时不能用
//    WaitForSingleObject 干等，必须 MsgWaitForMultipleObjects + PeekMessage，
//    否则必然死锁。
// 3. 用户数据目录放在 %LOCALAPPDATA%\Voxel-Launcher2\WebView2，
//    和配置目录分开，卸载时能整体删。

#pragma once

#include <windows.h>
#include <WebView2.h>
#include <wrl.h>
#include <string>

class WebViewHost {
public:
    bool Create(HWND parent,
                const std::wstring& srcDir,
                const std::wstring& virtualHost,
                const std::wstring& startUrl);

    void Resize();
    void Navigate(const std::wstring& url);
    void Shutdown();

    ICoreWebView2* Core() const { return web_.Get(); }
    ICoreWebView2Controller* Controller() const { return controller_.Get(); }

    // 上一次 Create 失败的真实原因。原来只返回 bool，弹窗只能笼统地说
    // "请安装 WebView2 Runtime"；而实际最常见的 0x800700AA（ERROR_BUSY）
    // 恰恰不是没装运行时，是数据目录被占用/损坏。把 HRESULT 和出错阶段带出去，
    // 用户和日志才有可行动的线索。
    HRESULT LastError() const { return lastError_; }
    const std::wstring& LastStage() const { return stage_; }

private:
    // 实际初始化流程。Create 在外层负责"失败就换一个干净数据目录重试一次"。
    bool CreateInternal(HWND parent,
                        const std::wstring& srcDir,
                        const std::wstring& virtualHost,
                        const std::wstring& startUrl,
                        const std::wstring& userDataDir);

    static std::wstring UserDataDir();

    // 从内嵌资源应答一次 https://voxel.local/* 请求。
    // 有了它，发布产物才能是单 exe，不必在旁边放 src/ 目录。
    HRESULT ServeEmbedded(ICoreWebView2WebResourceRequestedEventArgs* args);

    // 把 <dir> 改名成 <dir>.corrupt-<tick>，成功返回 true。
    // 数据目录一旦被上一次异常退出写坏，WebView2 会持续返回 ERROR_BUSY，
    // 单纯重试没用；挪走让它重建是唯一不需要管理员权限的修法。
    static bool MoveAside(const std::wstring& dir);

    HRESULT lastError_ = S_OK;
    std::wstring stage_;

    HWND parent_ = nullptr;
    HWND host_ = nullptr;
    EventRegistrationToken resourceToken_{};
    EventRegistrationToken navToken_{};
    EventRegistrationToken newWindowToken_{};
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationStartingEventHandler> navHandler_;
    Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventHandler> newWindowHandler_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> web_;
};
