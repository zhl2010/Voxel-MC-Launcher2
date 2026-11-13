use std::fmt;

/// 统一错误类型。库里不允许 unwrap()，边界统一转成字符串给前端。
#[derive(Debug)]
pub enum VoxelError {
    Io(String),
    Http(String),
    Json(String),
    /// 所有镜像都失败：记录最后路径与最后一次错误
    MirrorExhausted { path: String, last: String },
    HashMismatch { path: String, expect: String, actual: String },
    JavaNotFound { hint: String },
    /// classpath 超过 OS 命令行上限，且无法用 argfile / manifest jar 绕过
    CmdlineTooLong { len: usize, limit: usize, java_major: u8 },
    /// 元数据语义错误（缺字段、继承链断裂、非法坐标等）
    Meta(String),
}

impl fmt::Display for VoxelError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            VoxelError::Io(s) => write!(f, "io: {s}"),
            VoxelError::Http(s) => write!(f, "http: {s}"),
            VoxelError::Json(s) => write!(f, "json: {s}"),
            VoxelError::MirrorExhausted { path, last } => {
                write!(f, "all mirrors failed for {path}: {last}")
            }
            VoxelError::HashMismatch { path, expect, actual } => {
                write!(f, "hash mismatch {path}: expect {expect}, got {actual}")
            }
            VoxelError::JavaNotFound { hint } => write!(f, "java not found: {hint}"),
            VoxelError::CmdlineTooLong { len, limit, java_major } => {
                write!(f, "cmdline too long ({len} > {limit}), java {java_major}")
            }
            VoxelError::Meta(s) => write!(f, "{s}"),
        }
    }
}

impl std::error::Error for VoxelError {}