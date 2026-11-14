// IPC 桥：WebView2 postMessage ⇄ C++ 命令表
//
// 协议（对齐旧 Electron 版 preload + main.js 的 handle() 信封）：
//   页面 → 宿主  { id, cmd, args }
//   宿主 → 页面  { id, ok: true, data }  /  { id, ok: false, error }
//   宿主 → 页面  { type: "event", event, payload }
//
// 线程模型：
//  - 同步命令在 UI 线程执行，用于读配置、开文件对话框（IFileOpenDialog 需要 STA）。
//  - 异步命令丢进工作线程，结果/事件通过隐藏消息窗口回投 UI 线程再发给 WebView2 ——
//    ICoreWebView2 不是线程安全的，绝不能在工作线程里直接调用。
#pragma once

#include <windows.h>
#include <WebView2.h>
#include <wrl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Bridge;

class Reply {
  public:
    Reply(std::shared_ptr<Bridge> bridge, std::string id);
    void Ok(const nlohmann::json& data);
    void Fail(const std::string& error);
    void Event(const std::string& event, const nlohmann::json& payload);

  private:
    std::shared_ptr<Bridge> bridge_;
    std::string id_;
};

class Bridge : public std::enable_shared_from_this<Bridge> {
  public:
    using SyncHandler = std::function<nlohmann::json(const nlohmann::json& args)>;
    using AsyncHandler = std::function<void(const nlohmann::json& args, Reply reply)>;

    void Register(const std::string& cmd, SyncHandler handler);
    void RegisterAsync(const std::string& cmd, AsyncHandler handler);

    bool Attach(ICoreWebView2* web);
    void Detach();
    void Shutdown();

    void Emit(const std::string& event, const nlohmann::json& payload);
    void Post(std::function<void()> task);

  private:
    friend class Reply;

    static LRESULT CALLBACK PumpProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void Drain();
    void Dispatch(const nlohmann::json& message);
    void Respond(const std::string& id, bool ok, const nlohmann::json& data,
                 const std::string& error);

    std::map<std::string, SyncHandler> sync_;
    std::map<std::string, AsyncHandler> async_;

    Microsoft::WRL::ComPtr<ICoreWebView2> web_;
    Microsoft::WRL::ComPtr<ICoreWebView2WebMessageReceivedEventHandler> messageHandler_;
    EventRegistrationToken token_{};

    // 工作线程槽位：done 让 UI 线程能把已经跑完的线程 join 掉再回收。
    // 原来每个异步命令都 emplace_back 一个 std::thread 且永不清理，
    // 反复触发"刷新版本/校验/启动"会把线程数顶到耗尽。
    struct WorkerSlot {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    // 同时在跑的异步命令上限：超过就直接拒绝，而不是无限起线程。
    static constexpr int kMaxConcurrentWorkers = 8;

    HWND pump_ = nullptr;
    WNDPROC originalProc_ = nullptr;
    std::mutex queueMutex_;
    std::vector<std::function<void()>> queue_;
    std::vector<WorkerSlot> workers_;
    std::atomic<int> activeWorkers_{0};
    std::atomic<bool> alive_{true};
};
