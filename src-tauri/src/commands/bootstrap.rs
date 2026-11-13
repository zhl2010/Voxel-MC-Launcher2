use serde::Serialize;

use super::config::{self, Settings};

/// 账户摘要。旧 `game-service.js` 的 `getAccountSummary()` 只暴露这几项，
/// 访问令牌一律不出后端 —— 前端只需要画画头像和昵称。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AccountSummary {
    pub name: String,
    pub uuid: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub online: bool,
    pub server: Option<String>,
}

/// 当前正在运行的游戏（`status != "idle"` 时才有值）。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CurrentLaunch {
    pub version: String,
    pub loader: String,
    pub loader_version: String,
    pub loader_label: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GameStateSnapshot {
    pub status: String,
    pub current: Option<CurrentLaunch>,
    pub account: Option<AccountSummary>,
}

/// 环境信息面板（旧版显示 Electron/Chrome/Node 版本的地方）。
/// Rust 版没有那三样，"electron/chrome/node" 保留字段但填占位值，
/// 否则前端 `updateStaticInfo` 取到 undefined 会渲染出 "undefined"。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppInfo {
    pub version: String,
    pub eml_version: String,
    pub electron: String,
    pub chrome: String,
    pub node: String,
    pub platform: String,
    pub arch: String,
    pub game_dir: String,
    pub default_game_dir: String,
    pub game_dir_is_custom: bool,
    pub versions_dir: String,
    pub instance_dir: String,
    pub data_dir: String,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BootstrapPayload {
    pub settings: Settings,
    pub account: Option<AccountSummary>,
    pub game_state: GameStateSnapshot,
    pub app_info: AppInfo,
}

fn build_app_info(settings: &Settings) -> AppInfo {
    let game_dir = config::effective_game_dir(settings);
    let default_dir = config::minecraft_dir();
    let custom = game_dir != default_dir;

    // 版本隔离开启时，实例目录是 `<gameDir>/versions/<version>`，否则就是游戏根目录。
    let instance_dir = if settings.version_isolation && !settings.version.is_empty() {
        game_dir.join("versions").join(&settings.version)
    } else {
        game_dir.clone()
    };

    AppInfo {
        version: env!("CARGO_PKG_VERSION").to_string(),
        eml_version: "—".into(),
        electron: "—".into(),
        chrome: "—".into(),
        node: "—".into(),
        platform: "win32".into(),
        arch: std::env::consts::ARCH.to_string(),
        game_dir: display(&game_dir),
        default_game_dir: display(&default_dir),
        game_dir_is_custom: custom,
        versions_dir: display(&game_dir.join("versions")),
        instance_dir: display(&instance_dir),
        data_dir: display(&config::app_data_dir()),
    }
}

/// 统一把 PathBuf 变成前端可见的字符串；非 UTF-8 路径在这里一次性收敛掉，
/// 避免下游每一处都写 `to_string_lossy`。
fn display(path: &std::path::Path) -> String {
    path.to_string_lossy().into_owned()
}

/// 对齐旧 preload 的 `app:bootstrap`：一次请求把界面首屏需要的状态全给齐，
/// 省掉三次 IPC 往返（Tauri 的 IPC 序列化比 Electron 便宜，但也没必要多花钱）。
#[tauri::command]
pub fn app_bootstrap() -> Result<BootstrapPayload, String> {
    let settings = config::load_settings();
    Ok(BootstrapPayload {
        app_info: build_app_info(&settings),
        // 骨架阶段还没有账户子系统：DPAPI 加密的 session 文件尚未接入
        // （后续放到 voxel-store::session）。先如实返回"未登录"。
        account: None,
        game_state: GameStateSnapshot {
            status: "idle".into(),
            current: None,
            account: None,
        },
        settings,
    })
}

#[tauri::command]
pub fn app_info() -> Result<AppInfo, String> {
    Ok(build_app_info(&config::load_settings()))
}
