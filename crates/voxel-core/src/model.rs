//! 领域模型。
//!
//! 注意：骨架阶段用 String 占位 Url / DateTime / Uuid，避免第一版就拉依赖。
//! Vol.2 实现阶段替换为 url::Url、chrono::DateTime<Utc>、uuid::Uuid（serde 直接反序列化）。

pub struct InstanceId(pub String);
pub struct VersionId(pub String);

/// SHA1 用定长数组而不是 String：省掉每次比较都要从 hex 解析。
pub struct Sha1(pub [u8; 20]);

pub struct LatestVersions {
    pub release: String,
    pub snapshot: String,
}

pub struct VersionManifest {
    pub latest: LatestVersions,
    pub versions: Vec<VersionSummary>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VersionType {
    Release,
    Snapshot,
    OldBeta,
    OldAlpha,
}

pub struct VersionSummary {
    pub id: String,
    pub kind: VersionType,
    /// 该版本 json 的下载地址（不是 client jar）
    pub url: String,
    pub released: String,
    pub sha1: Option<Sha1>,
}

pub struct AssetIndexRef {
    pub id: String,
    pub url: String,
    pub sha1: Option<Sha1>,
    pub size: u64,
    pub total_size: u64,
}

pub struct Downloads {
    pub client: Option<DownloadEntry>,
    pub server: Option<DownloadEntry>,
}

pub struct DownloadEntry {
    pub url: String,
    pub sha1: Option<Sha1>,
    pub size: u64,
}

pub struct JavaVersion {
    pub component: String,
    pub major_version: u8,
}

pub struct Arguments {
    pub game: Vec<Argument>,
    pub jvm: Vec<Argument>,
}

pub enum ValueOrVec {
    One(String),
    Many(Vec<String>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuleAction {
    Allow,
    Disallow,
}

pub struct Rule {
    pub action: RuleAction,
    pub os: Option<OsRule>,
    pub features: Option<std::collections::HashMap<String, bool>>,
}

pub struct OsRule {
    pub name: Option<String>,
    pub arch: Option<String>,
    pub version: Option<String>,
}

pub struct Library {
    /// group:artifact:version[:classifier]
    pub name: String,
    pub downloads: Option<LibraryDownloads>,
    pub natives: Option<std::collections::HashMap<String, String>>,
    pub rules: Vec<Rule>,
    pub extract: Option<Extract>,
}

pub struct LibraryDownloads {
    pub artifact: Option<DownloadEntry>,
    pub classifiers: Option<std::collections::HashMap<String, DownloadEntry>>,
}

pub struct Extract {
    pub exclude: Vec<String>,
}

pub enum Argument {
    Plain(String),
    Conditional { rules: Vec<Rule>, value: ValueOrVec },
}

/// 原始 version json。1.13 以后用 arguments，1.12 及以前用 minecraftArguments，
/// 两者并存时以 arguments 为准（少数混合版本才有这种情况）。
pub struct VersionJson {
    pub id: String,
    /// 非空即需要与父版本合并
    pub inherits_from: Option<String>,
    pub main_class: Option<String>,
    pub asset_index: Option<AssetIndexRef>,
    pub assets: Option<String>,
    pub downloads: Option<Downloads>,
    pub java_version: Option<JavaVersion>,
    pub libraries: Vec<Library>,
    pub arguments: Option<Arguments>,
    pub minecraft_arguments: Option<String>,
}

/// 合并 inheritsFrom、求值 rules 之后的最终形态。
/// 下游（net / runtime / store）只信任这个类型，不再碰 VersionJson。
pub struct ResolvedVersion {
    pub id: String,
    pub main_class: String,
    pub libraries: Vec<Library>,
    pub game_args: Vec<Argument>,
    pub jvm_args: Vec<Argument>,
    pub asset_index: AssetIndexRef,
    pub java_version: Option<JavaVersion>,
    pub client_jar: Option<DownloadEntry>,
    /// 合并过程中产生的告警（例如父版本缺 mainClass、rules 无法判定）
    pub warnings: Vec<String>,
}

/// 运行环境上下文，rules 求值需要它判断当前 OS / 特性开关。
pub struct EnvContext {
    pub os_name: String,
    pub os_arch: String,
    pub os_version: String,
    /// 例如 has_custom_resolution / is_demo_user，来自启动器的特性开关
    pub features: std::collections::HashMap<String, bool>,
}