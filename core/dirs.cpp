#include "dirs.h"

#include "fs.h"
#include "util.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>
#include <algorithm>
#include <cstdlib>
#include <set>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

namespace voxel {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring Lower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& ch : out) ch = static_cast<wchar_t>(::towlower(ch));
    return out;
}

int ParseMajorVersion(const std::wstring& raw) {
    if (raw.empty()) return 0;
    // "1.8.0_511" → 8；"17.0.12" → 17；"26.0.2.1" → 26
    std::wstring text = raw;
    int first = -1;
    int second = -1;
    size_t pos = 0;
    auto nextNumber = [&](int& target) -> bool {
        while (pos < text.size() && !iswdigit(text[pos])) ++pos;
        if (pos >= text.size()) return false;
        std::wstring digits;
        while (pos < text.size() && iswdigit(text[pos])) digits.push_back(text[pos++]);
        target = _wtoi(digits.c_str());
        return true;
    };
    if (!nextNumber(first)) return 0;
    if (first == 1 && nextNumber(second)) return second;
    return first;
}

// 从 <home>/release 读版本号；某些 JRE 没有 release 文件，再从 bin/java.exe 的
// 文件描述里兜底（这里只做 release，读不到就当未知）。
std::wstring ReadReleaseVersion(const std::wstring& home) {
    const std::string text = fs::ReadText(fs::Join(home, L"release"));
    if (text.empty()) return std::wstring();
    const std::string key = "JAVA_VERSION=";
    size_t pos = text.find(key);
    if (pos == std::string::npos) return std::wstring();
    pos += key.size();
    std::string value;
    if (pos < text.size() && text[pos] == '"') {
        ++pos;
        while (pos < text.size() && text[pos] != '"') value.push_back(text[pos++]);
    } else {
        while (pos < text.size() && text[pos] != '\r' && text[pos] != '\n') value.push_back(text[pos++]);
    }
    return Utf8ToWide(value);
}

void AddJavaHome(std::vector<JavaInfo>& out, std::set<std::wstring>& seen, const std::wstring& home) {
    if (home.empty()) return;
    const std::wstring normalized = Lower(home);
    if (seen.count(normalized)) return;

    JavaInfo info;
    info.home = home;
    const std::wstring javaw = fs::Join(fs::Join(home, L"bin"), L"javaw.exe");
    const std::wstring java = fs::Join(fs::Join(home, L"bin"), L"java.exe");
    if (fs::Exists(javaw)) info.path = javaw;
    else if (fs::Exists(java)) info.path = java;
    else return;

    info.version = ReadReleaseVersion(home);
    info.major = ParseMajorVersion(info.version);
    // 没有 release 文件时按 java 可执行文件猜测：视为未知（0），排序时排最后
    seen.insert(normalized);
    out.push_back(info);
}

void ScanRegistry(std::vector<JavaInfo>& out, std::set<std::wstring>& seen, HKEY root,
                  const std::wstring& subKey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return;

    wchar_t buffer[512]{};
    for (DWORD index = 0;; ++index) {
        DWORD nameSize = 512;
        const LONG rc = RegEnumKeyExW(key, index, buffer, &nameSize, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) break;

        HKEY child = nullptr;
        if (RegOpenKeyExW(key, buffer, 0, KEY_READ, &child) == ERROR_SUCCESS) {
            wchar_t home[MAX_PATH * 2]{};
            DWORD size = sizeof(home);
            DWORD type = 0;
            if (RegQueryValueExW(child, L"JavaHome", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(home), &size) == ERROR_SUCCESS &&
                type == REG_SZ) {
                AddJavaHome(out, seen, home);
            }
            RegCloseKey(child);
        }
    }
    RegCloseKey(key);
}

void ScanCommonDirs(std::vector<JavaInfo>& out, std::set<std::wstring>& seen) {
    const wchar_t* roots[] = {
        L"C:\\Program Files\\Java",
        L"C:\\Program Files\\Eclipse Adoptium",
        L"C:\\Program Files\\AdoptOpenJDK",
        L"C:\\Program Files\\Microsoft",
        L"C:\\Program Files\\Zulu",
        L"C:\\Program Files\\Amazon Corretto",
        L"C:\\Program Files\\BellSoft",
        L"C:\\Program Files (x86)\\Java",
    };
    for (const wchar_t* root : roots) {
        for (const std::wstring& child : fs::ListDirs(root)) {
            const std::wstring home = fs::Join(root, child);
            // Microsoft 目录下是 jdk-17.0.1 这类子目录，Java 目录下直接是 jdk-17
            if (fs::Exists(fs::Join(fs::Join(home, L"bin"), L"javaw.exe"))) {
                AddJavaHome(out, seen, home);
                continue;
            }
            for (const std::wstring& grand : fs::ListDirs(home)) {
                AddJavaHome(out, seen, fs::Join(home, grand));
            }
        }
    }
}

// 游戏目录自带的 runtime（旧版用官方 JRE 安装到这里）：runtime/<组件>/<平台>/<版本>
void ScanGameRuntime(std::vector<JavaInfo>& out, std::set<std::wstring>& seen,
                     const std::wstring& gameDir) {
    const std::wstring root = fs::Join(gameDir, L"runtime");
    for (const std::wstring& component : fs::ListDirs(root)) {
        const std::wstring comp = fs::Join(root, component);
        for (const std::wstring& platform : fs::ListDirs(comp)) {
            const std::wstring plat = fs::Join(comp, platform);
            for (const std::wstring& version : fs::ListDirs(plat)) {
                const std::wstring home = fs::Join(plat, version);
                if (fs::Exists(fs::Join(fs::Join(home, L"bin"), L"javaw.exe"))) {
                    AddJavaHome(out, seen, home);
                }
                // 多一层 <name> 目录的布局（java-runtime-gamma/gamma/...）
                for (const std::wstring& inner : fs::ListDirs(home)) {
                    AddJavaHome(out, seen, fs::Join(home, inner));
                }
            }
        }
    }
}

}  // namespace

int SystemMemoryMb() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return 8192;
    return static_cast<int>(status.ullTotalPhys / (1024 * 1024));
}

std::vector<JavaInfo> DetectJava(const std::wstring& gameDir) {
    std::vector<JavaInfo> found;
    std::set<std::wstring> seen;

    wchar_t buffer[MAX_PATH * 2]{};
    if (GetEnvironmentVariableW(L"JAVA_HOME", buffer, MAX_PATH * 2) > 0) {
        AddJavaHome(found, seen, buffer);
    }

    ScanGameRuntime(found, seen, gameDir);

    ScanRegistry(found, seen, HKEY_LOCAL_MACHINE, L"SOFTWARE\\JavaSoft\\JDK");
    ScanRegistry(found, seen, HKEY_LOCAL_MACHINE, L"SOFTWARE\\JavaSoft\\JRE");
    ScanRegistry(found, seen, HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\JavaSoft\\JDK");
    ScanRegistry(found, seen, HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\JavaSoft\\JRE");

    ScanCommonDirs(found, seen);

    // PATH 里的 java.exe（Oracle 的 javapath 是个转发器，指向真实 JRE，同样可用）
    if (const wchar_t* pathEnv = _wgetenv(L"PATH")) {
        std::wstring paths = pathEnv;
        size_t start = 0;
        while (start <= paths.size()) {
            const size_t end = paths.find(L';', start);
            const std::wstring part =
                paths.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!part.empty()) {
                const std::wstring javaw = fs::Join(part, L"javaw.exe");
                if (fs::Exists(javaw)) {
                    // javapath 这类目录的上一级不是 JDK，直接把它当独立候选
                    const std::wstring home = fs::ParentDir(part);
                    if (Lower(fs::BaseName(home)) == L"javapath") {
                        if (!seen.count(Lower(part))) {
                            JavaInfo info;
                            info.home = home;
                            info.path = javaw;
                            info.version = L"(PATH 转发)";
                            info.major = 0;
                            seen.insert(Lower(part));
                            found.push_back(info);
                        }
                    } else {
                        AddJavaHome(found, seen, home);
                    }
                }
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }

    std::sort(found.begin(), found.end(), [](const JavaInfo& a, const JavaInfo& b) {
        if (a.major != b.major) return a.major < b.major;
        return Lower(a.home) < Lower(b.home);
    });
    return found;
}

JavaInfo SelectJava(const nlohmann::json& settings, const std::wstring& gameDir, int requiredMajor,
                    std::wstring* error) {
    const bool manual = settings.value("javaMode", std::string("auto")) == "manual";
    const std::string configured = settings.value("javaPath", std::string());

    if (manual && !configured.empty()) {
        const std::wstring path = Utf8ToWide(configured);
        if (fs::Exists(path)) {
            JavaInfo info;
            info.path = path;
            info.home = fs::ParentDir(fs::ParentDir(path));
            info.version = ReadReleaseVersion(info.home);
            info.major = ParseMajorVersion(info.version);
            if (requiredMajor > 0 && info.major > 0 && info.major < requiredMajor) {
                if (error) {
                    *error = L"指定的 Java " + info.version + L" 版本过低，该版本需要 Java " +
                             std::to_wstring(requiredMajor) + L" 或更高";
                }
                return JavaInfo{};
            }
            return info;
        }
        if (error) *error = L"指定的 Java 路径不存在：" + path;
        return JavaInfo{};
    }

    const std::vector<JavaInfo> candidates = DetectJava(gameDir);
    const JavaInfo* best = nullptr;
    const JavaInfo* unknown = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.major == 0) {
            if (!unknown) unknown = &candidate;
            continue;
        }
        if (requiredMajor > 0 && candidate.major < requiredMajor) continue;
        // 优先"够用的最低版本"：Java 26 跑 1.21.1 也行，但 21 更贴近官方推荐
        if (!best || candidate.major < best->major) best = &candidate;
    }
    if (!best) best = unknown;  // 版本未知的 PATH 转发器：交给 Java 自己决定
    if (!best) {
        if (error) {
            *error = requiredMajor > 0
                         ? L"未找到 Java " + std::to_wstring(requiredMajor) +
                               L" 或更高版本，请在设置中手动指定 java(w).exe"
                         : L"未找到可用的 Java，请在设置中手动指定 java(w).exe";
        }
        return JavaInfo{};
    }
    return *best;
}

std::wstring PickFolder(HWND owner, const std::wstring& initial) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return std::wstring();
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    if (!initial.empty() && fs::IsDirectory(initial)) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }

    if (dialog->Show(owner) != S_OK) return std::wstring();

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::wstring();
    LPWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return std::wstring();
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

namespace {
std::wstring PickFile(HWND owner, const wchar_t* title, const wchar_t* filter, const std::wstring& initialDir) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return std::wstring();
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);

    const COMDLG_FILTERSPEC spec{title, filter};
    dialog->SetFileTypes(1, &spec);

    if (!initialDir.empty() && fs::IsDirectory(initialDir)) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialDir.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }

    if (dialog->Show(owner) != S_OK) return std::wstring();

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::wstring();
    LPWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return std::wstring();
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}
}  // namespace

std::wstring PickJavaFile(HWND owner) {
    return PickFile(owner, L"选择 Java 可执行文件", L"Java 可执行文件 (*.exe)\0*.exe\0",
                    L"C:\\Program Files\\Java");
}

std::wstring PickPngFile(HWND owner) {
    return PickFile(owner, L"选择皮肤图片", L"PNG 图片 (*.png)\0*.png\0", std::wstring());
}

}  // namespace voxel
