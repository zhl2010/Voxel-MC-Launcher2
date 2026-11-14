// 启动核心：解析版本 → 补齐文件 → 构建 JVM/游戏参数 → 拉起 java → 转发输出与状态。
//
// 参数拼装规则对齐旧 Electron 版 minecraft-launcher.js：
//  1. 版本 JSON 的 arguments.jvm / arguments.game 先按 rules 过滤再替换占位符，
//     含未解析 ${...} 的条目整条丢弃（否则会变成 --quickPlayPath 这种空值）。
//  2. 类路径只由启动器生成：清单里遗留的 -cp / -Djava.class.path 一律剥离。
//  3. 老版本没有 -Djava.library.path，缺了就注入，否则 LWJGL 报 UnsatisfiedLinkError。
#pragma once

#include <nlohmann/json.hpp>

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/auth.h"

namespace voxel {

class Launcher {
  public:
    using EmitFn = std::function<void(const std::string& event, const nlohmann::json& payload)>;

    static Launcher& Instance();

    // 立即返回 launching 状态，真正的工作在后台线程里跑
    nlohmann::json Start(const nlohmann::json& settings, const Account& account,
                         const std::wstring& gameDir, EmitFn emit);

    void Cancel();
    nlohmann::json State();
    void Shutdown();

    bool Busy();

  private:
    Launcher() = default;

    void Run(nlohmann::json settings, Account account, std::wstring gameDir, EmitFn emit);
    void RunImpl(nlohmann::json settings, Account account, std::wstring gameDir, EmitFn emit);
    void SetStatus(const std::string& status, const nlohmann::json& current);
    void EmitGame(const EmitFn& emit, const std::string& type, const nlohmann::json& data);
    void EmitState(const EmitFn& emit);

    std::mutex mutex_;
    // 串行化 Start：检查"是否已在跑"与 join 上一次 worker_ 必须原子地完成，
    // 否则两次并发 game_launch 可能都通过检查，起两个 JVM。
    std::mutex startMutex_;
    std::atomic<bool> cancel_{false};
    std::string status_ = "idle";
    nlohmann::json current_ = nullptr;
    nlohmann::json accountJson_ = nullptr;
    HANDLE process_ = nullptr;
    std::thread worker_;
};

}  // namespace voxel
