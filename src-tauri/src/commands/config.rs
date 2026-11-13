use std::path::PathBuf;

use serde::{Deserialize, Serialize};

/// 启动器设置项。
///
/// 字段名必须与旧 `config-store.js` 的 `DEFAULT_SETTINGS` 逐字对应：前端 `app.js`
/// 直接按 `settings.uiScale` / `settings.accentColor` 这种名字取值，改一个名字就会
/// 静默失效（不报错、但界面永远用默认值），所以这里是"契约"而不是普通结构体。
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct Settings {
    pub username: String,
    pub version: String,
    pub loader: String,
    pub loader_version: String,
    pub min_memory: u32,
    pub max_memory: u32,
    pub window_width: u32,
    pub window_height: u32,
    pub fullscreen: bool,
    pub java_mode: String,
    pub java_path: String,
    pub jvm_args: String,
    pub ui_scale: u32,
    pub font_scale: u32,
    pub accent_color: String,
    pub theme_mode: String,
    pub game_dir: String,
    pub proxy_url: String,
    pub mirror_mojang: bool,
    pub mirror_fabric: bool,
    pub mirror_forge: bool,
    /// 旧配置里的键名是 `mirrorNeoForge`（大写 N），camelCase 规则会生成
    /// `mirrorNeoforge`，所以这里必须显式覆盖。
    #[serde(rename = "mirrorNeoForge")]
    pub mirror_neoforge: bool,
    pub force_local: bool,
    pub version_isolation: bool,
    pub download_source: String,
    pub last_played: String,
}

impl Default for Settings {
    fn default() -> Self {
        // 与 config-store.js 的 DEFAULT_SETTINGS 一致：任何一项不同都会让首次启动的
        // 界面状态和旧版对不上。
        Self {
            username: String::new(),
            version: String::new(),
            loader: "vanilla".into(),
            loader_version: String::new(),
            min_memory: 1024,
            max_memory: 4096,
            window_width: 1280,
            window_height: 720,
            fullscreen: false,
            java_mode: "auto".into(),
            java_path: String::new(),
            jvm_args: String::new(),
            ui_scale: 100,
            font_scale: 100,
            accent_color: "#eef0ea".into(),
            theme_mode: "dark".into(),
            game_dir: String::new(),
            proxy_url: String::new(),
            mirror_mojang: true,
            mirror_fabric: true,
            mirror_forge: true,
            mirror_neoforge: true,
            force_local: false,
            version_isolation: false,
            download_source: String::new(),
            last_played: String::new(),
        }
    }
}

/// 启动器自身的数据目录（配置、账号、日志），与 `.minecraft` 分开：
/// 两个版本的启动器共用同一个游戏目录，但各自的账号/配置互不干扰。
pub fn app_data_dir() -> PathBuf {
    std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_default()
        .join("Voxel-Launcher2")
}

/// 旧 Electron 版的 userData 目录（productName = "Voxel Launcher"）。
/// 骨架阶段先读它的配置与清单缓存，用户升级后界面状态不会突然清空。
pub fn legacy_data_dir() -> PathBuf {
    std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_default()
        .join("Voxel Launcher")
}

/// 默认游戏目录：`%APPDATA%\.minecraft`，与旧版/官方启动器共用。
pub fn minecraft_dir() -> PathBuf {
    std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_default()
        .join(".minecraft")
}

fn read_json(path: &PathBuf) -> Option<serde_json::Value> {
    let raw = std::fs::read_to_string(path).ok()?;
    serde_json::from_str(&raw).ok()
}

/// 读取设置：新版目录优先，其次旧 Electron 版目录，最后回落默认值。
///
/// 任何一步失败都不视为致命错误 —— 配置损坏绝不能让启动器打不开，旧版
/// `ConfigStore::read` 也是这个语义。
pub fn load_settings() -> Settings {
    let candidates = [
        app_data_dir().join("launcher-config.json"),
        legacy_data_dir().join("launcher-config.json"),
    ];

    for path in candidates.iter() {
        let Some(value) = read_json(path) else { continue };
        let Some(settings) = value.get("settings") else { continue };
        // 逐字段反序列化：缺失字段由 Default 补齐，多出的字段被忽略，
        // 旧版本配置里已废弃的键不会导致整体读取失败。
        if let Ok(parsed) = serde_json::from_value::<Settings>(settings.clone()) {
            return parsed;
        }
    }

    Settings::default()
}

/// 实际生效的游戏目录：用户在设置里填了自定义目录就用它，否则用默认 `.minecraft`。
pub fn effective_game_dir(settings: &Settings) -> PathBuf {
    let custom = settings.game_dir.trim();
    if !custom.is_empty() {
        let candidate = PathBuf::from(custom);
        if candidate.is_absolute() {
            return candidate;
        }
    }
    minecraft_dir()
}
