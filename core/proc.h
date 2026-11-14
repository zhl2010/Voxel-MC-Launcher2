// 子进程小工具：命令行引号、同步执行并捕获输出（给 tar 解压 natives 用）。
#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace voxel::proc {

// 按 Windows CreateProcess 规则给单个参数加引号
std::wstring Quote(const std::wstring& arg);

// 同步运行并等待退出；exitCode 允许为空。失败返回 false 并写 error。
bool RunCapture(const std::wstring& exe, const std::vector<std::wstring>& args,
                const std::wstring& cwd, DWORD timeoutMs, int* exitCode, std::wstring* error);

}  // namespace voxel::proc
