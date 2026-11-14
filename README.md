# Voxel MC Launcher 2

Minecraft 启动器的 **C++ / WebView2** 实现的延续（旧 Electron 版：[Voxel-MC-Launcher](https://github.com/zhl2010/Voxel-MC-Launcher)）。

前端 UI 与旧版保持一致：`src/` 里的 `index.html` / `styles.css` / `app.js` 是从旧仓库原样搬过来的，**不是重写**。

## 结构

```
main.cpp          宿主入口：窗口、单实例、IPC 命令表
app/              窗口、WebView2 宿主、内嵌资源、与前端之间的 IPC 桥
core/             配置、清单、账户、目录、加密、安装器、进程
net/              WinHTTP 封装
launch/           版本解析、classpath、参数拼装、进程拉起
src/              前端源码（构建时编进 exe，发布产物不需要它）
resources.rc      把 src/ 下的四个文件以 RCDATA 形式嵌入
tools/            无界面自检（不进发布产物）
third_party/      WebView2 SDK、nlohmann::json
```

## 单体 exe

构建产物 `build\voxel-launcher.exe` 是**单文件**：前端（`index.html` / `styles.css` /
`app.js` / `voxel-api.js`）由 `resources.rc` 编进 PE，运行时通过 WebView2 的
`WebResourceRequested` 从内存应答 `https://voxel.local/*`，不再需要 exe 旁边的
`src/` 目录。改了前端必须重新编译，否则 exe 里还是旧资源。

`build.cmd` 还会把同一份 exe 复制到 `%LOCALAPPDATA%\VoxelLauncher\`，作为**交付副本**，
原因见下。

## 关于"必须以管理员身份运行"

如果 exe 所在的目录被打了**低完整性标签**（`Mandatory Label\Low Mandatory Level`），
在此编出的 exe 会继承该标签。低完整性的 exe 以普通用户启动时，WebView2 的浏览器进程
会在 `msedge_elf.dll` 里崩溃（异常码 `0x80000003`），表现为"只有管理员能打开"。

DSH 沙箱会给工作区目录打这个标签，所以在工作区里直接运行的产物会中招。两种解法：

- 用 `build.cmd` 产出的交付副本（`%LOCALAPPDATA%\VoxelLauncher\voxel-launcher.exe`），
  它不带低标签，普通双击即可；
- 或者对工作区内的 exe 执行一次（需管理员）：
  `icacls build\voxel-launcher.exe /setintegritylevel Medium`

重新编译会覆盖 exe 并重新继承目录标签，所以每次构建后都需要重新复制或重设。

## 开发

需要 Visual Studio 2022 Build Tools（含 C++ 工作负载）与 Windows SDK，以及 WebView2 Runtime（Win10 1803+ 通常自带）。

```powershell
.\build.cmd            # 编译宿主 → build\voxel-launcher.exe
.\build-selftest.cmd   # 编译自检 → build\selftest.exe
.\build\selftest.exe   # 跑自检（联网清单、账户、Java 探测、文件校验、启动）
```

`build.cmd` 里写死了 MSVC 与 SDK 的版本路径，换机器时改文件头部的 `MSVC` / `SDKVER` 即可。

## 关键约定

- 启动器数据目录 `%APPDATA%\Voxel Launcher\`，与旧版共用；`.minecraft` 也共用。
- WebView2 用户数据目录 `%LOCALAPPDATA%\Voxel-Launcher2\WebView2\`，与配置目录分开。
  该目录损坏会让 WebView2 报 `0x800700AA`（ERROR_BUSY），宿主会自动把它改名重建后重试一次。
- classpath 超长时：Java 9+ 用 `@argfile`，Java 8 用 manifest jar。
- 库代码不使用异常做控制流；对外一律返回 `bool` + `std::string* error`。

## 已知问题

WebView2 运行时若处于"半损坏"状态（版本号可读、浏览器进程起不来），宿主会弹窗给出
真实 HRESULT。此时需修复/重装 Microsoft Edge WebView2 Runtime 的**独立离线包**
（Standalone Installer，不是 1.9 MB 的 bootstrapper——后者只比对版本，已装就直接退出）。
