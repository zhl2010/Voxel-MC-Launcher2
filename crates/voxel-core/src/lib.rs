//! voxel-core：纯领域模型与纯函数。
//!
//! 铁律：本 crate 不出现 std::fs、reqwest、std::process。
//! 输入是两个已加载的 VersionJson，输出是 ResolvedVersion 与一组 args token。
//! 这样设计的目的：全量可单测，夹具直接用真实的 1.20.1 / 1.12.2 / Fabric 实例 json。

pub mod error;
pub mod launch_ctx;
pub mod model;
pub mod resolve;

pub use error::VoxelError;
pub use model::*;