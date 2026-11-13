//! voxel-net：HTTP 客户端、镜像回退、下载与校验。
//!
//! 计划模块：client.rs（reqwest 单例 + 代理）、mirror.rs（单文件粒度回退）、
//! download.rs（并发闸门 + 断点续传 + 进度事件）、verify.rs（sha1 校验）。

/// 计划依赖：tokio, reqwest(rustls-tls, stream, socks), sha1, futures, tracing
pub const CRATE_STUB: &str = "voxel-net";