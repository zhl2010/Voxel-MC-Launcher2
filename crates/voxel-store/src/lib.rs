//! voxel-store：配置、会话凭证、实例清单。
//!
//! 计划模块：config.rs（原子写，serde）、session.rs（keyring 优先，失败降级 DPAPI 加密文件）、
//! instance.rs（实例注册表）

pub const CRATE_STUB: &str = "voxel-store";