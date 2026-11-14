#include "bridge.h"

#include "../core/util.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr UINT kPumpMessage = WM_APP + 41;

// 只接受虚拟主机映射出来的页面（生产）或本地 file://（运行时过旧时的兜底）。
// 没有这道校验时，页面里任何一次跳转到外部网站，对方就能调用本地命令表
// （改游戏目录、打开任意路径、启动游戏）。
bool TrustedSource(const std::wstring& source) {
    if (source.empty()) return true;  // 个别运行时取不到来源，放行以免误伤
    if (source.rfind(L"https://voxel.local", 0) == 0) return true;
    if (source.rfind(L"file://", 0) == 0) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------- Reply

Reply::Reply(std::shared_ptr<Bridge> bridge, std::string id)
    : bridge_(std::move(bridge)), id_(std::move(id)) {}

void Reply::Ok(const nlohmann::json& data) {
    if (!bridge_) return;
    bridge_->Respond(id_, true, data, std::string());
}

void Reply::Fail(const std::string& error) {
    if (!bridge_) return;
    bridge_->Respond(id_, false, nlohmann::json(), error);
}

void Reply::Event(const std::string& event, const nlohmann::json& payload) {
    if (!bridge_) return;
    bridge_->Emit(event, payload);
}

// ---------------------------------------------------------------- Bridge

void Bridge::Register(const std::string& cmd, SyncHandler handler) {
    sync_[cmd] = std::move(handler);
}

void Bridge::RegisterAsync(const std::string& cmd, AsyncHandler handler) {
    async_[cmd] = std::move(handler);
    // 同名同步表里若还留着旧实现会优先命中同步表，这里显式清掉
    sync_.erase(cmd);
}

void Bridge::Post(std::function<void()> task) {
    if (!alive_.load()) return;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push_back(std::move(task));
    }
    if (pump_) PostMessageW(pump_, kPumpMessage, 0, 0);
}

void Bridge::Respond(const std::string& id, bool ok, const nlohmann::json& data,
                     const std::string& error) {
    nlohmann::json reply = {{"id", id}};
    if (ok) {
        reply["ok"] = true;
        reply["data"] = data;
    } else {
        reply["ok"] = false;
        reply["error"] = error;
    }
    const std::wstring text = voxel::Utf8ToWide(reply.dump());

    Post([this, text]() {
        if (web_) web_->PostWebMessageAsJson(text.c_str());
    });
}

void Bridge::Emit(const std::string& event, const nlohmann::json& payload) {
    const std::wstring text = voxel::Utf8ToWide(
        nlohmann::json{{"type", "event"}, {"event", event}, {"payload", payload}}.dump());

    Post([this, text]() {
        if (web_) web_->PostWebMessageAsJson(text.c_str());
    });
}

LRESULT CALLBACK Bridge::PumpProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    Bridge* self = reinterpret_cast<Bridge*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == kPumpMessage) {
        if (self) self->Drain();
        return 0;
    }
    if (self && self->originalProc_) {
        return CallWindowProcW(self->originalProc_, hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Bridge::Drain() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        tasks.swap(queue_);
    }
    for (auto& task : tasks) {
        try {
            task();
        } catch (...) {
            // 一个任务的异常不能拖垮整个消息循环
        }
    }
}

bool Bridge::Attach(ICoreWebView2* web) {
    if (!web) return false;
    web_ = web;

    // 隐藏的消息窗口：工作线程只能往这里 PostMessage，真正的 WebView2 调用都在 UI 线程
    pump_ = CreateWindowExW(0, L"STATIC", L"VoxelBridgePump", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, nullptr, nullptr);
    if (pump_) {
        SetWindowLongPtrW(pump_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        originalProc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(pump_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bridge::PumpProc)));
    }

    messageHandler_ = Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [this](ICoreWebView2* /*sender*/, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            // 先验来源：只信任本地映射页面，其它一律丢弃
            LPWSTR source = nullptr;
            const bool trusted =
                SUCCEEDED(args->get_Source(&source)) && TrustedSource(source ? source : L"");
            if (source) CoTaskMemFree(source);
            if (!trusted) return S_OK;

            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                const std::string text = voxel::WideToUtf8(raw);
                CoTaskMemFree(raw);
                nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
                if (!message.is_discarded() && message.is_object()) {
                    Dispatch(message);
                }
            }
            return S_OK;
        });

    if (!messageHandler_) return false;
    return SUCCEEDED(web_->add_WebMessageReceived(messageHandler_.Get(), &token_));
}

void Bridge::Dispatch(const nlohmann::json& message) {
    if (!web_) return;

    const std::string cmd = message.value("cmd", std::string());
    const std::string id = message.contains("id") && message["id"].is_number()
                               ? std::to_string(message["id"].get<long long>())
                               : message.value("id", std::string());
    const nlohmann::json args =
        message.contains("args") ? message["args"] : nlohmann::json::object();

    auto it = sync_.find(cmd);
    if (it != sync_.end()) {
        nlohmann::json reply = nlohmann::json::object();
        try {
            reply["ok"] = true;
            reply["data"] = it->second(args);
        } catch (const std::exception& error) {
            reply["ok"] = false;
            reply["error"] = std::string("命令执行失败：") + error.what();
        } catch (...) {
            reply["ok"] = false;
            reply["error"] = "命令执行失败（未知错误）";
        }
        reply["id"] = message.contains("id") ? message["id"] : nlohmann::json(id);
        const std::wstring text = voxel::Utf8ToWide(reply.dump());
        web_->PostWebMessageAsJson(text.c_str());
        return;
    }

    auto async = async_.find(cmd);
    if (async != async_.end()) {
        // 并发上限：超了就如实拒绝，不能让线程数随点击次数无限增长
        if (activeWorkers_.load() >= kMaxConcurrentWorkers) {
            nlohmann::json busy = {
                {"id", message.contains("id") ? message["id"] : nlohmann::json(id)},
                {"ok", false},
                {"error", "后台任务过多，请稍候再试"},
            };
            const std::wstring busyText = voxel::Utf8ToWide(busy.dump());
            web_->PostWebMessageAsJson(busyText.c_str());
            return;
        }

        auto self = shared_from_this();
        Reply reply(self, id);
        AsyncHandler handler = async->second;
        auto done = std::make_shared<std::atomic<bool>>(false);
        activeWorkers_.fetch_add(1);

        // 先把已经结束的线程移出容器，再追加新线程；join 必须在锁外做 ——
        // 工作线程回调 Reply::Ok 时会去抢 queueMutex_，持锁 join 会直接死锁。
        std::vector<std::thread> finished;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            for (auto it = workers_.begin(); it != workers_.end();) {
                if (it->done->load()) {
                    finished.push_back(std::move(it->thread));
                    it = workers_.erase(it);
                } else {
                    ++it;
                }
            }
            workers_.push_back(WorkerSlot{
                std::thread([self, handler, args, reply, done]() mutable {
                    try {
                        handler(args, reply);
                    } catch (const std::exception& error) {
                        reply.Fail(std::string("命令执行失败：") + error.what());
                    } catch (...) {
                        reply.Fail("命令执行失败（未知错误）");
                    }
                    done->store(true);
                    self->activeWorkers_.fetch_sub(1);
                }),
                done});
        }
        for (auto& thread : finished) {
            if (thread.joinable()) thread.join();
        }
        return;
    }

    nlohmann::json reply = {
        {"id", message.contains("id") ? message["id"] : nlohmann::json(id)},
        {"ok", false},
        {"error", "该功能尚未接入原生后端（" + cmd + "）"},
    };
    const std::wstring text = voxel::Utf8ToWide(reply.dump());
    web_->PostWebMessageAsJson(text.c_str());
}

void Bridge::Shutdown() {
    alive_ = false;
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        for (auto& slot : workers_) workers.push_back(std::move(slot.thread));
        workers_.clear();
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

void Bridge::Detach() {
    Shutdown();
    if (web_ && token_.value != 0) {
        web_->remove_WebMessageReceived(token_);
        token_ = EventRegistrationToken{};
    }
    messageHandler_.Reset();

    if (pump_) {
        SetWindowLongPtrW(pump_, GWLP_USERDATA, 0);
        DestroyWindow(pump_);
        pump_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.clear();
    web_.Reset();
}
