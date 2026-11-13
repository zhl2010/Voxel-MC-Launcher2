use std::path::{Path, PathBuf};

use serde::Serialize;

use super::config;

/// 版本列表项。字段与旧 `version-service.js` 的 `format()` 输出一致：
/// `id / type / releaseTime / installed / local`，前端 `renderVersionList` 直接吃这几个键。
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VersionEntry {
    pub id: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub release_time: Option<String>,
    pub installed: bool,
    pub local: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LatestVersions {
    pub release: String,
    pub snapshot: String,
}

#[derive(Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VersionListPayload {
    pub latest: LatestVersions,
    pub versions: Vec<VersionEntry>,
    pub source: String,
    pub error: Option<String>,
}

/// 网络不可用时仍然要给出一个能点的清单，否则版本库页面直接空白。
/// 这一组与旧版 `version-service.js` 的 `FALLBACK_VERSIONS` 保持一致。
const FALLBACK_VERSIONS: [&str; 11] = [
    "26.3",
    "1.21.8",
    "1.21.4",
    "1.21.1",
    "1.20.6",
    "1.20.1",
    "1.19.4",
    "1.18.2",
    "1.16.5",
    "1.12.2",
    "1.8.9",
];

/// 扫描 `<gameDir>/versions/` 下的本地实例。
///
/// 判定标准沿用旧版：目录名 + 同名 `.json` 同时存在才算一个可启动版本。
/// PCL/HMCL 导入的实例、Fabric/Forge 独立实例都符合这个形状。
fn scan_local_versions(game_dir: &Path) -> Vec<VersionEntry> {
    let versions_dir = game_dir.join("versions");
    let Ok(entries) = std::fs::read_dir(&versions_dir) else {
        return Vec::new();
    };

    let mut found = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let Some(name) = path.file_name().and_then(|n| n.to_str()) else {
            continue;
        };
        let manifest = path.join(format!("{name}.json"));
        if !manifest.is_file() {
            continue;
        }
        // 版本 json 里的 type 决定前端用哪种标签配色（正式版/快照/远古版）。
        let kind = std::fs::read_to_string(&manifest)
            .ok()
            .and_then(|raw| serde_json::from_str::<serde_json::Value>(&raw).ok())
            .and_then(|v| v.get("type").and_then(|t| t.as_str()).map(str::to_string))
            .unwrap_or_else(|| "release".to_string());

        found.push(VersionEntry {
            id: name.to_string(),
            kind,
            release_time: None,
            installed: true,
            local: true,
        });
    }
    found
}

/// 读取 Mojang 清单缓存（旧版拉过就存在磁盘上）。
/// 返回 (latest, 官方版本条目)。缓存是纯展示用途，读不到就走兜底列表。
fn read_manifest_cache() -> Option<(LatestVersions, Vec<VersionEntry>)> {
    let candidates: [PathBuf; 2] = [
        config::app_data_dir().join("version-manifest-cache.json"),
        config::legacy_data_dir().join("version-manifest-cache.json"),
    ];

    for path in candidates.iter() {
        let Some(raw) = std::fs::read_to_string(path).ok() else {
            continue;
        };
        let Ok(value) = serde_json::from_str::<serde_json::Value>(&raw) else {
            continue;
        };
        let Some(list) = value.get("versions").and_then(|v| v.as_array()) else {
            continue;
        };

        let latest = LatestVersions {
            release: value
                .pointer("/latest/release")
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_string(),
            snapshot: value
                .pointer("/latest/snapshot")
                .and_then(|v| v.as_str())
                .unwrap_or_default()
                .to_string(),
        };

        let entries = list
            .iter()
            .filter_map(|item| {
                let id = item.get("id")?.as_str()?.to_string();
                Some(VersionEntry {
                    id,
                    kind: item
                        .get("type")
                        .and_then(|t| t.as_str())
                        .unwrap_or("release")
                        .to_string(),
                    release_time: item
                        .get("releaseTime")
                        .and_then(|t| t.as_str())
                        .map(str::to_string),
                    installed: false,
                    local: false,
                })
            })
            .collect();

        return Some((latest, entries));
    }

    None
}

/// 版本清单。旧版会打网络请求（10 分钟缓存 + 磁盘缓存 + 兜底三级），
/// Rust 版骨架阶段先用"磁盘缓存 + 本地扫描 + 兜底"这条离线链路：
/// 它不依赖 reqwest（那个属于 voxel-net 的下一个模块），但足以让版本库页面完整可交互。
#[tauri::command]
pub fn versions_list(force: bool) -> Result<VersionListPayload, String> {
    // force 暂时无意义：目前没有网络拉取分支可跳过缓存。留参数是为了
    // 前端 `api.getVersions(force)` 的调用形状不变。
    let _ = force;

    let settings = config::load_settings();
    let game_dir = config::effective_game_dir(&settings);
    let local = scan_local_versions(&game_dir);

    let (latest, mut official, source, error) = match read_manifest_cache() {
        Some((latest, official)) => (latest, official, "cache".to_string(), None),
        None => {
            let fallback = FALLBACK_VERSIONS
                .iter()
                .map(|id| VersionEntry {
                    id: (*id).to_string(),
                    kind: "release".into(),
                    release_time: None,
                    installed: false,
                    local: false,
                })
                .collect();
            (
                LatestVersions {
                    release: FALLBACK_VERSIONS[0].to_string(),
                    snapshot: FALLBACK_VERSIONS[0].to_string(),
                },
                fallback,
                "fallback".to_string(),
                Some("未能读取版本清单缓存，已使用内置列表。".to_string()),
            )
        }
    };

    // 官方条目如果在本机已有同名实例，标记为已安装（前端据此加"已安装"角标）。
    let installed_ids: Vec<&str> = local.iter().map(|v| v.id.as_str()).collect();
    for entry in official.iter_mut() {
        entry.installed = installed_ids.contains(&entry.id.as_str());
    }

    // 只把"官方清单里没有的本地版本"并入列表，避免同一个版本出现两次。
    let extra_local: Vec<VersionEntry> = local
        .into_iter()
        .filter(|v| !official.iter().any(|o| o.id == v.id))
        .collect();

    Ok(VersionListPayload {
        latest,
        versions: extra_local.into_iter().chain(official).collect(),
        source,
        error,
    })
}
