// =============================================================================
//  KWin 脚本：把当前所有普通窗口的外框几何推给 TaskbarPlant
//
//  Wayland 下客户端无法查询其它应用的窗口位置（协议层禁止），
//  只有合成器（KWin）知道。这里让 KWin 自己把几何发出来。
// =============================================================================

var SERVICE = "org.taskbarplant.Geometry";
var PATH    = "/Geometry";
var IFACE   = "org.taskbarplant.Geometry";

function push()
{
    var out = [];
    var list = workspace.windowList();

    for (var i = 0; i < list.length; ++i) {
        var w = list[i];
        try {
            if (w.deleted) continue;

            // 实测 windowType 取值：0 = 普通窗口，1 = 桌面，2 = 面板，8 = 工具条。
            if (w.windowType !== 0) continue;

            // resourceClass 是 WM_CLASS 的第二个字段，实测为 "TaskbarPlant"（首字母大写）。
            // 必须转小写比较，否则自己的全屏悬浮层会混进列表，被当成可站立的窗口。
            var rc = String(w.resourceClass).toLowerCase();
            if (rc.indexOf("taskbarplant") >= 0) continue;
            if (rc.indexOf("plasmashell") >= 0)  continue;
            if (rc.indexOf("kwin") >= 0)         continue;
            // XWayland 视频桥接的辅助窗口（64x36，正好在屏幕中间），
            // 不可见但会被列出来，不排除掉的话花盆会一直"停在正中间"。
            if (rc.indexOf("xwaylandvideobridge") >= 0) continue;

            // 注意：不要用 w.visible 过滤（实测恒为 false，会把列表清空）。
            // 最小化状态由接收端判断，这里把原始值一并推过去，方便排查。
            var min = 0;
            try { if (w.minimized) min = 1; } catch (e) {}
            var hid = 0;
            try { if (w.hidden) hid = 1; } catch (e) {}

            var g = w.frameGeometry;               // 含标题栏的外框
            if (g.width <= 0 || g.height <= 0) continue;

            // 太小的窗口站不住花盆（花盆本身 72 宽）。这类多是各种辅助/桥接窗口，
            // 它们不可见，却会在屏幕中间接住花盆。
            if (g.width < 120 || g.height < 80) continue;

            out.push([Math.round(g.x), Math.round(g.y),
                      Math.round(g.width), Math.round(g.height), min, hid]);
        } catch (e) {
            // 窗口正在销毁时访问属性会抛，忽略即可
        }
    }

    callDBus(SERVICE, PATH, IFACE, "update", JSON.stringify(out));
}

// 33ms ≈ 30fps：足够跟上窗口拖动，又不至于让 D-Bus 太忙
var t = new QTimer();
t.interval = 33;
t.timeout.connect(push);
t.start();

// 立即推一次，避免启动时空白
push();
