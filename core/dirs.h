// 目录选择、游戏目录切换、Java 探测。
//
// 对话框走 IFileOpenDialog（Vista+ 通用文件对话框），必须在 STA 主线程调用，
// 所以这几个命令在 IPC 桥里注册成"同步命令"，由 UI 线程直接执行。
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <windows.h>

namespace voxel {

struct JavaInfo {
    std::wstring home;
    std::wstring path;    // 优先 javaw.exe
    int major = 0;
    std::wstring version;  // release 文件里的原始版本号
};

// 扫描 JAVA_HOME / 注册表 / 常见安装目录 / PATH / 游戏目录自带 runtime
std::vector<JavaInfo> DetectJava(const std::wstring& gameDir);

// 按设置与版本要求挑一个 Java；失败时把原因写进 error
JavaInfo SelectJava(const nlohmann::json& settings, const std::wstring& gameDir, int requiredMajor,
                    std::wstring* error);

int SystemMemoryMb();

std::wstring PickFolder(HWND owner, const std::wstring& initial);
std::wstring PickJavaFile(HWND owner);
std::wstring PickPngFile(HWND owner);

}  // namespace voxel
