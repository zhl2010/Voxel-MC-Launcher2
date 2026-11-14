// 文件系统小工具：路径拼接、读写、目录枚举。
//
// 统一用 std::filesystem 的 *ec 重载，任何目录不存在/权限不足都返回 false 而不是抛异常 ——
// 启动器的容错语义是"能继续就继续"，不让一个可选目录把整个流程炸掉。
#pragma once

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "util.h"

namespace voxel::fs {

namespace stdfs = std::filesystem;

inline bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return true;   // C:\...
    if (path.rfind(L"\\\\", 0) == 0) return true;            // UNC \\server\share
    return false;
}

inline std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    // b 是绝对路径时不能再拼，否则会得到 a\C:\... 这种无法打开的怪路径
    if (IsAbsolutePath(b)) return b;
    const wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

inline bool Exists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

inline bool IsDirectory(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    std::error_code ec;
    if (stdfs::is_directory(path, ec)) return true;
    stdfs::create_directories(path, ec);
    return !ec && stdfs::is_directory(path, ec);
}

inline std::string ReadText(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 旧版启动器写文件时可能带 UTF-8 BOM，nlohmann 遇到会直接抛 parse_error
    if (data.size() >= 3 && data[0] == '\xEF' && data[1] == '\xBB' && data[2] == '\xBF') {
        data.erase(0, 3);
    }
    return data;
}

inline bool WriteText(const std::wstring& path, const std::string& text) {
    EnsureDir(stdfs::path(path).parent_path().wstring());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

// 原子替换写：先写 .tmp 再 MoveFileEx，避免半个文件把配置/清单弄坏。
inline bool WriteTextAtomic(const std::wstring& path, const std::string& text) {
    // 临时名带 pid + 时钟：并发写同一目标时不会互相覆盖对方的 .tmp
    const std::wstring temp = path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount());
    if (!WriteText(temp, text)) return false;
    if (MoveFileExW(temp.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    DeleteFileW(temp.c_str());  // 替换失败别留下垃圾
    return false;
}

inline std::uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return static_cast<std::uint64_t>(data.nFileSizeLow) |
           (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32);
}

inline bool DeleteTree(const std::wstring& path) {
    std::error_code ec;
    stdfs::remove_all(path, ec);
    return !ec;
}

inline std::vector<std::wstring> ListDirs(const std::wstring& path) {
    std::vector<std::wstring> out;
    std::error_code ec;
    stdfs::directory_iterator it(path, ec), end;
    if (ec) return out;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code inner;
        if (it->is_directory(inner)) out.push_back(it->path().filename().wstring());
    }
    return out;
}

inline std::vector<std::wstring> ListFiles(const std::wstring& path, bool recursive = false) {
    std::vector<std::wstring> out;
    std::error_code ec;
    if (recursive) {
        stdfs::recursive_directory_iterator it(path, ec), end;
        if (ec) return out;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code inner;
            if (it->is_regular_file(inner)) out.push_back(it->path().wstring());
        }
        return out;
    }
    stdfs::directory_iterator it(path, ec), end;
    if (ec) return out;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code inner;
        if (it->is_regular_file(inner)) out.push_back(it->path().wstring());
    }
    return out;
}

inline std::wstring BaseName(const std::wstring& path) {
    return stdfs::path(path).filename().wstring();
}

inline std::wstring ParentDir(const std::wstring& path) {
    return stdfs::path(path).parent_path().wstring();
}

inline std::wstring TempDir() {
    wchar_t buf[MAX_PATH * 2]{};
    if (GetTempPathW(MAX_PATH * 2, buf) == 0) return L".";
    return std::wstring(buf);
}

inline std::wstring UniqueTempPath(const std::wstring& prefix) {
    static std::atomic<unsigned> counter{0};
    const unsigned tick = GetTickCount();
    const unsigned seq = counter.fetch_add(1);
    return Join(TempDir(), prefix + L"-" + std::to_wstring(tick) + L"-" + std::to_wstring(seq));
}

}  // namespace voxel::fs
