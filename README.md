# MiniCross

Windows 外置准星。44 KB，零依赖，常驻内存 0.5 MB。

A 44 KB crosshair overlay for Windows. No runtime, no installer, no Electron.

![screenshot](docs/screenshot.png)

## 数字

实测，同一个桌面会话（Win11，2560×1600）：

| | MiniCross | crosshair-y (Electron) | CrosshairOverlay (.NET) |
|---|---|---|---|
| 下载包 | **44 KB** | 135.65 MB | 47.66 MB |
| 进程数 | **1** | **5** | — |
| 私有内存 | **1.5 MB** | **250 MB** | — |
| 工作集（面板关闭） | **0.2–0.6 MB** | **449.8 MB** | — |
| 空闲 CPU | 0 ms / 10 s | — | — |

怎么测的：

- 包体积是 `gh release download` 拿到的原始包，不是解压后。
- 内存是 `Get-Process` 的 `WorkingSet64` / `PrivateMemorySize64`，启动后等 20 秒测一次，不是峰值。
- crosshair-y 解压后运行，20 秒时按进程路径筛出 5 个 `crosshair-y.exe` 逐个加总：

```
  Id     WS          Private
27580   112.9 MB    76.6 MB
27672    90.2 MB    32.1 MB
34976    54.8 MB    13.6 MB
36812    85.6 MB    22.6 MB
39588   117.0 MB   105.1 MB
----------------------------------
合计    449.8 MB   250.0 MB
```

- CrosshairOverlay 只发了 Inno Setup 安装包（`MZP` 头），本机没有解包工具，我没装它，所以内存栏空着——不是估计，是不测。

那 5 个进程是 Electron 的常规结构：主进程、渲染进程、GPU 进程、utility 进程。一个准星叠加层，连带背着整个 Chromium。

## 功能

6 个图层，各自独立开关：

- 十字：四条臂可以单独关（`arms = 1111`，`0001` 就是只有右臂，`1101` 是 T 形）
- 斜十字：任意角度
- 中心点
- 圆环
- 四角括号
- PNG 图片：WIC 解码，支持透明通道，可旋转

每个图层都有自己的颜色、透明度、描边宽度、描边颜色和透明度。

全局：整体缩放、整体旋转、多显示器选择、按屏宽百分比定位、像素偏移。

## 用法

双击 `crosshair.exe`。它读同目录下的 `config.ini`。

改完存盘，2 秒内自动生效，不用重启。

热键：

| 键 | 作用 |
|---|---|
| F8 | 显示 / 隐藏 |
| F9 | 重载配置 |
| F10 | 切到下一个方案 |
| Ctrl+Alt+方向键 | 挪位置（只在本次运行有效，不写回文件） |
| Ctrl+Alt+Q | 退出 |

## 设置面板

按 `Ctrl+Alt+E` 打开原生设置面板。

左边下拉选图层（Cross / Diagonal / Dot / Circle / Bracket / Image / General），中间是参数控件，右边是实时预览。拖滑块，屏幕上的准星当场就变，不用保存。

点 `Save` 写回 `config.ini`（自动备份为 `config.ini.bak`），`Reset` 丢弃改动重新读文件。

面板是 Win32 原生控件，不是网页套壳：没有 WebView2、没有 Chromium、没有额外进程。

## 配置

```ini
[cross]
enabled   = 1
length    = 10
thickness = 2
gap       = 4
arms      = 1111
color     = #00FF00
alpha     = 255
outline   = 1
outline_color = #000000
outline_alpha = 200
```

颜色支持 `#RGB`、`#RRGGBB`、`#AARRGGBB`，或者写 `red` / `lime` / `cyan` 这种名字。

方案（profiles）写在文件末尾，只列要覆盖的项：

```ini
[profile:awp]
cross.length    = 6
cross.gap       = 9
circle.enabled  = 1
circle.radius   = 15
```

`general.profile` 指定当前用哪个，F10 按 `general.profiles` 里的顺序循环。

完整参数见 `config.ini` 里的注释。

## 编译

需要 gcc（MinGW-w64 / MSYS2 都行）：

```bat
build.bat
```

本质上就一条命令：

```bat
gcc -Os -s -mwindows "-Wl,--stack=65536" src\crosshair.c -o crosshair.exe ^
  -lgdi32 -luser32 -lole32 -lwindowscodecs -luuid -lshell32
```

`-Os -s` 是体积优化，`-Wl,--stack=65536` 把主线程栈从默认的 1 MB 压到 64 KB——私有内存里那部分就是这么省下来的。

## 内存是怎么压下去的

三件事：

1. **消息循环阻塞**。没有定时器在空转，窗口没消息时线程就睡着，所以 CPU 是 0。
2. **自适应 trim**。改配置或按热键之后的 30 秒内，每 2 秒调一次 `SetProcessWorkingSetSize(-1,-1)`，把不用的页换出去；30 秒后降到 60 秒一次。想永远保持在 1 MB 以下，把 `trim_idle_ms` 改成 `2000` 就行，代价是每分钟多醒 30 次（实测 CPU 仍是 0）。
3. **不常驻大对象**。没有图片时不分配位图；整个窗口只有准星包围盒那么大（比如 38×38 px），不是全屏透明层。

私有内存 1.5 MB 降不下去，那是 Windows GUI 进程的地板：线程栈、`user32`/`gdi32`/`ntdll` 的私有数据页、DIB 位图，换 C++ 也还是这个量级。

## 不做什么

- 不注入进程
- 不 `SetWindowsHookEx` 全局钩子
- 不 `OpenProcess` / `ReadProcessMemory`
- 不装驱动
- 不联网

只有一个 `WS_EX_LAYERED | WS_EX_TRANSPARENT` 的置顶窗口，跟游戏进程零交互。代码就在 `src/crosshair.c`，可以自己看。

## 已知问题

- 独占全屏模式下热键可能收不到（无边框窗口没问题）
- 没有配置界面，只能手改 ini。上一版是 .NET 写的，带网页配置台，重写成 C 的时候砍了，打算单独做一个 HTML 的
- 没做抗锯齿，小尺寸下边缘会有锯齿

## 为什么写这个

crosshair-y 免费，但装完 135 MB。Crosshair X 卖 ¥42，跑起来是 Electron 那一套。

我只是需要屏幕中间有个十字。

前身是 .NET 版，常驻 13 MB。重写成 C 之后 1.5 MB，exe 从 81 KB 变成 44 KB（其中 8 KB 是原生设置面板）。

## License

MIT。名字和 logo 别拿去商用。
