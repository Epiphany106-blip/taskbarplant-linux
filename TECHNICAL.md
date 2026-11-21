# 技术实现说明

移植过程中真正花了时间的地方。不是 API 文档，是"为什么这么写"和"踩过哪些坑"。

## 点击穿透：`setMask` 不够

Qt 的 `QWidget::setMask()` 在 xcb 后端只设置 `ShapeBounding`（绘制裁剪），**输入区域仍是整个窗口**，全屏悬浮层会把鼠标全吃掉。

必须自己调 X11 的 `XShapeCombineRectangles(dpy, win, ShapeInput, ...)`。

而且输入区域是**逐色块精确**的：绘制时用一个全局 `QRegion` 收集每个 `fillRect` 经过变换矩阵映射后的矩形，得到的就是"真正有像素的地方"。所以花盆周围的空白不会挡鼠标，投影和碎屑也不计入。

顺带一个副作用要注意：`ShapeBounding` 一旦设置，mask 之外的区域就不再重绘，backing store 里会留下旧像素。表现为快速移动时的残影，以及"拖过去把下层窗口内容擦掉"。所以这里**只设 ShapeInput，不设 ShapeBounding**。

## 窗口承托：KWin 脚本桥

Wayland 协议禁止客户端查询其它应用的窗口位置，`_NET_CLIENT_LIST` 在 Plasma 6 下基本是空的（只有 XWayland 应用）。所以只能让合成器自己报。

做法：

1. 程序注册 D-Bus 服务 `org.taskbarplant.Geometry`，对象 `/Geometry`，方法 `update(QString json)`
   （**注意：`registerObject` 必须显式指定接口名**，两参数版会把 slot 注册到类名接口下，导致调用方 UnknownMethod）
2. 程序把一个 KWin 脚本包释放到 `~/.local/share/kwin/scripts/taskbarplant-geometry/`
3. 在 `kwinrc` 写 `Plugins/taskbarplant-geometryEnabled=true`（**键名是 `<id>Enabled`**，写成 `<id>` 无效）
4. `org.kde.KWin /Scripting` 加载并启动脚本
5. 脚本每 33ms 遍历 `workspace.windowList()`，把所有普通窗口的 `frameGeometry` 推过来

第一步那个接口名是最隐蔽的坑：程序日志显示 `service registered`、`loadScript valid=1`、`isScriptLoaded=1`，`busctl monitor` 也能看到每 33ms 一条推送，但 slot 一次都没被调用。方法调用返回 UnknownMethod，被总线静默丢弃。

踩过的坑都写在脚本注释里了，最坑的三个：

- `w.visible` 恒为 `false`，用它过滤会把列表清空
- `w.windowType`：`0` = 普通窗口，`1` = 桌面，`2` = 面板，`8` = 工具条（**不是位标志**）
- `resourceClass` 是 `WM_CLASS` 的第二个字段，首字母大写，比较要转小写

还有两个小的：脚本文件放 `/tmp` 会被 KWin 静默拒绝，必须放用户目录；`xwaylandvideobridge` 那个 64×36 的不可见辅助窗口正好横跨屏幕中线，不滤掉的话花盆每次都停在屏幕正中间。

## 承托判定：穿越检测

一帧的位移可能达到 26px（重力 2400），所以不能判"花盆最低点是否停在标题栏附近"，那会直接穿过去。

正确做法是判**穿越**：窗口顶边落在 `[上一帧最低点, 这一帧最低点]` 之间，才算这一帧从上方跨到了下方。

`prevLowestAbs` 必须初始化成花盆当前位置。留成 `-1e9` 的话第一帧会把任何顶边都当成"刚跨过去"，花盆启动时会瞬间被吸到屏幕最上方。

## 回正：重心失稳 + 弹簧

花盆倾斜时不是简单"往竖直方向插值"，而是分三段：

- **稳定区**（< 50°）：欠阻尼弹簧，`angVel -= angle * k * dt`，ζ≈0.46，过冲 19%，晃两下收敛
- **失稳区**（50°~90°）：重心越过底面边缘，回正力矩变号，加速倒向侧躺
- **躺平区**（≥ 90°）：强阻尼按住，计时结束再过硬阻尼撑起

关键是第二段。只做弹簧的话，斜到 89° 还会自己立正，因为代码里"回正"是收敛的，而现实里重心出界就该倒。少这一条，整个动作都假。

实现上只改 `angVel`，`angle` 的积分交给物理段，这样撞到窗口或地面时旋转动量才能正确传递。

调参没有靠反复编译试，而是用 Python 按 `dt=1/60` 复现同样的积分顺序，先算出过冲、周期、收敛时间，再写进 C++。物理手感这种东西，能提前算就别盲试。

## 从原版继承并修掉的 bug

原版里有两处在 Windows 上不明显、移植后暴露的问题：

```cpp
// 原版：重摔时补一点旋转，但没取符号
if (impact > 1000.0f) b.angVel += 0.4f;
```

`+= 0.4f` 恒为正。往右摔时顺水推舟，往左摔时反向顶回去——**只有一边能躺平**。改成按当前转向取符号：

```cpp
b.angVel += 0.4f * (b.angVel >= 0.0f ? 1.0f : -1.0f);
```

另一处是原版的回正是纯几何插值（`angle += -angle * 0.15f`），没有重心失稳概念，也没有加速度感。这里换成了弹簧动力学。

## 渲染

- 悬浮层只设 `ShapeInput`，不设 `ShapeBounding`（理由见第一段）
- 每帧算脏矩形（当前帧 ∪ 上一帧），先 `CompositionMode_Source` 涂透明再画
- 脏矩形必须包含投影：投影画在承托面上，花盆悬空时和花盆包围盒完全不重叠
- 输入区域每帧重设时可能丢掉 `mouseRelease`，所以 tick 里额外检查全局左键状态，松开了就强制结束拖拽

## 调试

`TBP_TRACE=1` 会每秒输出两行：

```
support: n=.. kwin=.. cx=.. prev=.. cur=.. |x,y wxh[H][L][A] => HIT/miss <top>
pot: x=.. y=.. surf=.. shadow=.. vy=.. drag=.. hit=.. deg=.. rise=.. wait=..
```

`H` 表示水平覆盖，`L` 表示在花盆下方，`A` 表示上一帧已在下方。这几个标记能一眼看出判定卡在哪一步。

`busctl --user monitor --match "type='method_call',interface='org.taskbarplant.Geometry'"` 可以直接看到 KWin 推上来的 JSON，不需要注册服务，排查脚本是否正常时最有效。
