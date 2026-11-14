#include "config.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace voxel {
namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) {
        if (p) CoTaskMemFree(p);
        return std::wstring();
    }
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}

// 读整个文件（二进制），剥掉 UTF-8 BOM —— 旧版写文件时可能带 BOM，
// nlohmann 遇到 BOM 会直接抛 parse_error。
std::string ReadTextFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() >= 3 && data[0] == '\xEF' && data[1] == '\xBB' && data[2] == '\xBF') {
        data.erase(0, 3);
    }
    return data;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

}  // namespace

std::wstring UserDataDir() {
    std::wstring roaming = KnownFolder(FOLDERID_RoamingAppData);
    if (roaming.empty()) return L".";
    return roaming + L"\\Voxel Launcher";
}

std::wstring ConfigFile() {
    return UserDataDir() + L"\\launcher-config.json";
}

nlohmann::json DefaultSettings() {
    return nlohmann::json{
        {"username", ""},
        {"version", ""},
        {"loader", "vanilla"},
        {"loaderVersion", ""},
        {"minMemory", 1024},
        {"maxMemory", 4096},
        {"windowWidth", 1280},
        {"windowHeight", 720},
        {"fullscreen", false},
        {"javaMode", "auto"},
        {"javaPath", ""},
        {"jvmArgs", ""},
        {"uiScale", 100},
        {"fontScale", 100},
        {"accentColor", "#eef0ea"},
        {"themeMode", "dark"},
        {"gameDir", ""},
        {"proxyUrl", ""},
        {"mirrorMojang", true},
        {"mirrorFabric", true},
        {"mirrorForge", true},
        {"mirrorNeoForge", true},
        {"forceLocal", false},
        {"versionIsolation", false},
        {"downloadSource", ""},
        {"lastPlayed", ""}};
}

nlohmann::json MergeAllowed(const nlohmann::json& patch, const nlohmann::json& base) {
    nlohmann::json next = base;
    if (!patch.is_object()) return next;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        // 只认默认配置里出现过的键，未知字段一律丢弃（避免老版本脏字段污染）
        if (!next.contains(it.key())) continue;
        if (it.value().is_null()) continue;
        next[it.key()] = it.value();
    }
    return next;
}

// MergeAllowed 只挡未知键和 null，挡不住"该是数字的字段被写成字符串"这类脏数据。
// 后面 get<int>() 遇到非数字会抛异常，而 LoadSettings 在启动路径上，先把类型归一化。
void CoerceTypes(nlohmann::json& settings) {
    const nlohmann::json defaults = DefaultSettings();
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!settings.contains(it.key())) continue;
        const nlohmann::json& value = settings[it.key()];
        const bool typeOk = it.value().is_number()
                                ? value.is_number()
                                : it.value().is_boolean() ? value.is_boolean()
                                                          : value.is_string();
        if (!typeOk) settings[it.key()] = it.value();
    }
}

nlohmann::json LoadSettings() {
    nlohmann::json defaults = DefaultSettings();
    const std::string raw = ReadTextFile(ConfigFile());
    if (raw.empty()) return defaults;

    nlohmann::json parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return defaults;

    if (!parsed.contains("settings") || !parsed["settings"].is_object()) return defaults;
    nlohmann::json merged = MergeAllowed(parsed["settings"], defaults);
    CoerceTypes(merged);

    // 与旧版一致：minMemory 必须小于 maxMemory
    if (merged["minMemory"].get<int>() >= merged["maxMemory"].get<int>()) {
        int maxMem = merged["maxMemory"].get<int>();
        merged["minMemory"] = std::max(512, maxMem - 512);
    }
    return merged;
}

bool SaveSettings(const nlohmann::json& settings) {
    const std::wstring dir = UserDataDir();
    CreateDirectoryW(dir.c_str(), nullptr);

    nlohmann::json root = nlohmann::json::object();
    // 保留原文件的 createdAt，避免每次保存都刷新创建时间
    const std::string raw = ReadTextFile(ConfigFile());
    if (!raw.empty()) {
        nlohmann::json old = nlohmann::json::parse(raw, nullptr, false);
        if (!old.is_discarded() && old.is_object() && old.contains("createdAt")) {
            root["createdAt"] = old["createdAt"];
        }
    }
    if (!root.contains("createdAt")) root["createdAt"] = "";  // 由前端忽略，仅为结构对齐

    root["settings"] = MergeAllowed(settings, DefaultSettings());

    const std::wstring file = ConfigFile();
    const std::wstring temp = file + L".tmp";
    if (!WriteTextFile(temp, root.dump(2))) return false;
    return MoveFileExW(temp.c_str(), file.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

}  // namespace voxel
