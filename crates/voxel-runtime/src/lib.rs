//! voxel-runtime：Java 探测、classpath、进程启动、日志管线。
//!
//! 计划模块：java.rs（注册表 + JAVA_HOME 探测）、launch.rs（Command + Job Object）、
//! verify.rs（rayon 并行校验）、job.rs（windows-sys Job Object 防孤儿进程）

pub const CRATE_STUB: &str = "voxel-runtime";