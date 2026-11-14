#include "proc.h"

#include <windows.h>
#include <algorithm>

namespace voxel::proc {

std::wstring Quote(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;

    std::wstring out = L"\"";
    int backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(static_cast<size_t>(backslashes * 2 + 1), L'\\');
            out.push_back(ch);
            backslashes = 0;
            continue;
        }
        out.append(static_cast<size_t>(backslashes), L'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    out.append(static_cast<size_t>(backslashes * 2), L'\\');
    out.push_back(L'"');
    return out;
}

bool RunCapture(const std::wstring& exe, const std::vector<std::wstring>& args,
                const std::wstring& cwd, DWORD timeoutMs, int* exitCode, std::wstring* error) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) {
        if (error) *error = L"创建管道失败";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine = Quote(exe);
    for (const auto& arg : args) commandLine += L" " + Quote(arg);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nullptr;

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                                        &startup, &info);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        if (error) *error = L"无法启动 " + exe + L"（错误码 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }

    // 输出只用于诊断，读干净避免子进程因管道写满而卡死
    std::string output;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        if (output.size() < 64 * 1024) output.append(buffer, read);
    }
    CloseHandle(readPipe);

    const DWORD wait = WaitForSingleObject(info.hProcess, timeoutMs);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        if (error) *error = L"命令超时：" + exe;
        return false;
    }

    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);

    if (exitCode) *exitCode = static_cast<int>(code);
    if (code != 0) {
        if (error) *error = exe + L" 返回错误码 " + std::to_wstring(code);
        return false;
    }
    return true;
}

}  // namespace voxel::proc
