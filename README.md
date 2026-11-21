# 桌面电子盆栽 · Linux (Qt6)

把 Windows 上的桌面电子盆栽移植到 Linux/KDE 的版本。九种像素风植物，带真实物理，能站在任意窗口的标题栏上。

![九种植物](docs/plants.png)

## 来源与致谢

本项目的**渲染逻辑、物理参数、像素画风全部来自 Ziyi 编写的 Windows 原版 `TaskbarPlant.cpp`**（纯 Win32 + GDI+，无第三方库）。

原版的绘制完全基于 4px 网格吸附的离散色块和逐行栅格化的像素椭圆，物理也全是手写的刚体模拟。移植时这些常数和公式被逐行照搬——重力 2400、弹跳系数 0.42、阻尼弹簧刚度 110、九种植物的每一块像素坐标和色值，都是原版的。

**我做的事情是平台移植和交互适配**：把 GDI+ 的绘制翻译成 `QPainter::fillRect`，把 Win32 的分层窗口换成 X11 的 XShape，把 `EnumWindows` 换成 KWin 脚本桥。

原版在 Windows 上的设计（尤其是那些像素画的细节和手感）值得单独说一句：很好。

## 特性

- **九种植物**：仙人掌、向日葵、食人花、郁金香、蘑菇、小松树、多肉、竹子、玫瑰
- **真实物理**：重力、地面碰撞、弹跳、旋转、拖拽甩出、落地扬尘、静止自动回正
- **只摸得到花**：花盆和植物之外的区域鼠标完全穿透，下层窗口照常操作
- **站在窗口上**：花盆能停在任意窗口的标题栏顶边，窗口移动、缩放时跟着走，窗口最小化时自然掉落
- **像素风**：没有抗锯齿、没有平滑矢量，所有边缘都是 4px 阶梯
- **淡出退出**：右键菜单退出时整个场景淡出

## 操作

| 操作 | 效果 |
| --- | --- |
| 拖动花盆 | 抓起来甩，松手时按甩出速度飞出去 |
| 拖动植物 | 茎被拉弯，松手回弹 |
| 右键 | 菜单：九种植物切换 / 放置新花盆 / 移除这个花盆 / 退出程序 |

右键点在空白处时，菜单只保留「放置新花盆」和「退出程序」。

## 技术要点

### 点击穿透：`setMask` 不够

Qt 的 `QWidget::setMask()` 在 xcb 后端只设置 `ShapeBounding`（绘制裁剪），**输入区域仍是整个窗口**，全屏悬浮层会把鼠标全吃掉。

必须自己调 X11 的 `XShapeCombineRectangles(dpy, win, ShapeInput, ...)`。

而且输入区域是**逐色块精确**的：绘制时用一个全局 `QRegion` 收集每个 `fillRect` 经过变换矩阵映射后的矩形，得到的就是"真正有像素的地方"。所以花盆周围的空白不会挡鼠标，投影和碎屑也不计入。

### 窗口承托：KWin 脚本桥

Wayland 协议禁止客户端查询其它应用的窗口位置，`_NET_CLIENT_LIST` 在 Plasma 6 下基本是空的（只有 XWayland 应用）。所以只能让合成器自己报。

做法：

1. 程序注册 D-Bus 服务 `org.taskbarplant.Geometry`，对象 `/Geometry`，方法 `update(QString json)`
   （**注意：`registerObject` 必须显式指定接口名**，两参数版会把 slot 注册到类名接口下，导致调用方 UnknownMethod）
2. 程序把一个 KWin 脚本包释放到 `~/.local/share/kwin/scripts/taskbarplant-geometry/`
3. 在 `kwinrc` 写 `Plugins/taskbarplant-geometryEnabled=true`（**键名是 `<id>Enabled`**，写成 `<id>` 无效）
4. `org.kde.KWin /Scripting` 加载并启动脚本
5. 脚本每 33ms 遍历 `workspace.windowList()`，把所有普通窗口的 `frameGeometry` 推过来

踩过的坑都写在脚本注释里了，最坑的三个：

- `w.visible` 恒为 `false`，用它过滤会把列表清空
- `w.windowType`：`0` = 普通窗口，`1` = 桌面，`2` = 面板，`8` = 工具条（**不是位标志**）
- `resourceClass` 是 `WM_CLASS` 的第二个字段，首字母大写，比较要转小写

### 承托判定：穿越检测

一帧的位移可能达到 26px（重力 2400），所以不能判"花盆最低点是否停在标题栏附近"，那会直接穿过去。

正确做法是判**穿越**：窗口顶边落在 `[上一帧最低点, 这一帧最低点]` 之间，才算这一帧从上方跨到了下方。

### 渲染

- 悬浮层不设 `ShapeBounding`（会裁掉重绘、留下残影）
- 每帧算脏矩形（当前帧 ∪ 上一帧），先 `CompositionMode_Source` 涂透明再画
- 脏矩形必须包含投影：投影画在承托面上，花盆悬空时和花盆包围盒完全不重叠

## 构建

依赖：Qt6 (Widgets/Gui/DBus)、libX11、libXext、CMake ≥ 3.20、C++17

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

openSUSE：

```bash
sudo zypper install gcc-c++ cmake qt6-base-devel libX11-devel libXext-devel
```

Debian/Ubuntu：

```bash
sudo apt install build-essential cmake qt6-base-dev libx11-dev libxext-dev
```

## 运行

```bash
./build/taskbarplant
```

单文件可执行，无需安装、无需启动脚本。首次运行会自己完成 KWin 脚本的安装与加载。

调试时加 `TBP_TRACE=1` 会把判定过程和花盆状态打到终端：

```bash
TBP_TRACE=1 ./build/taskbarplant
```

## 命令行参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `-u`, `--upright-delay <sec>` | `3` | 摔倒后躺多久再自己立起来（秒）。`0` 表示立刻回正 |
| `--scale <factor>` | `1.0` | 整体缩放。`1.0` 是原版比例，放大后像素颗粒会变粗 |
| `-h`, `--help` | — | 显示帮助 |
| `-v`, `--version` | — | 显示版本 |

```bash
./build/taskbarplant --upright-delay 8     # 摔倒了就多躺一会儿
./build/taskbarplant --scale 1.5           # 放大 1.5 倍
```

「摔倒」指的是倾斜超过约 6° 且完全静止。只是轻微歪一点的话仍然立刻回正，不会傻站着等。

## 环境变量

| 变量 | 作用 |
| --- | --- |
| `TBP_TRACE=1` | 输出承托判定与花盆状态日志 |
| `TBP_SCALE=1.5` | 同 `--scale`（命令行优先） |
| `TBP_PLATFORM=wayland` | 强制原生 Wayland 后端（会失去点击穿透和窗口承托） |
| `TBP_POT_X` / `TBP_POT_Y` | 指定初始花盆位置，用于调试 |

## 已知限制

- **只在 KDE Plasma 下能站在窗口上**。靠 KWin 脚本桥实现，其它桌面环境会退回 X11 枚举，只能看到 XWayland 窗口，再不行就只停屏幕底边。
- 点击穿透依赖 X11 的 XShape，所以程序强制走 `xcb` 后端（KDE 下即 XWayland）。原生 Wayland 下无法实现。
- 窗口被其它窗口完全遮住时，花盆仍会停在被遮住那个窗口的标题栏上。
- 花盆状态只存在内存里，重启后回到一个向日葵。
- 原版的「全屏应用时自动隐藏」没有移植，Linux 下没有可靠的全屏检测接口。

## 开发说明

三方协作，各司其职：

- **Ziyi** — Windows 原版 `TaskbarPlant.cpp` 的作者。像素画、物理手感、交互设计的源头都在这里。已授权发布本移植版。
- **Zirui Liu** — 提需求、定方向、做测试反馈。点击穿透、跟随窗口、残影、停在屏幕中间这些问题，都是他一个个测出来的。
- **DeepSeek V4.1 Flash 1017**（运行在 WorkBuddy 中）— 负责移植实现与调试。平台层的坑（XShape 输入区域、KWin 脚本桥、D-Bus 接口名）大部分是靠它用 `busctl` / `xprop` 排查跑通的。

原版是纯手写 Win32 + GDI+，没有任何第三方库。移植版保留了同样的克制：只用 Qt6 做窗口和绘制，没有任何运行时依赖。

## 许可

MIT，见 [LICENSE](LICENSE)。

这意味着你可以自由使用、修改、分发、商用，唯一要求是保留版权与许可声明。原版 `TaskbarPlant.cpp` 由 Ziyi 编写并授权，本仓库是基于它改编的 Linux 移植版本。
