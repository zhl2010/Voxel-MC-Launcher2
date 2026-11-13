# Voxel MC Launcher 2

Minecraft 启动器的 **Tauri 2 + Rust** 重写版。旧 Electron 版见 [Voxel-MC-Launcher](https://github.com/zhl2010/Voxel-MC-Launcher)。

前端 UI 与旧版保持一致：`src/` 里的 `index.html` / `styles.css` / `app.js` 是从旧仓库原样搬过来的，**不是重写**。

## 当前状态

第一阶段：架构骨架。`crates/` 里是模块边界与类型签名，业务实现标着 `todo!()`。

## 结构

```
crates/
  voxel-core/     纯领域模型与纯函数（零 IO，可全量单测）
  voxel-net/      HTTP、镜像回退、下载、校验
  voxel-meta/     版本元数据、inheritsFrom、加载器
  voxel-runtime/  Java 探测、classpath、进程
  voxel-store/    配置、会话凭证、实例
src-tauri/        Tauri 命令层与窗口
src/              前端（旧版原样搬入）
```

依赖方向单向：`src-tauri → {store, meta, net, runtime} → core`。

## 开发

需要 Rust 工具链（本机未安装时先装 rustup），以及 WebView2 Runtime（Win10 22H2 起一般自带）。

```powershell
cargo check -p voxel-core
cargo tauri dev
```

## 关键约定

- 启动器数据目录 `%APPDATA%\Voxel-Launcher2\`，与旧版隔离；`.minecraft` 共用。
- `identifier` = `com.zhl2010.voxel-launcher2`，不要改，否则数据目录会变。
- 库代码禁止 `unwrap()`，统一走 `VoxelError`。
- classpath 超长时：Java 9+ 用 `@argfile`，Java 8 用 manifest jar。