use serde::Serialize;

/// 前端启动时拉一次的状态快照。
/// 字段名与旧 preload.cjs 暴露的形状保持一致，避免改动 renderer/app.js。
#[derive(Serialize)]
pub struct AppStateSnapshot {
    pub launcher_version: String,
    pub data_dir: String,
    pub minecraft_dir: String,
}

/// 对应旧链路 window.voxel.getState()
#[tauri::command]
pub fn get_state() -> Result<AppStateSnapshot, String> {
    Ok(AppStateSnapshot {
        launcher_version: env!("CARGO_PKG_VERSION").to_string(),
        data_dir: "stub".into(),
        minecraft_dir: "stub".into(),
    })
}