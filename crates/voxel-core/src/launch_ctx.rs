use std::path::PathBuf;

use crate::error::VoxelError;
use crate::model::*;

/// 占位符变量集中在这里，避免 ${...} 替换逻辑散落到各处。
pub struct LaunchContext {
    pub java_major: u8,
    pub classpath: Vec<PathBuf>,
    pub natives_dir: PathBuf,
    pub game_dir: PathBuf,
    pub assets_dir: PathBuf,
    pub asset_index_name: String,
    pub version_name: String,
    pub player_name: String,
    pub player_uuid: String,
    /// 令牌只以脱敏形态存在，Debug 打印时不可泄露
    pub access_token: RedactedString,
    pub max_memory_mb: u32,
    pub min_memory_mb: Option<u32>,
    pub extra_jvm: Vec<String>,
    pub extra_game: Vec<String>,
    pub window: Option<WindowSize>,
}

pub struct RedactedString(pub String);

impl std::fmt::Debug for RedactedString {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "***")
    }
}

impl std::fmt::Display for RedactedString {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "***")
    }
}

pub struct WindowSize {
    pub width: u32,
    pub height: u32,
}

/// 渲染参数模板：替换 ${natives_directory}、${classpath} 等占位符。
pub fn render_arguments(args: &[Argument], ctx: &LaunchContext) -> Vec<String> {
    let _ = (args, ctx);
    todo!("Vol.2 实现：占位符替换 + rules 过滤")
}

/// 按坐标 -> 本地路径规则拼 classpath，缺失的 jar 直接跳过并记录告警。
pub fn build_classpath(version: &ResolvedVersion, libs_root: &std::path::Path) -> Vec<PathBuf> {
    let _ = (version, libs_root);
    todo!("Vol.2 实现：maven 坐标转路径")
}

/// classpath 的三种投递方式。
///
/// 这是本次重构性价比最高的一处收敛：上层只要求「给我一个能塞进 -cp 的东西」，
/// 不再有「要不要拼串」的分支散落在启动逻辑里。
pub enum ClasspathArg {
    /// 直接作为 -cp 的值（长度安全时首选）
    Inline(String),
    /// Java 9+ 的 @argfile
    ArgFile(PathBuf),
    /// Java 8 的 manifest jar（Class-Path 属性）
    ManifestJar(PathBuf),
}

/// # Errors
/// 三路都不可用时返回 `VoxelError::CmdlineTooLong`。
pub fn classpath_to_arg(
    classpath: &[PathBuf],
    java_major: u8,
    work_dir: &std::path::Path,
) -> Result<ClasspathArg, VoxelError> {
    let _ = (classpath, java_major, work_dir);
    todo!("Vol.2 实现：长度判定 + argfile / manifest jar 落盘")
}