# 词光 Word Atlas · 桌面单机版

把 [word-atlas](../word-atlas) 这个 Vite + React 的雅思词汇图谱网页，打包成 Windows 单机程序：
双击 `WordAtlas.exe` 就能用，不联网、不装浏览器、不需要 Node。

外壳（HTML / JS / CSS / 字体 / 品牌插画 / 音效，约 7 MB）编译进 exe 的资源段，
7,000+ 词的插画、缩略图、英美发音和词条分块（约 620 MB）放在 exe 同目录的 `atlas/` 下按需读取，
所以 exe 只有 8 MB 左右，启动快、内存占用低，音频还能拖动进度（HTTP Range / 206）。

技术底座沿用 [maze](../maze) 项目的 **TauriCPP**（C++17 + Win32 + WebView2），
只做了三处必要增强：外置数据目录、Range 分段响应、窗口图标 / 外链处理。

## 下载

词库媒体包 **`atlas.zip`**（7,000+ 词的插画、缩略图、英美发音，约 620 MB）体积太大不进仓库，单独下载：

> 夸克网盘分享「atlas.zip」，点击链接或复制整段内容，打开「夸克APP」即可获取。
> 链接：https://pan.quark.cn/s/45bfbd0b5729?pwd=gRDv
> 提取码：gRDv
> 分享口令：`/~e9ba3brRiQ~/`

解压后把目录放到 exe 同级（`dist\WordAtlas\atlas`）即可运行；要自己构建的话放到 `web\public\atlas`。

## 目录结构

```
WordAtlas/
├─ src/                  C++ 外壳入口（WinMain）与图标
│  ├─ main.cpp           窗口尺寸、起始 URL、app.* 桥接命令
│  └─ app.ico            窗口 / 任务栏图标（由 icon.png 生成）
├─ CMakeLists.txt        编译外壳，把 frontend/ 打包成 exe 资源
├─ build.ps1             一键构建：前端 → C++ → 发布 dist\WordAtlas
├─ icon.png              图标源图（= web/public/icon-512.png）
├─ TauriCPP/             框架源码（窗口、桥接、虚拟文件系统、资源打包工具）
├─ web/                  网页源码（React 19 + TS + Vite）
├─ frontend/             前端构建产物（vite 输出，编译期嵌入 exe，可随时重建）
└─ dist/WordAtlas/       发布目录：WordAtlas.exe + atlas/
```

## 构建

前置：Visual Studio 2022（C++ 桌面负载）、CMake ≥ 3.20、Node.js ≥ 20、vcpkg 且已安装
`webview2:x64-windows-static`（`vcpkg install webview2:x64-windows-static`），环境变量 `VCPKG_ROOT` 指向 vcpkg。

```powershell
.\build.ps1              # 完整构建 + 发布到 dist\WordAtlas
```

常用参数：

| 参数 | 说明 |
| --- | --- |
| `-SkipWeb` | 跳过 npm 构建，直接用现有 `frontend/`（只改 C++ 时最省时） |
| `-SkipAtlas` | 发布时不重新同步 `atlas/`（首次之后通常已同步） |
| `-DevMode` | 不嵌入外壳：exe 读同目录的 `frontend/`，并软链到 `web/public/atlas`，改 UI 不用重编 |
| `-Clean` | 删除 `build/` 后重建 |
| `-NoPublish` | 只编译，不整理发布目录 |

产物：`dist\WordAtlas\WordAtlas.exe` + `dist\WordAtlas\atlas\`。
整个 `WordAtlas` 文件夹拷走即可离线运行（约 630 MB，绝大多数是插画和音频）。

## 先在浏览器里验收前端

打包前先用真实浏览器确认外壳和媒体都正常，省得在 exe 里盲调：

```powershell
cd web
npm run build:desktop      # 产出 ../frontend（外壳，不含 atlas 媒体）
npm run preview:local      # 起本地预览服务器 http://127.0.0.1:4173
npm run preview:check      # Playwright 自动检查：渲染、资源、Range 音频、截图
```

预览服务器把 `frontend/` 与 `web/public/atlas` 合并到同一个源下，行为和 exe 内的请求处理一致
（同样支持 `Range` 分段响应）。截图输出在 `web/.local/preview-home.png`。
只想要交互式调试就直接开浏览器访问 `http://127.0.0.1:4173/`。

## 运行机制

- 起始 URL 是 `https://tauricpp.app/index.html`（虚拟主机名，不发起真实网络请求）。
- 所有 `tauricpp.app` 请求都被 `WebResourceRequested` 拦截，由外壳自己作答：
  1. 先查 exe 资源段加载到内存的 `VirtualFS`（外壳小文件）；
  2. 再查 exe 同目录的 `atlas/`（大媒体，支持 `Range` → 206 / 416）；
  3. 无扩展名的路由回退 `index.html`；其余 404。
- 这里刻意**不使用** `SetVirtualHostNameToFolderMapping`：被映射的请求由 WebView2 直接从磁盘提供，
  不会经过 `WebResourceRequested`，内嵌外壳就永远命中不了。
- 页面里注册的桥接命令：`app.info`、`app.open_external`（仅 http(s)，交给系统浏览器）、`app.quit`。
- `target="_blank"` / `window.open` 也交给系统默认浏览器，不弹无地址栏的子窗口。
- WebView2 的配置、localStorage、IndexedDB 存放在 `%LOCALAPPDATA%\TauriCPP\WordAtlas.exe`，
  不会被系统清理临时目录时连带删掉，学习进度可长期保留。
- 桌面环境下前端不再注册 Service Worker（`main.tsx` 里按 `tauricpp.app` 判断并反注册），
  避免 SW 缓存把更新后的外壳挡住。
- 窗口默认 1280×860，可缩放居中，背景色 `#0D1120` 与页面同色（消除白屏），按 F12 打开 DevTools（仅保留了调试入口，发布不受影响）。

## 媒体文件不入库

`web/public/atlas`（插画、缩略图、英美发音，约 620 MB / 37,575 个文件）不提交到 git。
克隆后按下面任一种方式补齐：

**① 从原 word-atlas 项目拷贝**

```powershell
robocopy D:\Cplusplus\word-atlas\public\atlas D:\Cplusplus\WordAtlas\web\public\atlas /E
```

**② 直接下载 `atlas.zip`**：见开头「下载」一节（解压到 `dist\WordAtlas\atlas` 或 `web\public\atlas`）。

没有媒体时程序照样能启动，只是插画和发音缺失（构建与外壳验证不受影响）。

## 更新网页内容

网页改动都在 `web/` 里做（和原项目一致：`npm run dev` 开发）。改完重新跑 `.\build.ps1` 即可。
改了 `atlas/` 里的媒体，`.\build.ps1` 会用 `robocopy /MIR` 同步到发布目录；
只想更新媒体不发版，也可以直接把 `web\public\atlas` 拷到 `dist\WordAtlas\atlas`。
