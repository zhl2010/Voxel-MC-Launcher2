use tauri::{Emitter, WebviewWindow};

/// 把窗口状态变化推给前端，让标题栏的最大化/全屏图标跟着切换。
/// 前端 `onWindowState` 依赖 `window:state` 这个事件名和 `{maximized, fullScreen}` 形状。
fn notify_state(window: &WebviewWindow) -> Result<(), String> {
    let maximized = window.is_maximized().map_err(|e| e.to_string())?;
    // 全屏查询失败不该阻塞标题栏刷新：退化成 false，图标至多短暂不一致。
    let full_screen = window.is_fullscreen().unwrap_or(false);
    window
        .emit(
            "window:state",
            serde_json::json!({ "maximized": maximized, "fullScreen": full_screen }),
        )
        .map_err(|e| e.to_string())
}

#[tauri::command]
pub fn window_minimize(window: WebviewWindow) -> Result<(), String> {
    window.minimize().map_err(|e| e.to_string())
}

#[tauri::command]
pub fn window_toggle_maximize(window: WebviewWindow) -> Result<(), String> {
    let maximized = window.is_maximized().map_err(|e| e.to_string())?;
    if maximized {
        window.unmaximize().map_err(|e| e.to_string())?;
    } else {
        window.maximize().map_err(|e| e.to_string())?;
    }
    notify_state(&window)
}

#[tauri::command]
pub fn window_toggle_fullscreen(window: WebviewWindow) -> Result<(), String> {
    let full_screen = window.is_fullscreen().map_err(|e| e.to_string())?;
    window
        .set_fullscreen(!full_screen)
        .map_err(|e| e.to_string())?;
    notify_state(&window)
}

#[tauri::command]
pub fn window_close(window: WebviewWindow) -> Result<(), String> {
    window.close().map_err(|e| e.to_string())
}
