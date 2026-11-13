mod commands;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .invoke_handler(tauri::generate_handler![
            commands::state::get_state,
            commands::bootstrap::app_bootstrap,
            commands::bootstrap::app_info,
            commands::versions::versions_list,
            commands::window::window_minimize,
            commands::window::window_toggle_maximize,
            commands::window::window_toggle_fullscreen,
            commands::window::window_close,
        ])
        .run(tauri::generate_context!())
        .expect("failed to run tauri application");
}
