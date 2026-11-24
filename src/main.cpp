// =============================================================================
//  TaskbarPlant —— 桌面电子盆栽 / Linux (Qt6) 移植版
//
//  原版：https://github.com/Ziyi-Zhao-star/TaskbarPlant
//        由 Ziyi Zhao 发起，代码由 DeepSeek V4 Flash 1017 与 GLM 5.3 Flash 编写。
//  本移植版：由 Zirui Liu 发起，代码由 DeepSeek V4 Flash 1017 与 Space Bunny 编写。
//
//  渲染逻辑 1:1 照搬 TaskbarPlant.cpp（Win32 + GDI+）：
//    所有绘制都是 4px 网格吸附的离散色块 + 逐行栅格化的像素椭圆，
//    这里全部翻译成 QPainter::fillRect（关闭抗锯齿），视觉上与原版一致。
//
//  物理也照搬原版常数：重力 2400、弹跳 0.42、旋转包围盒碰撞、
//  拖拽甩动、阻尼弹簧摆动、落地扬尘碎屑、静止自动回正。
//
//  与原版的差异（平台相关）：
//    · "停在别的窗口标题栏上"：Wayland 协议禁止客户端查询其它应用的窗口位置，
//      所以走 KWin 脚本桥 —— 程序注册 D-Bus 服务，KWin 脚本每 33ms 把所有普通窗口的
//      frameGeometry 推过来（src/kwin_bridge.js）。这样连 Wayland 原生窗口也能站上去。
//      非 KDE 环境回退到 X11 枚举（只能看到 XWayland 窗口），再不行就停在屏幕底边。
//    · 点击穿透：Qt 的 setMask 只裁剪绘制，不设输入区域，所以自己调 XShape 设
//      ShapeInput，且输入区域是"逐色块精确"的 —— 只有花和盆本身接收鼠标。
//    · 全屏应用检测在 Linux 下没有可靠接口，暂未实现（原版会 SW_HIDE 自己）。
// =============================================================================

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QToolButton>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QWidgetAction>
#include <QFrame>
#include <QMenu>
#include <QFileDialog>
#include <QAction>
#include <QScreen>
#include <QCursor>
#include <QSettings>
#include <QDir>
#include <QTextStream>
#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QStyleHints>
#include <QActionGroup>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QPainterPath>
#include <QStyle>
#include <QRegion>
#include <QImage>
#include <QDateTime>

// 版本号由构建期从 git tag 判定（cmake/gen_version.cmake 生成这个头文件）。
// 落在正式版 / 预发布版 tag 上就用 tag 的版本，否则是 "<基线>-canary"。
#if defined(__has_include)
#  if __has_include("tbp_version.h")
#    include "tbp_version.h"
#  endif
#endif
#ifndef TBP_VERSION
#  define TBP_VERSION "0.0.0-canary"
#endif

// 注意：这些必须在 <X11/Xlib.h> 之前 —— Xlib 会 #define Bool int、None 0L 之类，
// 把 QJsonValue::Type 的枚举名替换掉，导致一堆莫名其妙的语法错误。
#if defined(TBP_HAVE_DBUS)
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#endif
#include <QApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QFileInfo>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

// -----------------------------------------------------------------------------
//  常量
//
//  原版的 PIX = 4.0（一个"像素块"4 个屏幕像素），是在 1366x768 那种屏幕上调
//  出来的比例。放到 1080p / 4K 上就明显偏小，所以在运行时按屏幕高度放大：
//    scale = round(screenHeight / 900 * 10) / 10，限制在 [0.8, 2.5]
//  用户也可以用 TBP_SCALE 环境变量强制指定。
//  PIX / POT_W / POT_H / BODY_HALF_W 都是 g_scale 的倍数，其余逻辑一律不变。
// -----------------------------------------------------------------------------
static const int   ANIM_INTERVAL = 16;        // ms, ~60FPS
static const float DT            = 0.016f;
static const float GRAVITY       = 2400.0f;
static const float RESTITUTION   = 0.42f;
static const float PI_F          = 3.14159265f;

static float g_scale = 1.0f;                  // 当前绘制用的缩放（= g_baseScale * 该盆 scale）
static float g_baseScale = 1.0f;              // 屏幕基准缩放（start() 里定，不含单盆缩放）
static float g_uprightDelay = 3.0f;           // 摔倒后躺多久再立起来（秒），--upright-delay 可改
static float g_drySeconds = 1800.0f;          // 泥土从湿透到干透的秒数，默认 30 分钟
static bool  g_dryFromCli = false;            // --dry-seconds 是否显式给过（优先于 ini）
static float g_wiltAmount = 0.0f;             // 当前绘制用的枯萎程度 0..1（绘制期临时值）
static bool  g_wiltEnabled = true;            // false = 关闭枯萎，叶子永远绿

// 单盆缩放
static const float POT_SCALE_MIN  = 0.65f;
static const float POT_SCALE_MAX  = 1.45f;
static const float POT_SCALE_STEP = 0.10f;

// 浇水
static const float WATER_DURATION = 2.2f;     // 浇水动画时长（秒）
static const float DRY_THRESHOLD  = 0.40f;    // 水分低于它开始发蔫

static inline float PIXf()         { return 4.0f  * g_scale; }
static inline float POT_Wf()       { return 72.0f * g_scale; }
static inline float POT_Hf()       { return 48.0f * g_scale; }
static inline float BODY_HALF_Wf() { return 32.0f * g_scale; }

// 兼容旧写法：这几个名字在绘制代码里出现频率极高，保留为宏而不是函数调用，
// 避免逐处改动引入笔误。
#define PIX         PIXf()
#define POT_W       POT_Wf()
#define POT_H       POT_Hf()
#define BODY_HALF_W BODY_HALF_Wf()

enum PlantType {
    PLANT_CACTUS = 0,
    PLANT_SUNFLOWER,
    PLANT_FLYTRAP,
    PLANT_TULIP,
    PLANT_MUSHROOM,
    PLANT_PINE,
    PLANT_SUCCULENT,
    PLANT_BAMBOO,
    PLANT_ROSE,
    PLANT_LAVENDER,
    PLANT_CLOVER,
    PLANT_CHILI,
    PLANT_DANDELION,
    PLANT_DAISY,
    PLANT_ALOE,
    PLANT_MORNINGGLORY,
    PLANT_CHERRY,
    PLANT_COUNT
};

enum MenuCmd {
    MENU_CMD_WATER = PLANT_COUNT,   // 浇水
    MENU_CMD_EDIT,                  // 编辑模式
    MENU_CMD_SCALE_UP,              // 花盆调大
    MENU_CMD_SCALE_DOWN,            // 花盆调小
    MENU_CMD_SPAWN,                 // 放置新花盆
    MENU_CMD_REMOVE,                // 移除这个花盆
    MENU_CMD_EXPORT,                // 导出植物图片
    MENU_CMD_QUIT,                  // 退出程序
    MENU_CMD_VERSION,               // 版本号（不关菜单，连点 7 次开隐藏调试模式）
    MENU_ITEM_COUNT
};

// 菜单底部的版本号（上游 v3.8.2：连点 7 次开隐藏调试模式）
//
// 版本号本身带 "v" 前缀，和上游一致。构建期由 cmake/gen_version.cmake 从 git tag 判定：
// 落在正式版 / 预发布版 tag 上就是那个版本号，否则是 "<基线>-canary"。
static inline QString VersionText() { return QStringLiteral("v") + QString::fromLatin1(TBP_VERSION); }

// ---- 版本号连点计数（隐藏调试模式） ----------------------------------------
//
// 静默计数：点第 1~6 次没有任何提示，也不关菜单；点满 7 次才开出调试面板。
// 两次点击间隔超过 3 秒就重新从头数（上游同样的规则）。
static const int    kVersionClicksToDebug = 7;
static const qint64 kVersionClickWindowMs  = 3000;
static int    g_versionClicks    = 0;
static qint64 g_versionLastClick = 0;

// 隐藏调试模式的动作
enum DebugAction { DBG_DRY = 0, DBG_RAIN, DBG_SNOW, DBG_OFF, DBG_COUNT };

static const char* kPlantNames[PLANT_COUNT] = {
    "仙人掌", "向日葵", "食人花", "郁金香", "蘑菇",
    "小松树", "多肉", "竹子", "玫瑰",
    "薰衣草", "三叶草", "辣椒", "蒲公英",
    "雏菊", "芦荟", "牵牛花", "樱花"
};

static const float kPlantH[PLANT_COUNT] = {
    112.0f, 150.0f, 140.0f, 140.0f, 100.0f,
    148.0f,  64.0f, 158.0f, 132.0f,
    152.0f,  88.0f, 128.0f, 148.0f,
    132.0f, 104.0f, 168.0f, 132.0f
};

// -----------------------------------------------------------------------------
//  基础工具
// -----------------------------------------------------------------------------
static inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline float SnapPx(float v) { return PIX * floorf(v / PIX + 0.5f); }
static inline QColor C(int r, int g, int b, int a = 255) { return QColor(r, g, b, a); }

// GDI+ FillRectF 的语义：负宽高时归一化，零宽高时不画
//
// 同时承担第二个职责：当 g_capRegion 非空时，把每个色块在"设备坐标"下的矩形
// 累积进去 —— 这样得到的区域就是"真正有像素的地方"，也就是鼠标应该能摸到的范围。
// 比按包围盒估算精确得多（花盆周围的空白不会挡鼠标）。
static QRegion* g_capRegion = nullptr;
static bool     g_capOnly   = false;

static void FillRectF(QPainter& g, const QColor& col, float x, float y, float w, float h)
{
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    if (w <= 0.0f || h <= 0.0f) return;

    QRectF rf(x, y, w, h);
    if (g_capRegion) {
        // transform() 含 translate / rotate，映射出来的就是屏幕像素位置
        *g_capRegion += g.transform().mapRect(rf).toAlignedRect();
    }
    if (!g_capOnly) {
        // 枯萎：把绿色系的色块往枯黄方向插值。只动绿，花和花盆不受影响，
        // 这样十几种植物的绘制函数一行都不用改。
        QColor c = col;
        if (g_wiltAmount > 0.001f
            && col.green() > col.red() && col.green() > col.blue()) {
            const QColor dry(158, 124, 66);
            c = QColor((int)(col.red()   + (dry.red()   - col.red())   * g_wiltAmount),
                       (int)(col.green() + (dry.green() - col.green()) * g_wiltAmount),
                       (int)(col.blue()  + (dry.blue()  - col.blue())  * g_wiltAmount),
                       col.alpha());
        }
        g.fillRect(rf, c);
    }
}

// 像素椭圆：按 4px 行逐行栅格化，每行宽度取整到像素格，边缘天然是阶梯
static void FillPixelEllipse(QPainter& g, const QColor& col,
                             float cx, float cy, float rx, float ry)
{
    float top  = SnapPx(cy - ry);
    int   rows = (int)floorf((2.0f * ry) / PIX + 0.5f);
    for (int i = 0; i < rows; ++i) {
        float y  = top + (float)i * PIX;
        float dy = y + PIX * 0.5f - cy;
        float k  = 1.0f - (dy * dy) / (ry * ry);
        if (k <= 0.0f) continue;
        float hw = SnapPx(rx * sqrtf(k));
        if (hw < PIX) hw = PIX;
        FillRectF(g, col, SnapPx(cx - hw), y, hw * 2.0f, PIX);
    }
}

// 像素叶片：从 (x,y) 往 dir 方向斜着伸出的阶梯叶，逐格收窄
static void DrawPixelLeaf(QPainter& g, float x, float y, int dir,
                          float len, float span, float halfW,
                          const QColor& col, const QColor* vein,
                          const QColor* shade = nullptr,
                          float tipLean = 0.0f)
{
    int rows = (int)floorf(len / PIX + 0.5f);
    if (rows < 3) rows = 3;
    const float y0 = SnapPx(y);

    float prevCx = SnapPx(x);   // 上一行的横向位置，用来保证阶梯连续
    for (int i = 0; i < rows; ++i) {
        const float k = (float)(i + 1) / (float)rows;

        // 宽度：前 70% 缓慢收窄（基部饱满），后 30% 迅速收成尖
        float hw;
        if (k < 0.7f) hw = halfW * (1.0f - k * 0.42f);
        else          hw = halfW * (1.0f - 0.7f * 0.42f) * (1.0f - (k - 0.7f) / 0.3f);
        hw = SnapPx(hw);
        if (hw < PIX * 0.5f) hw = PIX * 0.5f;

        // 横向：匀速斜伸。上游原文写的是分段斜率（前 70% 与后 30% 各一段），
        // 但两段的系数相乘后都化简成 k，实际渲染就是匀速，这里如实照搬。
        // tipLean 按 k² 加权：叶根不动，越靠叶尖摆得越多。
        // 直接加在 x 上（上游的写法）等于整片叶子横向瞬移，看起来是抽搐而不是颤动。
        float cx = SnapPx(x + (float)dir * (span * k + tipLean * k * k));

        // 每行横向最多前移一格。span 不是 4 的倍数时，最后一行 snap 会跳 8px，
        // 而叶尖只有一格宽，于是叶片在末端断开 2px —— 看起来像"叶尖掉了一块"。
        // 夹住之后阶梯一定连续，代价是叶子略短一点点。
        if (i > 0) {
            if (cx - prevCx >  PIX) cx = prevCx + PIX;
            if (cx - prevCx < -PIX) cx = prevCx - PIX;
        }
        prevCx = cx;

        // 纵向：前 70% 每行上爬一格，后 30% 每行只爬半格，叶尖放平下垂
        // （用 rows*PIX 而不是 len：两者在 g_scale=1 时相等，缩放时前者不会错位）
        const float seg  = 0.7f * (float)rows * PIX;
        const float rise = (k <= 0.7f)
                         ? (float)(i + 1) * PIX
                         : seg + ((float)(i + 1) - 0.7f * (float)rows) * PIX * 0.5f;
        const float yy = y0 - rise;

        FillRectF(g, col, cx - hw, yy, hw * 2.0f, PIX);
        if (shade) FillRectF(g, *shade, cx - hw, yy + PIX, hw * 2.0f, PIX * 0.5f);
        if (vein && i * 2 < rows)
            FillRectF(g, *vein, cx - PIX * 0.5f, yy, PIX, PIX);
    }
}

// =============================================================================
//  一、像素风绘制（花盆局部坐标系：原点 = 花盆几何中心）
// =============================================================================

static void DrawPotLocal(QPainter& g)
{
    const float hw = POT_W * 0.5f;
    const float hh = POT_H * 0.5f;

    const QColor body      = C(174, 100, 58);
    const QColor bodyDark  = C(122, 64, 32);
    const QColor bodyLight = C(206, 132, 84);
    const QColor rim       = C(208, 126, 70);
    const QColor rimDark   = C(144, 76, 38);
    const QColor rimLight  = C(234, 166, 110);
    const QColor soil      = C(56, 36, 22);
    const QColor soilLight = C(98, 70, 46);

    // 盆身：9 行像素块，逐层内收
    const int rows = 9;
    const int insets[9] = { 0, 0, 0, 1, 1, 1, 2, 2, 3 };
    for (int i = 0; i < rows; ++i) {
        float y  = -hh + 12.0f + (float)i * PIX;
        float x0 = -BODY_HALF_W + (float)insets[i] * PIX;
        float x1 =  BODY_HALF_W - (float)insets[i] * PIX;
        FillRectF(g, body,      x0, y, x1 - x0, PIX);
        FillRectF(g, bodyDark,  x1 - PIX, y, PIX, PIX);
        FillRectF(g, bodyLight, x0, y, PIX, PIX);
    }

    // 盆身砖缝
    for (int i = 1; i < rows; i += 2) {
        float y = -hh + 12.0f + (float)i * PIX;
        int inset = insets[i];
        float x0 = -BODY_HALF_W + (float)inset * PIX;
        FillRectF(g, bodyDark, x0 + 3 * PIX, y, PIX, PIX);
        FillRectF(g, bodyDark, x0 + 8 * PIX, y + PIX, PIX, PIX);
    }

    // 盆沿：3 行像素块
    FillRectF(g, rim,      -hw, -hh,           POT_W, 2 * PIX);
    FillRectF(g, rimDark,  -hw, -hh + 2 * PIX, POT_W, PIX);
    FillRectF(g, rimLight, -hw, -hh,           PIX, 2 * PIX);

    // 盆沿锯齿缺口
    FillRectF(g, rimDark,  -hw + PIX,     -hh + PIX, PIX, PIX);
    FillRectF(g, rimDark,   hw - 2 * PIX, -hh + PIX, PIX, PIX);
    FillRectF(g, rimLight,  hw - 3 * PIX, -hh,       PIX, PIX);

    // 泥土
    FillRectF(g, soil,      -hw + 2 * PIX, -hh + PIX, 14 * PIX, PIX);
    FillRectF(g, soilLight, -hw + 3 * PIX, -hh + PIX, 2 * PIX, PIX);
    FillRectF(g, soilLight, -hw + 8 * PIX, -hh + 2 * PIX, PIX, 1.0f);
    FillRectF(g, soilLight,  hw - 5 * PIX, -hh + PIX, PIX, PIX);
}

// 像素茎：逐格往上堆，越靠近顶端摆得越多
struct PtF { float x, y; };

static PtF DrawBentStem(QPainter& g, float baseX, float baseY, float height,
                        float sway, const QColor& col, float width)
{
    float w  = SnapPx(width);
    if (w < PIX) w = PIX;
    int   rows = (int)floorf(height / PIX + 0.5f);
    float y0   = SnapPx(baseY);
    float bx   = SnapPx(baseX);
    float tipX = bx;
    (void)bx;
    for (int i = 0; i < rows; ++i) {
        float k = (float)(i + 1) / (float)rows;
        tipX = SnapPx(baseX + sway * 46.0f * g_scale * powf(k, 1.9f));
        FillRectF(g, col, tipX - w * 0.5f, y0 - (float)(i + 1) * PIX, w, PIX);
    }
    return PtF{ tipX, y0 - (float)rows * PIX };
}

static void DrawLeaf(QPainter& g, float soilY, float height, float p, float sway,
                     int dir, float t)
{
    const QColor leaf  = C(100, 178, 72);
    const QColor vein  = C(64, 126, 48);

    const float x = sway * 46.0f * g_scale * powf(p, 1.9f);
    const float y = soilY - height * p;
    // 叶片自身的颤动，越靠叶尖幅度越大（p 是叶位在茎上的比例）。
    // 吸附到 4px 网格后表现为整格的轻微跳动 —— 像素画式的"呼吸"。
    // 幅度收在 1 格以内：再大叶尖就会从叶片主体上断开（末行宽度只有 1 格，
    // 偏移超过 1 格就没有重叠了）。
    const float flutter = sinf(t * 1.9f + height * 0.11f + (float)dir * 2.0f)
                        * PIX * 1.0f * p;
    // 不用 shade：上游是把半格高（2px）的暗色铺在每一行下方，渲染出来是
    // 一条条 2px 横纹，整片叶子像排骨，而且破坏了 4px 网格。
    // 想要暗边的话应该是整片叶子下缘的阶梯轮廓，不是逐行描边。
    DrawPixelLeaf(g, x, y + PIX * 2.0f, dir, 26.0f, 30.0f, 9.0f,
                  leaf, &vein, nullptr, flutter);
}

static void DrawSunflowerHead(QPainter& g, float cx, float cy, float sway, float t)
{
    const QColor petalA = C(250, 198, 46);
    const QColor petalB = C(232, 158, 26);
    const QColor core   = C(120, 72, 30);
    const QColor coreD  = C(88, 50, 20);
    const QColor seed   = C(58, 34, 14);

    g.save();
    g.translate(SnapPx(cx), SnapPx(cy));
    g.rotate(sway * 12.0f + sinf(t * 1.3f) * 3.0f);

    const int n = 12;
    for (int i = 0; i < n; ++i) {
        float a  = (float)i * (2.0f * PI_F) / (float)n;
        float px = SnapPx(cosf(a) * 24.0f);
        float py = SnapPx(sinf(a) * 24.0f);
        const QColor& br = (i & 1) ? petalB : petalA;
        FillRectF(g, br, px - 6.0f, py - 6.0f, 12.0f, 12.0f);
    }
    FillPixelEllipse(g, core,  0.0f, 0.0f, 14.0f, 14.0f);
    FillPixelEllipse(g, coreD, 0.0f, 8.0f, 10.0f, 4.0f);

    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j)
            FillRectF(g, seed, (float)i * 8.0f - 2.0f, (float)j * 8.0f - 2.0f, 4.0f, 4.0f);

    g.restore();
}

static void DrawSunflower(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem = C(76, 142, 58);
    const float h = 108.0f;
    PtF tip = DrawBentStem(g, 0.0f, soilY, h, sway, stem, 7.0f);
    // 叶位取黄金分割点 1/φ² (0.382) 与 1/φ (0.618)，上游 v3.8.2 的美学微调
    DrawLeaf(g, soilY, h, 0.382f, sway, -1, t);
    DrawLeaf(g, soilY, h, 0.618f, sway, +1, t);
    DrawSunflowerHead(g, tip.x, tip.y - 10.0f, sway, t);
}

static void DrawCactus(QPainter& g, float soilY, float sway)
{
    const float h = 92.0f;
    const QColor body   = C(82, 158, 82);
    const QColor dark   = C(54, 116, 56);
    const QColor light  = C(122, 200, 116);
    const QColor spike  = C(226, 244, 198);
    const QColor petal  = C(242, 106, 140);
    const QColor petal2 = C(252, 176, 200);

    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 14.0f);

    FillRectF(g, body,  -12.0f, -h, 24.0f, h + PIX);
    FillRectF(g, dark,    4.0f, -h,  8.0f, h + PIX);
    FillRectF(g, light, -12.0f, -h,  4.0f, h + PIX);
    FillPixelEllipse(g, body,   0.0f, -h,       12.0f, 8.0f);
    FillPixelEllipse(g, light, -8.0f, -h - 4.0f, 4.0f, 4.0f);

    // 左臂
    FillRectF(g, body,  -28.0f, -64.0f, 16.0f, 12.0f);
    FillRectF(g, body,  -28.0f, -88.0f, 12.0f, 28.0f);
    FillRectF(g, dark,  -20.0f, -88.0f,  4.0f, 28.0f);
    FillPixelEllipse(g, body, -22.0f, -88.0f, 6.0f, 6.0f);

    // 右臂
    FillRectF(g, body,   12.0f, -48.0f, 16.0f, 12.0f);
    FillRectF(g, body,   16.0f, -72.0f, 12.0f, 28.0f);
    FillRectF(g, dark,   24.0f, -72.0f,  4.0f, 28.0f);
    FillPixelEllipse(g, body, 22.0f, -72.0f, 6.0f, 6.0f);

    // 刺
    for (int i = 0; i < 9; ++i) {
        float y = SnapPx(-h + 8.0f + (float)i * 8.0f);
        FillRectF(g, spike, -16.0f, y,        4.0f, 4.0f);
        FillRectF(g, spike,  12.0f, y + 4.0f, 4.0f, 4.0f);
    }

    // 顶端小花
    FillRectF(g, petal,  -8.0f, -h - 20.0f, 16.0f, 8.0f);
    FillRectF(g, petal, -12.0f, -h - 16.0f, 24.0f, 8.0f);
    FillRectF(g, petal2, -4.0f, -h - 16.0f,  8.0f, 4.0f);

    g.restore();
}

static void DrawFlytrap(QPainter& g, float soilY, float sway, float t, float extraOpen)
{
    const QColor stem    = C(74, 138, 58);
    const QColor headFil = C(112, 178, 68);
    const QColor headDrk = C(76, 134, 48);
    const QColor mouth   = C(150, 32, 52);
    const QColor teeth   = C(240, 244, 214);

    const float h = 96.0f;
    PtF tip = DrawBentStem(g, 0.0f, soilY, h, sway, stem, 8.0f);

    float open = 0.30f + 0.16f * sinf(t * 2.2f) + extraOpen;
    open = Clampf(open, 0.05f, 1.0f);

    g.save();
    g.translate(SnapPx(tip.x), SnapPx(tip.y) - PIX);
    g.rotate(sway * 24.0f);

    FillPixelEllipse(g, headFil, 0.0f, 8.0f, 22.0f, 11.0f);
    FillRectF(g, headDrk, -16.0f, 14.0f, 32.0f, PIX);
    FillPixelEllipse(g, mouth, 0.0f, 2.0f, 12.0f, 6.0f);

    for (int i = 0; i < 5; ++i)
        FillRectF(g, teeth, -16.0f + (float)i * 8.0f, 0.0f, 4.0f, 4.0f);

    // 上颚
    g.save();
    g.rotate(-open * 34.0f);
    FillPixelEllipse(g, headFil, 0.0f, -8.0f, 22.0f, 11.0f);
    FillRectF(g, headDrk, -16.0f, -14.0f, 32.0f, PIX);
    for (int i = 0; i < 5; ++i)
        FillRectF(g, teeth, -16.0f + (float)i * 8.0f, -4.0f, 4.0f, 4.0f);
    g.restore();

    g.restore();
}

static void DrawTulip(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem   = C(84, 150, 62);
    const QColor leafA  = C(102, 176, 74);
    const QColor leafB  = C(72, 138, 54);
    const QColor petalA = C(226, 78, 106);
    const QColor petalB = C(196, 52, 84);
    const QColor petalC = C(250, 148, 168);

    const float h = 88.0f;
    PtF tip = DrawBentStem(g, 0.0f, soilY, h, sway, stem, 8.0f);

    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 10.0f);
    DrawPixelLeaf(g, 0.0f,  0.0f, -1, 40.0f, 24.0f, 6.0f, leafA, nullptr);
    DrawPixelLeaf(g, 0.0f, -8.0f, +1, 36.0f, 24.0f, 6.0f, leafB, nullptr);
    g.restore();

    g.save();
    g.translate(SnapPx(tip.x), SnapPx(tip.y));
    g.rotate(sway * 16.0f + sinf(t * 1.7f) * 2.0f);

    FillRectF(g, petalA, -16.0f, -28.0f, 32.0f, 20.0f);
    FillRectF(g, petalB, -16.0f, -28.0f,  4.0f, 20.0f);
    FillRectF(g, petalB,  12.0f, -28.0f,  4.0f, 20.0f);
    FillRectF(g, petalA, -16.0f, -36.0f, 12.0f,  8.0f);
    FillRectF(g, petalA,   4.0f, -36.0f, 12.0f,  8.0f);
    FillRectF(g, petalB,  -4.0f, -32.0f,  8.0f,  4.0f);
    FillRectF(g, petalC,  -4.0f, -24.0f,  8.0f, 12.0f);
    FillRectF(g, petalA, -12.0f,  -8.0f, 24.0f,  4.0f);

    g.restore();
}

static void DrawMushroom(QPainter& g, float soilY, float sway, float t)
{
    const QColor stalk   = C(238, 226, 202);
    const QColor stalkSh = C(206, 190, 164);
    const QColor capA    = C(212, 62, 62);
    const QColor capB    = C(168, 38, 44);
    const QColor spot    = C(250, 240, 222);
    const QColor gill    = C(196, 170, 140);

    const float h = 62.0f;

    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 9.0f + sinf(t * 1.1f) * 1.5f);

    FillRectF(g, stalk,   -8.0f, -h + 12.0f, 16.0f, h - 12.0f);
    FillRectF(g, stalkSh,  4.0f, -h + 12.0f,  4.0f, h - 12.0f);
    FillRectF(g, gill,   -20.0f, -h + 12.0f, 40.0f, 4.0f);

    FillRectF(g, capB, -24.0f, -h + 8.0f, 48.0f, 4.0f);
    FillRectF(g, capA, -24.0f, -h + 4.0f, 48.0f, 4.0f);
    FillRectF(g, capA, -20.0f, -h,        40.0f, 4.0f);
    FillRectF(g, capA, -16.0f, -h - 4.0f, 32.0f, 4.0f);
    FillRectF(g, capA, -12.0f, -h - 8.0f, 24.0f, 4.0f);
    FillRectF(g, capA,  -8.0f, -h - 12.0f, 16.0f, 4.0f);
    FillRectF(g, capA,  -4.0f, -h - 16.0f,  8.0f, 4.0f);

    FillRectF(g, spot, -16.0f, -h - 4.0f, 8.0f, 4.0f);
    FillRectF(g, spot, -12.0f, -h - 8.0f, 4.0f, 4.0f);
    FillRectF(g, spot,   8.0f, -h + 4.0f, 4.0f, 4.0f);

    g.restore();
}

static void DrawPine(QPainter& g, float soilY, float sway, float t)
{
    const QColor trunk    = C(122, 82, 46);
    const QColor trunkDk  = C(92, 58, 30);
    const QColor tierA    = C(52, 124, 70);
    const QColor tierB    = C(38, 98, 56);
    const QColor tierC    = C(86, 166, 98);
    const QColor tierSh   = C(28, 76, 46);

    g.save();
    g.translate(0.0f, soilY);
    g.rotate(sway * 5.0f + sinf(t * 0.9f) * 1.6f);

    FillRectF(g, trunk,   -6.0f, -38.0f, 12.0f, 38.0f);
    FillRectF(g, trunkDk,  2.0f, -38.0f,  4.0f, 38.0f);

    struct Tier { float baseY, halfW, height; };
    const Tier tiers[3] = {
        { -34.0f, 34.0f, 36.0f },
        { -66.0f, 26.0f, 34.0f },
        { -96.0f, 19.0f, 32.0f }
    };
    for (int i = 0; i < 3; ++i) {
        const Tier& tr = tiers[i];
        int rows = (int)(tr.height / PIX);
        float dx = sway * (5.0f + (float)i * 4.0f);
        for (int s = 0; s < rows; ++s) {
            float k  = (float)s / (float)rows;
            float hw = tr.halfW * (1.0f - k * 0.84f);
            hw = PIX * floorf(hw / PIX + 0.5f);
            if (hw < PIX) hw = PIX;
            float y = tr.baseY - (float)s * PIX;
            FillRectF(g, (i & 1) ? tierB : tierA, dx - hw, y - PIX, hw * 2.0f, PIX);
            FillRectF(g, tierC, dx - hw, y - PIX, PIX, PIX);
        }
        FillRectF(g, tierSh, dx - tr.halfW, tr.baseY - PIX, tr.halfW * 2.0f, PIX);
    }

    float tipDx = sway * 26.0f;
    FillRectF(g, tierA, tipDx - 10.0f, -132.0f, 20.0f, PIX);
    FillRectF(g, tierA, tipDx -  6.0f, -136.0f, 12.0f, PIX);
    FillRectF(g, tierB, tipDx -  3.0f, -140.0f,  6.0f, PIX);

    g.restore();
}

// 多肉：极坐标分区逐格判定，外轮廓按叶片尖端起伏
static void DrawSucculent(QPainter& g, float soilY, float sway, float t)
{
    const QColor edge     = C(52, 96, 66);
    const QColor outLit   = C(176, 216, 158);
    const QColor outShade = C(88, 142, 98);
    const QColor inLit    = C(210, 240, 184);
    const QColor inShade  = C(128, 184, 122);
    const QColor core     = C(234, 250, 210);
    const QColor coreHi   = C(250, 255, 234);
    const QColor* pal[7]  = { &edge, &outLit, &outShade, &inLit, &inShade, &core, &coreHi };

    const float SY  = 0.78f;
    const float SX0 = -40.0f, SY0 = -36.0f;
    const int   COLS = 20, ROWS = 18;

    static int cell[18][20];
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            float ux  = SX0 + ((float)c + 0.5f) * PIX;
            float uy  = (SY0 + ((float)r + 0.5f) * PIX) / SY;
            float rad = sqrtf(ux * ux + uy * uy);
            float ang = atan2f(ux, -uy);
            int col = -1;

            const float stepOut = 2.0f * PI_F / 8.0f;
            float u = ang / stepOut;
            u -= floorf(u + 0.5f);
            float rOut = 33.0f + 6.0f * (1.0f - 2.0f * fabsf(u));
            if (rad < rOut)
                col = (fabsf(u) > 0.38f) ? 0 : ((ang <= 0.0f) ? 1 : 2);

            const float stepIn = 2.0f * PI_F / 5.0f;
            float u2 = ang / stepIn + 0.5f;
            u2 -= floorf(u2 + 0.5f);
            float rIn = 23.0f + 4.0f * (1.0f - 2.0f * fabsf(u2));
            if (rad < rIn)
                col = (fabsf(u2) > 0.40f) ? 0 : ((ang <= 0.0f) ? 3 : 4);

            if (rad < 11.0f) col = (rad < 6.5f) ? 6 : 5;

            cell[r][c] = col;
        }
    }

    static int out[18][20];
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            if (cell[r][c] >= 0) { out[r][c] = cell[r][c]; continue; }
            bool touch = (r > 0 && cell[r - 1][c] >= 0)
                      || (r + 1 < ROWS && cell[r + 1][c] >= 0)
                      || (c > 0 && cell[r][c - 1] >= 0)
                      || (c + 1 < COLS && cell[r][c + 1] >= 0);
            out[r][c] = touch ? 0 : -1;
        }
    }

    g.save();
    g.translate(0.0f, SnapPx(soilY) - 32.0f);
    g.rotate(sway * 3.0f + sinf(t * 0.7f) * 0.8f);

    for (int r = 0; r < ROWS; ++r) {
        int c = 0;
        while (c < COLS) {
            int v = out[r][c];
            if (v < 0) { ++c; continue; }
            int c2 = c;
            while (c2 + 1 < COLS && out[r][c2 + 1] == v) ++c2;
            FillRectF(g, *pal[v], SX0 + (float)c * PIX, SY0 + (float)r * PIX,
                      (float)(c2 - c + 1) * PIX, PIX);
            c = c2 + 1;
        }
    }

    g.restore();
}

static void DrawBamboo(QPainter& g, float soilY, float sway, float t)
{
    const QColor stalk     = C(150, 194, 96);
    const QColor stalkDark = C(104, 150, 62);
    const QColor node      = C(84, 126, 50);
    const QColor leaf      = C(118, 182, 78);
    const QColor leafDark  = C(84, 142, 56);

    struct Stalk { float x, h, ph; };
    // 三竿高度按 φ 链递减：158 / 158÷φ ≈ 98 / 98÷φ ≈ 61（上游 v3.8.2）
    const Stalk stalks[3] = {
        { -16.0f,  98.0f, 0.0f },
        {   0.0f, 158.0f, 1.2f },
        {  20.0f,  61.0f, 2.4f }
    };

    g.save();
    g.translate(0.0f, SnapPx(soilY));

    for (int i = 0; i < 3; ++i) {
        const Stalk& sk = stalks[i];
        int segs = (int)(sk.h / 16.0f);
        for (int s = 0; s < segs; ++s) {
            float y0 = -(float)s * 16.0f;
            float k = (float)s / (float)segs;
            float dx = SnapPx(sk.x + (sway * 8.0f + sinf(t * 0.8f + sk.ph) * 1.0f) * k * k);
            FillRectF(g, stalk,     dx - 6.0f, y0 - 16.0f, 12.0f, 16.0f);
            FillRectF(g, stalkDark, dx + 2.0f, y0 - 16.0f,  4.0f, 16.0f);
            if (s > 0) FillRectF(g, node, dx - 8.0f, y0 - 2.0f, 16.0f, 4.0f);
        }

        float dx = SnapPx(sk.x + sway * 8.0f + sinf(t * 0.8f + sk.ph) * 1.0f);
        float ty = -(float)segs * 16.0f;
        DrawPixelLeaf(g, dx, ty + 16.0f, -1, 20.0f, 24.0f, 6.0f,
                      (i & 1) ? leafDark : leaf, nullptr);
        DrawPixelLeaf(g, dx, ty + 20.0f, +1, 16.0f, 24.0f, 6.0f,
                      (i & 1) ? leaf : leafDark, nullptr);
    }

    g.restore();
}

static void DrawRose(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem   = C(74, 132, 62);
    const QColor thorn  = C(58, 104, 48);
    const QColor pDark  = C(158, 32, 52);
    const QColor petal  = C(206, 52, 78);
    const QColor pLight = C(238, 106, 126);

    const float h = 124.0f;
    PtF tip = DrawBentStem(g, 0.0f, soilY, h, sway, stem, 5.0f);

    // 叶位取黄金分割点（上游 v3.8.2）
    DrawLeaf(g, soilY, h, 0.382f, sway, 1, t);
    DrawLeaf(g, soilY, h, 0.618f, sway, -1, t);
    FillRectF(g, thorn, SnapPx(tip.x) - 4.0f, SnapPx(tip.y) + 20.0f, 4.0f, 4.0f);
    FillRectF(g, thorn, SnapPx(tip.x) + 4.0f, SnapPx(tip.y) + 40.0f, 4.0f, 4.0f);

    g.save();
    g.translate(SnapPx(tip.x), SnapPx(tip.y) - 12.0f);
    g.rotate(sway * 14.0f + sinf(t * 1.1f) * 3.0f);

    FillRectF(g, pDark,  -16.0f, -16.0f, 32.0f, 32.0f);
    FillRectF(g, pDark,  -12.0f, -24.0f, 24.0f,  8.0f);
    FillRectF(g, petal,  -12.0f, -12.0f, 24.0f, 24.0f);
    FillRectF(g, petal,   -8.0f, -20.0f, 16.0f,  8.0f);
    FillRectF(g, pLight,  -8.0f,  -8.0f, 16.0f, 12.0f);
    FillRectF(g, pDark,   -4.0f,   0.0f,  8.0f,  8.0f);
    FillRectF(g, pLight,  -4.0f, -16.0f,  4.0f,  4.0f);

    g.restore();
}

// =============================================================================
//  薰衣草：三根带弯的细茎，顶端堆五格花穗，奇数行错开半格
// =============================================================================
static void DrawLavender(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem     = C(88, 146, 68);
    const QColor budA     = C(152, 106, 202);
    const QColor budB     = C(116, 74, 170);
    const QColor budLight = C(198, 160, 236);

    // 上游 v3.8.2 定稿：四穗、苗条、无根部细叶（花穗本身是观赏主体）。
    // x / 高度错开，穗尖横向间距 ≥ 11px，最矮一根放中前，与右穗完全分离。
    struct Spike { float x, h, ph, lean; int grains; };
    const Spike spikes[4] = {
        { -18.0f,  96.0f, 0.0f, -4.0f, 6 },   // 左后，偏高，左倾
        {  -6.0f, 120.0f, 1.1f,  3.0f, 7 },   // 最高，右倾
        {   6.0f,  52.0f, 2.0f,  4.0f, 5 },   // 最矮，中前
        {  22.0f,  84.0f, 2.9f,  3.0f, 6 }    // 右侧独立一根
    };

    g.save();
    g.translate(0.0f, SnapPx(soilY));

    for (int i = 0; i < 4; ++i) {
        const Spike& sp = spikes[i];
        const int   rows = (int)floorf(sp.h / PIX + 0.5f);
        const float bend = sway * 10.0f + sinf(t * 0.9f + sp.ph) * 2.0f + sp.lean;

        // 茎：一格宽，越靠上越往 bend 方向弯
        for (int r = 0; r < rows; ++r) {
            const float k  = (float)r / (float)rows;
            const float dx = SnapPx(sp.x + bend * k * k);
            FillRectF(g, stem, dx - PIX * 0.5f, -(float)(r + 1) * PIX, PIX, PIX);
        }

        // 花穗：从茎尖往上 grains 粒。基部两粒占两格宽（有厚度），
        // 其余全部一格宽（苗条），奇偶行错开半格。
        const float tipX = SnapPx(sp.x + bend);
        const float tipY = -(float)rows * PIX;
        for (int j = 0; j < sp.grains; ++j) {
            const float y   = tipY - (float)j * PIX;
            const float w   = PIX * ((j < 2) ? 2.0f : 1.0f);
            const float off = (j & 1) ? PIX * 0.5f : -PIX * 0.5f;
            const float gx  = tipX + off;
            // 每根穗的明暗节奏错开，四根并排时不至于同相
            const bool  alt = ((j & 1) ^ (i & 1)) != 0;
            FillRectF(g, alt ? budB : budA, gx - w * 0.5f, y, w, PIX);
            FillRectF(g, budLight,          gx - w * 0.5f, y, PIX, PIX);
        }
        // 顶芽
        FillRectF(g, budB, tipX - PIX * 0.5f,
                  tipY - (float)sp.grains * PIX, PIX, PIX);
    }

    g.restore();
}

// =============================================================================
//  三叶草：三片心形像素叶，每片一格一格拼出来
// =============================================================================
static void DrawClover(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem      = C(78, 140, 60);
    const QColor leafA     = C(96, 176, 76);
    const QColor leafB     = C(70, 146, 58);
    const QColor leafLight = C(140, 208, 108);

    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 6.0f + sinf(t * 0.8f) * 1.2f);

    const float h = 52.0f;
    FillRectF(g, stem, -PIX * 0.5f, -h, PIX, h);
    FillRectF(g, stem, -PIX * 2.0f, -h - PIX, PIX * 4.0f, PIX);   // 叶柄分叉

    // 心形叶：7 格宽 / 6 格高，顶行中间留缺口
    struct LeafPos { float cx, cy; };
    const LeafPos pos[3] = {
        { -16.0f, -h - 10.0f },
        {   0.0f, -h - 18.0f },
        {  16.0f, -h - 10.0f }
    };
    const int rowStart[6] = { -3, -3, -3, -2, -1, 0 };
    const int rowCount[6] = {  2,  7,  7,  5,  3, 1 };
    for (int i = 0; i < 3; ++i) {
        float cx = SnapPx(pos[i].cx + sway * 4.0f);
        float cy = SnapPx(pos[i].cy);
        for (int r = 0; r < 6; ++r) {
            float y = cy + (float)r * PIX;
            if (r == 0) {
                // 顶行两个叶瓣，中间断开
                FillRectF(g, leafA, cx - 3 * PIX, y, 2 * PIX, PIX);
                FillRectF(g, leafA, cx + 1 * PIX, y, 2 * PIX, PIX);
                continue;
            }
            QColor br = (i & 1) ? leafB : leafA;
            FillRectF(g, br, cx + (float)rowStart[r] * PIX, y,
                      (float)rowCount[r] * PIX, PIX);
        }
        // 叶脉高光
        FillRectF(g, leafLight, cx - PIX * 0.5f, cy + PIX, PIX, PIX * 2.0f);
    }

    g.restore();
}

// =============================================================================
//  辣椒：矮灌木 + 三只垂下来的红辣椒
// =============================================================================
static void DrawChili(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem     = C(84, 142, 60);
    const QColor leaf     = C(92, 168, 72);
    const QColor leafDark = C(64, 126, 52);
    const QColor pod      = C(212, 58, 46);
    const QColor podDark  = C(158, 34, 30);
    const QColor podLight = C(244, 118, 92);

    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 6.0f + sinf(t * 0.7f) * 1.0f);

    const float h = 84.0f;
    FillRectF(g, stem, -PIX * 0.5f, -h - 8.0f, PIX, h + 8.0f);

    // 枝叶
    DrawPixelLeaf(g, -2.0f, -h * 0.55f, -1, 24.0f, 24.0f, 6.0f, leaf, nullptr);
    DrawPixelLeaf(g,  2.0f, -h * 0.72f, +1, 22.0f, 24.0f, 6.0f, leafDark, nullptr);
    DrawPixelLeaf(g, -2.0f, -h * 0.90f, -1, 18.0f, 20.0f, 6.0f, leafDark, nullptr);
    DrawPixelLeaf(g,  2.0f, -h * 0.98f, +1, 16.0f, 20.0f, 6.0f, leaf, nullptr);

    // 辣椒：8px 宽的红条垂下，尾端收成 4px 尖
    struct Pod { float x, top, len; };
    const Pod pods[3] = {
        { -14.0f, -h * 0.50f, 22.0f },
        {  12.0f, -h * 0.62f, 26.0f },
        {   0.0f, -h * 0.78f, 20.0f }
    };
    for (int i = 0; i < 3; ++i) {
        float x    = SnapPx(pods[i].x + sway * 5.0f);
        float top  = SnapPx(pods[i].top);
        int   rows = (int)floorf(pods[i].len / PIX + 0.5f);
        for (int r = 0; r < rows; ++r) {
            float y = top + (float)r * PIX;
            FillRectF(g, pod,      x - PIX, y, PIX * 2.0f, PIX);
            FillRectF(g, podDark,  x + PIX * 0.5f, y, PIX * 0.5f, PIX);
            FillRectF(g, podLight, x - PIX, y, PIX * 0.5f, PIX);
        }
        FillRectF(g, pod, x - PIX * 0.5f, top + (float)rows * PIX, PIX, PIX);
        FillRectF(g, leafDark, x - PIX * 1.5f, top - PIX, PIX * 3.0f, PIX);  // 蒂
    }

    g.restore();
}

// =============================================================================
//  蒲公英：细茎顶一颗白色绒球，旁边飘走一颗种子
// =============================================================================
static void DrawDandelion(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem      = C(122, 158, 82);
    const QColor stemDark  = C(94, 128, 62);
    const QColor leaf      = C(104, 170, 78);
    const QColor puffShade = C(206, 212, 200);
    const QColor puff      = C(246, 246, 238);
    const QColor fly       = C(255, 255, 250);

    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 8.0f + sinf(t * 1.0f) * 2.0f);

    const float h = 104.0f;
    FillRectF(g, stem,     -PIX, -h, PIX * 2.0f, h);
    FillRectF(g, stemDark,  PIX, -h, PIX, h);

    // 贴地叶片（抬高一点，免得被花盆沿挡住）
    DrawPixelLeaf(g, -2.0f, -10.0f, -1, 34.0f, 30.0f, 6.0f, leaf, nullptr);
    DrawPixelLeaf(g,  2.0f, -16.0f, +1, 28.0f, 26.0f, 6.0f, leaf, nullptr);

    // 绒球：两层像素圆 + 外圈八颗绒毛
    const float cx = 0.0f, cy = -h - 12.0f;
    FillPixelEllipse(g, puffShade, cx, cy, 20.0f, 20.0f);
    FillPixelEllipse(g, puff,      cx, cy, 16.0f, 16.0f);
    for (int i = 0; i < 8; ++i) {
        float a = (float)i * PI_F / 4.0f;
        FillRectF(g, puff, SnapPx(cx + cosf(a) * 22.0f) - PIX * 0.5f,
                         SnapPx(cy + sinf(a) * 22.0f) - PIX * 0.5f, PIX, PIX);
    }

    // 飘走的种子
    float fx = 28.0f + sinf(t * 0.9f) * 6.0f;
    float fy = cy - 26.0f + cosf(t * 0.7f) * 4.0f;
    FillRectF(g, fly, SnapPx(fx), SnapPx(fy), PIX, PIX);

    g.restore();
}

// =============================================================================
//  雏菊：细茎 + 两片黄金分割叶位 + 八瓣花头，花头绕茎尖轻摆
// =============================================================================
static void DrawDaisy(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem       = C(96, 158, 72);
    const QColor petal      = C(250, 250, 242);
    const QColor petalShade = C(216, 220, 204);
    const QColor core       = C(244, 196, 58);
    const QColor coreDark   = C(206, 150, 34);

    const float h = 98.0f;
    const PtF tip = DrawBentStem(g, 0.0f, soilY, h, sway, stem, 6.0f);
    DrawLeaf(g, soilY, h, 0.382f, sway, -1, t);
    DrawLeaf(g, soilY, h, 0.618f, sway, +1, t);

    g.save();
    g.translate(SnapPx(tip.x), SnapPx(tip.y) - 8.0f);
    g.rotate(sway * 15.0f + sinf(t * 1.5f) * 3.0f);

    // 花瓣：八瓣交错两色，中心补一块白底盘
    const int n = 8;
    for (int i = 0; i < n; ++i) {
        const float a  = (float)i * (2.0f * PI_F) / (float)n;
        const float px = SnapPx(cosf(a) * 16.0f);
        const float py = SnapPx(sinf(a) * 16.0f);
        FillRectF(g, (i & 1) ? petalShade : petal, px - 4.0f, py - 4.0f, 8.0f, 8.0f);
    }
    FillPixelEllipse(g, petal,    0.0f, 0.0f, 10.0f, 10.0f);
    FillPixelEllipse(g, core,     0.0f, 0.0f,  7.0f,  7.0f);
    FillPixelEllipse(g, coreDark, 2.0f, 2.0f,  3.5f,  3.5f);

    g.restore();
}

// =============================================================================
//  芦荟（上游 v3.7 重绘）：后排三片暗叶 + 前排三片亮叶
//
//  叶片基部最宽、向上收窄、中段起按 k^1.6 往外弯，尖端收成一格，
//  前排叶面带一条高光条纹。前排压在后排之上，才有层次。
// =============================================================================
static void DrawAloe(QPainter& g, float soilY, float sway, float t)
{
    const QColor back       = C(62, 126, 92);
    const QColor backDark   = C(46, 100, 74);
    const QColor blade      = C(96, 168, 118);
    const QColor bladeDark  = C(62, 126, 92);
    const QColor bladeLight = C(150, 208, 160);
    const QColor stripe     = C(198, 234, 198);

    struct Frond { float x, h, lean; };
    const Frond backF[3] = {
        { -20.0f, 78.0f, -18.0f },
        {   0.0f, 96.0f,   0.0f },
        {  20.0f, 74.0f,  20.0f }
    };
    const Frond frontF[3] = {
        { -14.0f, 62.0f, -22.0f },
        {   2.0f, 80.0f,   6.0f },
        {  16.0f, 56.0f,  24.0f }
    };

    struct Pal { QColor main, dark, light; bool stripeOn; };
    const Pal pals[2] = {
        { back,  backDark,  backDark,   false },
        { blade, bladeDark, bladeLight, true  }
    };

    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 4.0f + sinf(t * 0.8f) * 1.0f);

    for (int layer = 0; layer < 2; ++layer) {
        const Pal& pal = pals[layer];
        const Frond* fs = (layer == 0) ? backF : frontF;
        for (int i = 0; i < 3; ++i) {
            const Frond& f = fs[i];
            const int rows = (int)floorf(f.h / PIX + 0.5f);
            for (int r = 0; r < rows; ++r) {
                const float k  = (float)r / (float)rows;      // 0=根部 1=叶尖
                const float cx = SnapPx(f.x + f.lean * powf(k, 1.6f) + sway * 5.0f * k);
                float hw = PIX * (1.5f - k);                   // 基部三格宽 → 尖端一格
                if (hw < PIX * 0.5f) hw = PIX * 0.5f;
                const float y = -(float)(r + 1) * PIX;
                FillRectF(g, pal.main,  cx - hw, y, hw * 2.0f, PIX);
                FillRectF(g, pal.dark,  cx + hw - PIX * 0.5f, y, PIX * 0.5f, PIX);
                FillRectF(g, pal.light, cx - hw, y, PIX * 0.5f, PIX);
                if (pal.stripeOn && k > 0.2f && k < 0.92f)
                    FillRectF(g, stripe, cx - PIX * 0.5f, y, PIX, PIX);
            }
        }
    }

    g.restore();
}

// =============================================================================
//  牵牛花：细茎顶着喇叭形花冠，喇叭口朝上，喉部白色带星纹
// =============================================================================
static void DrawMorningGlory(QPainter& g, float soilY, float sway, float t)
{
    const QColor stem       = C(84, 148, 66);
    const QColor leafA      = C(104, 174, 82);
    const QColor leafB      = C(74, 138, 60);
    const QColor petal      = C(108, 122, 214);
    const QColor petalLight = C(150, 160, 240);
    const QColor petalDark  = C(76, 84, 172);
    const QColor throat     = C(246, 244, 230);
    const QColor star       = C(120, 100, 190);

    const float h = 118.0f;
    const PtF tip = DrawBentStem(g, 0.0f, soilY, h, sway, stem, 6.0f);

    // 两片细长叶
    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 8.0f);
    DrawPixelLeaf(g, 0.0f,  0.0f, -1, 34.0f, 26.0f, 7.0f, leafA, nullptr);
    DrawPixelLeaf(g, 0.0f, -6.0f, +1, 28.0f, 24.0f, 6.0f, leafB, nullptr);
    g.restore();

    // 花冠：绕茎尖轻轻摆动
    g.save();
    g.translate(SnapPx(tip.x), SnapPx(tip.y) - 4.0f);
    g.rotate(sway * 12.0f + sinf(t * 1.3f) * 3.0f);

    // 花冠整体放大约 1.3 倍（上游 36×40 → 48×52）。
    // 上游那个尺寸相对 118 的茎太轻，正视图只剩"紫块夹一条白"，看不出喇叭形。
    // 每段的宽高都取 4 的倍数，保证仍然落在像素网格上。
    FillRectF(g, petalDark, -4.0f,  -12.0f,  8.0f, 12.0f);      // 花萼
    // 漏斗管：从下往上逐行加宽
    FillRectF(g, petal,      -8.0f, -20.0f, 16.0f,  8.0f);
    FillRectF(g, petalLight, -8.0f, -20.0f,  4.0f,  8.0f);
    FillRectF(g, petal,     -16.0f, -32.0f, 32.0f, 12.0f);
    FillRectF(g, petalLight, -16.0f, -32.0f,  4.0f, 12.0f);
    FillRectF(g, petal,     -24.0f, -44.0f, 48.0f, 12.0f);
    FillRectF(g, petalDark,  18.0f, -32.0f,  6.0f, 20.0f);      // 右侧背光
    // 喇叭口：白色喉部 + 深色星纹（中间再点一撮白）
    FillRectF(g, throat,    -24.0f, -52.0f, 48.0f,  8.0f);
    FillRectF(g, star,       -8.0f, -52.0f, 16.0f,  8.0f);
    FillRectF(g, throat,     -2.0f, -52.0f,  4.0f,  8.0f);

    g.restore();
}

// =============================================================================
//  樱花：褐色短干 + 三团粉色树冠，树冠里点几朵小白花，旁边飘着一片花瓣
// =============================================================================
static void DrawCherry(QPainter& g, float soilY, float sway, float t)
{
    const QColor trunk    = C(122, 88, 66);
    const QColor trunkDark= C(94, 64, 46);
    const QColor blobA    = C(244, 178, 196);
    const QColor blobB    = C(224, 142, 168);
    const QColor blobC    = C(252, 208, 220);
    const QColor flower   = C(250, 236, 244);
    const QColor petalFly = C(248, 184, 202);

    // 树干拉高到 88（上游是 62）。上游的树干只有 62 高却顶着三团圆冠，
    // 比例上整个像一朵粉色蘑菇；拉高之后才像一棵树。
    const float h = 88.0f;      // 树干高

    g.save();
    g.translate(0.0f, SnapPx(soilY));
    g.rotate(sway * 4.0f + sinf(t * 0.9f) * 1.4f);

    // 树干（比上游细一档：8px 宽）+ 一根小枝
    FillRectF(g, trunk,     -4.0f, -h, 8.0f, h);
    FillRectF(g, trunkDark,  1.0f, -h, 3.0f, h);
    FillRectF(g, trunk,      3.0f, -h + 16.0f, 18.0f, 4.0f);

    // 树冠：侧团下移并缩小，与主团拉开，不再叠成一个圆盘。
    // 侧团半径 = 主团 × 1/φ，高处受风摆幅更大。
    FillPixelEllipse(g, blobB, -24.0f + sway * 6.0f, -h - 4.0f,  14.0f, 10.0f);
    FillPixelEllipse(g, blobB,  24.0f + sway * 8.0f, -h - 2.0f,  14.0f, 10.0f);
    FillPixelEllipse(g, blobA,   0.0f + sway * 10.0f, -h - 16.0f, 24.0f, 17.0f);
    FillPixelEllipse(g, blobC,  -7.0f + sway * 10.0f, -h - 22.0f, 11.0f,  7.0f);
    FillPixelEllipse(g, blobC,  13.0f + sway * 8.0f,  -h - 10.0f,  7.0f,  6.0f);

    // 几朵小白花
    FillRectF(g, flower, -12.0f + sway * 8.0f,  -h - 18.0f, 4.0f, 4.0f);
    FillRectF(g, flower,   7.0f + sway * 10.0f, -h - 25.0f, 4.0f, 4.0f);
    FillRectF(g, flower,  18.0f + sway * 8.0f,  -h - 11.0f, 4.0f, 4.0f);

    g.restore();

    // 飘落的花瓣：从树冠右侧缓缓下坠，落到底再回到树冠循环
    const float pa = fmodf(t * 0.5f, 1.0f);
    const float fx = SnapPx(30.0f + sinf(t * 0.8f) * 10.0f + pa * 14.0f);
    const float fy = SnapPx(-h - 22.0f + pa * 46.0f + SnapPx(soilY));
    FillRectF(g, petalFly, fx, fy, PIX, PIX);
}

static inline float PlantHeight(PlantType t)
{
    return (t >= 0 && t < PLANT_COUNT) ? kPlantH[t] : 110.0f;
}

static void DrawPlantLocal(QPainter& g, float soilY, PlantType t,
                           float sway, bool poking, float now)
{
    switch (t) {
    case PLANT_CACTUS:    DrawCactus(g, soilY, sway); break;
    case PLANT_FLYTRAP:   DrawFlytrap(g, soilY, sway, now, poking ? 0.45f : 0.0f); break;
    case PLANT_TULIP:     DrawTulip(g, soilY, sway, now); break;
    case PLANT_MUSHROOM:  DrawMushroom(g, soilY, sway, now); break;
    case PLANT_PINE:      DrawPine(g, soilY, sway, now); break;
    case PLANT_SUCCULENT: DrawSucculent(g, soilY, sway, now); break;
    case PLANT_BAMBOO:    DrawBamboo(g, soilY, sway, now); break;
    case PLANT_ROSE:      DrawRose(g, soilY, sway, now); break;
    case PLANT_LAVENDER:  DrawLavender(g, soilY, sway, now); break;
    case PLANT_CLOVER:    DrawClover(g, soilY, sway, now); break;
    case PLANT_CHILI:     DrawChili(g, soilY, sway, now); break;
    case PLANT_DANDELION: DrawDandelion(g, soilY, sway, now); break;
    case PLANT_DAISY:        DrawDaisy(g, soilY, sway, now); break;
    case PLANT_ALOE:         DrawAloe(g, soilY, sway, now); break;
    case PLANT_MORNINGGLORY: DrawMorningGlory(g, soilY, sway, now); break;
    case PLANT_CHERRY:       DrawCherry(g, soilY, sway, now); break;
    case PLANT_SUNFLOWER:
    default:              DrawSunflower(g, soilY, sway, now); break;
    }
}

// -----------------------------------------------------------------------------
//  --dump-plants <path>
//
//  把十三种植物并排渲染成一张 PNG 后退出。用来生成 README 的展示图，
//  也方便改了像素数据之后直接看图验证，不用盯着桌面猜。
// -----------------------------------------------------------------------------
static bool DumpPlants(const QString& path)
{
    // 卡片式：深色底 + 花盆 + 植物 + 底部标签
    const int cellW = 216, cellH = 296;
    const int cols  = 5;
    const int rows  = (PLANT_COUNT + cols - 1) / cols;

    const QColor bg      = C(33, 37, 43);
    const QColor label   = C(150, 158, 172);
    const QColor divider = C(58, 63, 71);

    const float hh = POT_H * 0.5f;

    QImage img(cellW * cols, cellH * rows, QImage::Format_ARGB32_Premultiplied);
    img.fill(bg);

    QPainter g(&img);
    g.setRenderHint(QPainter::Antialiasing, false);
    g.setRenderHint(QPainter::SmoothPixmapTransform, false);
    g.setRenderHint(QPainter::TextAntialiasing, true);

    QFont f = g.font();
    f.setPixelSize(17);
    g.setFont(f);

    // TBP_WILT=0..1 强制指定枯萎程度，用来验证枯叶配色（不给就用健康的绿）
    bool wok = false;
    const float forcedWilt = qgetenv("TBP_WILT").toFloat(&wok);
    g_wiltAmount = wok ? Clampf(forcedWilt, 0.0f, 1.0f) : 0.0f;

    for (int i = 0; i < PLANT_COUNT; ++i) {
        const int col = i % cols;
        const int row = i / cols;

        const int inRow = qMin(cols, PLANT_COUNT - row * cols);
        const int offX  = (cols - inRow) * cellW / 2;

        const int cx    = offX + col * cellW + cellW / 2;
        const int potCy = row * cellH + cellH - 74;

        g.save();
        g.translate(cx, potCy);
        DrawPlantLocal(g, -hh + 3.0f, (PlantType)i, 0.0f, false, 0.0f);
        DrawPotLocal(g);
        g.restore();

        g.setPen(label);
        const QRect labelRc(offX + col * cellW, row * cellH + cellH - 46, cellW, 30);
        g.drawText(labelRc, Qt::AlignHCenter | Qt::AlignVCenter,
                   QString::fromUtf8(kPlantNames[i]));

        g.setPen(QPen(divider, 1));
        g.drawLine(offX + col * cellW + 18, row * cellH + cellH - 6,
                   offX + (col + 1) * cellW - 18, row * cellH + cellH - 6);
    }

    g_wiltAmount = 0.0f;
    g.end();
    return img.save(path);
}

// =============================================================================
//  二、物理（常数照搬）
// =============================================================================
struct Body {
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    float angle = 0, angVel = 0;
    bool  dragged = false;
};

struct Crumb {
    float x, y, vx, vy, life, floorY;
};

struct Pot {
    Body      body;
    PlantType plant = PLANT_SUNFLOWER;
    float     sway = 0, swayVel = 0;
    bool      poking = false;
    float     dragOffX = 0, dragOffY = 0;
    float     surfaceY = 0;       // 实际承托面（花盆站在哪）
    float     shadowY = 0;        // 投影落在哪（正下方最近的平面）
    float     supportY = 0;
    int       supportId = -1;     // 保留字段（当前实现无状态，不再依赖）
    float     supportTop = 0;     // 上一帧承托面顶边（绝对 y）
    float     supportLeft = 0;    // 上一帧承托面左边（绝对 x）
    float     supportWidth = 0;   // 上一帧承托面宽度（用于识别"还是同一个窗口"）
    float     prevLowestAbs = -1e9f; // 上一帧最低点；spawnPot 里会初始化成实际位置
    float     uprightDelay = 0.0f;   // 摔倒静止后已经躺了多久（秒）
    bool      rising = false;        // 正在从翻车状态爬起来（期间不应用失稳逻辑）
    bool      everSupported = false;
    float     scale = 1.0f;          // 单盆缩放（POT_SCALE_MIN ~ POT_SCALE_MAX）
    float     moisture = 1.0f;       // 泥土水分 1=湿透 0=干透
    float     waterAnim = 0.0f;      // 浇水动画剩余时间（秒）
    float     shedTimer = 0.0f;      // 枯叶掉落的节流计时
    float     windPh = 0.0f;         // 常年微风的相位偏移，每盆错开
    // 在已保存布局里的槽位；-1 = 运行时新放的盆，还没进过快照
    int       slot = -1;
};

// 一份"已保存的布局"。ini 里写的就是这个。
// 自动保存只回写各槽位的水分，位置 / 数量 / 缩放永远不动 ——
// 拖动、抛掷、放新盆、移除盆，只有编辑模式里点「保存」才会固化。
struct PotSeed {
    int   plant    = 0;
    float x        = 0.0f;
    float y        = 0.0f;
    float scale    = 1.0f;
    float moisture = 1.0f;
};

// 前向声明：日志函数主体在后面（它依赖一些启动期的东西）
static void tbpLog(const QString& msg);

// -----------------------------------------------------------------------------
//  布局文件
//
//  上游放在 exe 旁边的 ini；Linux 上按 XDG 放到 ~/.config/taskbarplant/layout.conf。
//  格式是纯文本，一行一盆：
//      <植物编号> <x> <y> <缩放> <水分>
//  外加一行 dry <秒> 记录"从湿透到干透"的时长。
//
//  为什么不用 QSettings / JSON：上游就是这么做，格式简单到可以手改，
//  而且出问题时 cat 一眼就能看出哪盆的位置写坏了。
// -----------------------------------------------------------------------------
static QString ConfigPath()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.config");
    return dir + QStringLiteral("/taskbarplant/layout.conf");
}

static void LoadConfigFile(std::vector<PotSeed>& layout, float* drySeconds)
{
    layout.clear();
    QFile f(ConfigPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        if (line.startsWith(QStringLiteral("dry"))) {
            bool ok = false;
            const float d = line.mid(3).trimmed().toFloat(&ok);
            if (ok && drySeconds) *drySeconds = Clampf(d, 60.0f, 86400.0f);
            continue;
        }

        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                            Qt::SkipEmptyParts);
        if (parts.size() < 3) continue;

        PotSeed s;
        bool ok1 = false, ok2 = false, ok3 = false;
        s.plant = parts[0].toInt(&ok1);
        s.x     = parts[1].toFloat(&ok2);
        s.y     = parts[2].toFloat(&ok3);
        if (!ok1 || !ok2 || !ok3) continue;
        if (s.plant < 0 || s.plant >= PLANT_COUNT) s.plant = 0;

        bool ok4 = false, ok5 = false;
        if (parts.size() > 3) s.scale    = parts[3].toFloat(&ok4);
        if (parts.size() > 4) s.moisture = parts[4].toFloat(&ok5);
        s.scale    = Clampf(ok4 ? s.scale : 1.0f, POT_SCALE_MIN, POT_SCALE_MAX);
        s.moisture = Clampf(ok5 ? s.moisture : 1.0f, 0.0f, 1.0f);
        layout.push_back(s);
    }
}

static void SaveConfigFile(const std::vector<PotSeed>& layout, float drySeconds)
{
    const QString path = ConfigPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        tbpLog(QStringLiteral("config: 写不进去 %1").arg(path));
        return;
    }
    QTextStream out(&f);
    out << "# TaskbarPlant: plant x y scale moisture\n";
    out << "dry " << (int)drySeconds << "\n";
    for (size_t i = 0; i < layout.size(); ++i) {
        const PotSeed& s = layout[i];
        out << s.plant << ' ' << QString::number(s.x, 'f', 1) << ' '
            << QString::number(s.y, 'f', 1) << ' '
            << QString::number(s.scale, 'f', 3) << ' '
            << QString::number(s.moisture, 'f', 3) << '\n';
    }
}

static float PotMaxY(float angle, float potScale = 1.0f)
{    const float hw = POT_W * 0.5f * potScale;
    const float hh = POT_H * 0.5f * potScale;
    const float c = cosf(angle);
    const float s = sinf(angle);
    float maxY = -1e9f;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            float ry = (float)sx * hw * s + (float)sy * hh * c;
            if (ry > maxY) maxY = ry;
        }
    }
    return maxY;
}

// -----------------------------------------------------------------------------
//  导出一株植物为 PNG（透明底）
//
//  和 DumpPlants 的区别：这里只画一个花盆，尺寸按该盆的缩放和植物高度自适应，
//  输出的图可以直接贴到别处。
// -----------------------------------------------------------------------------
static bool ExportPotPng(const Pot& p, const QString& path)
{
    const float h    = PlantHeight(p.plant) * p.scale * g_baseScale;
    const float ph   = POT_Hf() * p.scale;
    const int   pad  = 24;
    const int   w    = (int)(POT_Wf() * p.scale * 2.6f) + pad * 2;
    const int   ht   = (int)(h + ph) + pad * 2 + 8;

    QImage img(w, ht, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter g(&img);
    g.setRenderHint(QPainter::Antialiasing, false);

    const float savedScale = g_scale;
    const float savedWilt  = g_wiltAmount;

    g_scale = g_baseScale * p.scale;
    g_wiltAmount = (p.moisture < DRY_THRESHOLD)
                   ? (DRY_THRESHOLD - p.moisture) / DRY_THRESHOLD : 0.0f;

    g.translate(w * 0.5f, ht - pad - ph * 0.5f);
    DrawPlantLocal(g, -ph * 0.5f + 3.0f, p.plant, 0.0f, false, 0.0f);
    DrawPotLocal(g);

    g_scale = savedScale;
    g_wiltAmount = savedWilt;
    g.end();

    return img.save(path);
}



// =============================================================================
//  三、窗口承托面
//
//  原版用 EnumWindows + GetWindowRect 找"花盆脚下最近的窗口标题栏顶边"。
//
//  Linux 下有两个数据源：
//    A) KWin 脚本桥（首选）：程序注册 D-Bus 服务，KWin 脚本每 33ms 把所有普通
//       窗口的 frameGeometry 推过来。**这是唯一能覆盖 Wayland 原生窗口的办法** ——
//       Wayland 协议禁止客户端查询别的应用窗口位置，只有合成器知道。
//    B) X11 枚举（回退）：_NET_CLIENT_LIST + XTranslateCoordinates + _NET_FRAME_EXTENTS。
//       只能看到 XWayland 窗口，纯 Wayland 应用看不到。
//
//  判定采用"穿越检测"：窗口顶边必须落在 [上一帧最低点, 这一帧最低点] 之间，
//  也就是这一帧花盆从该顶边上方跨到下方。只判"停在附近"会漏接 ——
//  重力 2400 下一帧位移可达 26px，花盆会直接穿过标题栏。
// =============================================================================

#if defined(TBP_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

// Xlib 的默认错误处理会直接 abort 进程。窗口销毁 / 重排时偶尔会撞上 BadWindow，
// 这里吞掉即可（下一帧会自动重设）。
static int g_xErrSink = 0;
static int tbpXErrorHandler(Display*, XErrorEvent*) { ++g_xErrSink; return 0; }
#endif

// 一个可站立的窗口外框（屏幕绝对坐标，已含标题栏）
struct WinGeom {
    float x = 0, y = 0, w = 0, h = 0;
};

// KWin 推过来的几何。非空即表示桥已工作。
static std::vector<WinGeom> g_kwinGeoms;
static bool                 g_kwinLive = false;

// 诊断日志：同时输出到终端（stderr）和 ~/.cache/taskbarplant/run.log。
// 直接在终端启动就能看到全部过程，不需要任何启动脚本。
static void tbpLog(const QString& msg)
{
    // 终端优先：用户前台运行时立刻可见
    if (qEnvironmentVariableIsSet("TBP_QUIET") == false) {
        fprintf(stderr, "[tbp] %s\n", msg.toLocal8Bit().constData());
        fflush(stderr);
    }

    static QString path;
    if (path.isEmpty()) {
        QString d = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (d.isEmpty()) d = QDir::homePath() + "/.cache/taskbarplant";
        QDir().mkpath(d);
        path = d + "/run.log";
    }
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << msg << "\n";
        ts.flush();
    }
}

static inline float Absf(float v) { return v < 0.0f ? -v : v; }

// -----------------------------------------------------------------------------
//  常年微风（上游 v3.6.0）
//
//  让植物在没有交互时也轻轻摇。风是两列以上的正弦叠加，周期互不成整数比，
//  所以看起来不循环 —— 单一 sin 会让人看出节拍。
//  返回值 7.5 + 4.0 + 2.5 的量级是"px 级力矩"，直接加在摆动弹簧的速度上。
// -----------------------------------------------------------------------------
static inline float WindForce(float t)
{
    // 强弱起伏：sin(t * 0.23) 的周期是 2π/0.23 ≈ 27 秒。
    // （上游注释写的是"约 4.3 秒一阵"，那是把频率 0.23 当成了周期，实际慢得多）
    const float gust = 0.55f + 0.45f * sinf(t * 0.23f + 0.9f);
    return gust * (7.5f * sinf(t * 0.7f)
                 + 4.0f * sinf(t * 1.73f + 1.3f)
                 + 2.5f * sinf(t * 3.1f + 0.7f));
}

// 天气系统的"风雨增幅"。降水期间平滑升到 1，晴天回落到 0。
// 风力和摆动上限都乘它 —— 下雨下雪时植物晃得更厉害。
static float g_storm = 0.0f;

// =============================================================================
//  一·五、天气系统（上游 v3.7，v3.8 / v3.8.1 / v3.8.2 / v3.8.3 的调整全部同步）
//
//  照搬上游这一段：三个粒子结构、区域聚类、调度、积水积雪演化、云的绘制，
//  以及植物冠层积雪点表。只依赖花盆列表 / 窗口宽度 / 地面高度 / 时间，
//  没有任何平台相关的东西。
// =============================================================================
enum WeatherKind { WEATHER_CLEAR = 0, WEATHER_RAIN, WEATHER_SNOW };

struct RainDrop  { float x, y, vy; };                 // 竖直下落的雨线
struct SnowFlake { float x, y, vy, ph, amp; };        // 横向摇摆着飘的雪花
struct Splash    { float x, y, vx, vy, life; };       // 落水 / 砸盆溅起的水珠
struct WeatherZone { float l, r, top; };              // 一个降水区域的包围盒
struct SnowSpot  { float x, y, hw; };                 // 冠层上能积雪的一个点

static const float PUDDLE_MAX     = 6.0f;             // 积水最大深度 px
static const float SNOW_MAX       = 6.0f;             // 积雪最大厚度 px
static const float RAIN_SPEED_MIN = 900.0f;           // 雨滴下落速度 px/s
static const float RAIN_SPEED_MAX = 1250.0f;
static const float WEATHER_ZONE_GAP = 240.0f;         // 簇与簇之间允许的最大空隙 px

static int   g_weather      = WEATHER_CLEAR;
static float g_weatherTimer = 25.0f;    // 晴天时距下一次"掷骰子"的倒计时（秒）
static float g_weatherLeft  = 0.0f;     // 当前降水的剩余时间（秒）
static float g_precip       = 0.0f;     // 降水强度 0~1（起雨 / 停雨平滑过渡）
static float g_puddle       = 0.0f;     // 任务栏上沿的积水深度 px
static float g_snowCover    = 0.0f;     // 地面与植物上的积雪厚度 px
static int   g_lastWeather  = WEATHER_CLEAR;  // 上一次的天气（切换时清空另一种粒子）

static std::vector<RainDrop>    g_rain;
static std::vector<SnowFlake>   g_flakes;
static std::vector<Splash>      g_splash;
static std::vector<WeatherZone> g_zones;

// 天气区域（v3.8.1 分区，v3.8.3 实时化）：
// 按 x 给花盆排序聚类，相邻两盆的空隙超过 WEATHER_ZONE_GAP 就分成两簇，
// 各自下各自的雨雪，不再把降水洒在两盆之间的空地上。
// 每帧都随花盆当前位置重算 —— 拖拽中的花盆实时跟着走。
// 没有花盆时返回空区域：一盆都没有就不该下雨（v3.8.3 之前的全屏兜底是 bug）。
static void RebuildWeatherZones(const std::vector<Pot>& pots, int winW, float floorY)
{
    g_zones.clear();
    if (pots.empty() || winW <= 0) return;

    std::vector<size_t> order(pots.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    // 盆数很少，插入排序足够
    for (size_t i = 1; i < order.size(); ++i)
        for (size_t j = i; j > 0 && pots[order[j]].body.x < pots[order[j - 1]].body.x; --j) {
            size_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }

    float cl = 1e9f, cr = -1e9f, ctop = 1e9f;
    for (size_t i = 0; i < order.size(); ++i) {
        const Pot& p = pots[order[i]];
        const float halfW = (POT_W * 0.5f + 44.0f) * p.scale;
        const float pTop  = p.body.y - POT_H * 0.5f * p.scale
                          - (PlantHeight(p.plant) + 44.0f) * p.scale;

        if (cl <= cr && p.body.x - halfW - cr > WEATHER_ZONE_GAP) {
            WeatherZone z = { Clampf(cl, 0.0f, (float)winW),
                              Clampf(cr, 0.0f, (float)winW),
                              Clampf(ctop - 60.0f, 20.0f, floorY - 80.0f) };
            g_zones.push_back(z);
            cl = 1e9f; cr = -1e9f; ctop = 1e9f;
        }
        if (p.body.x - halfW < cl) cl = p.body.x - halfW;
        if (p.body.x + halfW > cr) cr = p.body.x + halfW;
        if (pTop < ctop) ctop = pTop;
    }
    if (cl <= cr) {
        WeatherZone z = { Clampf(cl, 0.0f, (float)winW),
                          Clampf(cr, 0.0f, (float)winW),
                          Clampf(ctop - 60.0f, 20.0f, floorY - 80.0f) };
        g_zones.push_back(z);
    }
}

// 水珠：花盆砸进积水 / 雨滴落地时溅起
static void SpawnSplash(float x, float y, int count)
{
    if (g_splash.size() > 120) return;
    for (int i = 0; i < count; ++i) {
        Splash s;
        s.x = x + (float)((rand() % 61) - 30);
        s.y = y - (float)(rand() % 3);
        s.vx = (float)((rand() % 220) - 110);
        s.vy = -(float)(150 + rand() % 160);
        s.life = 0.35f + (float)(rand() % 20) / 100.0f;
        g_splash.push_back(s);
    }
}

static void UpdateWeather(float dt, std::vector<Pot>& pots, int winW, float floorY, float now)
{
    // ---- 调度：晴天每 20 秒掷一次骰子，35% 雨 / 17% 雪 ----
    if (g_weather == WEATHER_CLEAR) {
        g_weatherTimer -= dt;
        if (g_weatherTimer <= 0.0f) {
            g_weatherTimer = 20.0f;
            const int roll = rand() % 100;
            if (roll < 35) {                        // 35% 下雨
                g_weather = WEATHER_RAIN;
                g_weatherLeft = 18.0f + (float)(rand() % 25);   // 18~42 秒
            } else if (roll < 52) {                 // 17% 下雪
                g_weather = WEATHER_SNOW;
                g_weatherLeft = 25.0f + (float)(rand() % 30);   // 25~54 秒
            }
        }
    } else {
        g_weatherLeft -= dt;
        if (g_weatherLeft <= 0.0f) {
            g_weather = WEATHER_CLEAR;
            g_weatherTimer = 45.0f + (float)(rand() % 90);      // 雨停后 45~134 秒
        }
    }

    // 风力 / 降水强度都平滑过渡，不会一开一停就突兀
    const float target = (g_weather != WEATHER_CLEAR) ? 1.0f : 0.0f;
    g_storm  += (target - g_storm)  * Clampf(dt * 0.8f, 0.0f, 1.0f);
    g_precip += (target - g_precip) * Clampf(dt * 0.6f, 0.0f, 1.0f);

    // 雨雪互斥：切换时立刻清掉另一种粒子，避免同屏重叠
    if (g_weather != g_lastWeather) {
        if (g_weather == WEATHER_SNOW)      g_rain.clear();
        else if (g_weather == WEATHER_RAIN) g_flakes.clear();
        g_lastWeather = g_weather;
    }

    RebuildWeatherZones(pots, winW, floorY);

    // ---- 粒子生成：每个区域按宽度铺密度，密度乘 g_precip 做平滑起落 ----
    for (size_t zi = 0; zi < g_zones.size(); ++zi) {
        const WeatherZone& z = g_zones[zi];
        const float zw = z.r - z.l;
        if (zw < 20.0f) continue;

        if (g_weather == WEATHER_RAIN && g_rain.size() < 70) {
            int n = (int)(zw / 55.0f) + 1;
            if (n > 6) n = 6;
            n = (int)(n * g_precip + 0.5f);
            for (int i = 0; i < n; ++i) {
                RainDrop d;
                d.x  = z.l + (float)(rand() % 1000) * 0.001f * zw;
                // 出生点贴云底（z.top），雨的最上沿与云的最下沿相接
                d.y  = z.top + (float)(rand() % 5);
                d.vy = RAIN_SPEED_MIN + (float)(rand() % 1000) * 0.001f
                     * (RAIN_SPEED_MAX - RAIN_SPEED_MIN);
                g_rain.push_back(d);
            }
        }
        if (g_weather == WEATHER_SNOW && g_flakes.size() < 150) {
            int n = (int)(zw / 30.0f) + 1;
            if (n > 16) n = 16;
            n = (int)(n * g_precip + 0.5f);
            for (int i = 0; i < n; ++i) {
                SnowFlake f;
                f.x   = z.l + (float)(rand() % 1000) * 0.001f * zw;
                f.y   = z.top + (float)(rand() % 5);      // 同样贴云底
                f.vy  = 55.0f + (float)(rand() % 80);
                f.ph  = (float)(rand() % 628) * 0.01f;
                f.amp = 10.0f + (float)(rand() % 16);
                g_flakes.push_back(f);
            }
        }
    }

    // ---- 雨滴：碰到植物补水，落到地面归入积水 ----
    for (size_t i = 0; i < g_rain.size();) {
        RainDrop& d = g_rain[i];
        d.y += d.vy * dt;
        bool hit = false;

        // 只在真正接触的一瞬判定，不把整个轮廓上方的雨都吸掉
        for (size_t pi = 0; pi < pots.size(); ++pi) {
            Pot& p = pots[pi];
            const float halfW = 30.0f * p.scale;
            if (d.x < p.body.x - halfW || d.x > p.body.x + halfW) continue;
            const float topY = p.body.y - POT_H * 0.5f * p.scale
                             - (PlantHeight(p.plant) + 4.0f) * p.scale;
            if (d.y >= topY) {
                p.moisture = Clampf(p.moisture + 0.012f, 0.0f, 1.0f);   // 下雨补水
                SpawnSplash(d.x, d.y, 2);
                hit = true;
                break;
            }
        }
        // 落到地面（积雪上也算）
        if (!hit && d.y >= floorY - g_snowCover - 2.0f) {
            if (g_puddle < PUDDLE_MAX) g_puddle += 0.05f;
            if ((rand() % 3) == 0)
                SpawnSplash(d.x, floorY - g_puddle - g_snowCover, 2);
            hit = true;
        }
        if (hit || d.y > floorY + 20.0f) {
            g_rain[i] = g_rain.back();
            g_rain.pop_back();
        } else {
            ++i;
        }
    }

    // ---- 雪花：慢慢飘，落地加积雪；风大时横向飘得更远 ----
    for (size_t i = 0; i < g_flakes.size();) {
        SnowFlake& f = g_flakes[i];
        f.y += f.vy * dt;
        f.x += sinf(now * 0.9f + f.ph) * f.amp * (0.6f + g_storm * 1.6f) * dt;
        if (f.y >= floorY - g_snowCover) {
            if (g_snowCover < SNOW_MAX) g_snowCover += 0.05f;
            g_flakes[i] = g_flakes.back();
            g_flakes.pop_back();
        } else {
            ++i;
        }
    }

    // ---- 水珠：轻重力，落地或到时间就消失 ----
    for (size_t i = 0; i < g_splash.size();) {
        Splash& s = g_splash[i];
        s.vy += 1500.0f * dt;
        s.x += s.vx * dt;
        s.y += s.vy * dt;
        s.life -= dt;
        if (s.life <= 0.0f || s.y > floorY - g_snowCover) {
            g_splash[i] = g_splash.back();
            g_splash.pop_back();
        } else {
            ++i;
        }
    }

    // ---- 积水 / 积雪演化：雪要化成水再消失，融雪汇入积水 ----
    // 融雪速度按"雪是否接触水"判定：贴着积水就以 0.55/s 快速融化，
    // 与天气无关（雪天贴水的雪也化）；干燥环境才保留晴天慢融。
    float melt = dt * 0.08f;
    if (g_weather == WEATHER_RAIN) melt = dt * 0.55f;
    if (g_puddle > 0.5f)           melt = dt * 0.55f;
    if (melt > g_snowCover) melt = g_snowCover;
    g_snowCover = Clampf(g_snowCover - melt, 0.0f, SNOW_MAX);
    g_puddle    = Clampf(g_puddle + melt * 0.8f, 0.0f, PUDDLE_MAX);

    if (g_weather == WEATHER_RAIN) {
        g_puddle = Clampf(g_puddle + dt * 0.30f, 0.0f, PUDDLE_MAX);
    } else if (g_weather == WEATHER_SNOW) {
        g_puddle = Clampf(g_puddle - dt * 0.12f, 0.0f, PUDDLE_MAX);
    } else {
        g_puddle = Clampf(g_puddle - dt * 0.20f, 0.0f, PUDDLE_MAX);
    }
}

// 地面天气层：积水 + 积雪，按区域各画一段（画在花盆后面，盆底压在水雪之上）
static void DrawWeatherFloor(QPainter& g, const std::vector<Pot>& pots,
                             int winW, float floorY)
{
    RebuildWeatherZones(pots, winW, floorY);
    for (size_t zi = 0; zi < g_zones.size(); ++zi) {
        const WeatherZone& z = g_zones[zi];
        if (z.r <= z.l) continue;

        // 积雪：顶面隔格起伏 + 底部压暗，像自然雪堆
        if (g_snowCover > 0.5f) {
            const QColor snow = C(242, 246, 250);
            const QColor snowShade = C(204, 216, 230);
            const float y = floorY - g_snowCover;
            FillRectF(g, snow, z.l, y, z.r - z.l, g_snowCover);
            for (float x = SnapPx(z.l); x < z.r; x += PIX) {
                const int cx = (int)(x / PIX);
                if (cx & 1)
                    FillRectF(g, snow, x, y - PIX * 0.5f, PIX, PIX * 0.5f);
                if ((cx & 3) == 0)
                    FillRectF(g, snowShade, x, floorY - PIX * 0.5f, PIX, PIX * 0.5f);
            }
        }

        // 积水：半透明水面（最高时也没过雪的下沿），最上面一条高光
        if (g_puddle > 0.5f) {
            const QColor water   = C(64, 138, 198, 150);
            const QColor waterHi = C(188, 226, 250, 180);
            const float y = floorY - g_puddle - g_snowCover * 0.5f;
            FillRectF(g, water,   z.l, y, z.r - z.l, g_puddle);
            FillRectF(g, waterHi, z.l, y, z.r - z.l, PIX * 0.5f);
        }
    }
}

// 每种植物"能积雪的位置"：花盆局部坐标里真实冠层的顶点集合。
// 雪只落在这些点上，不再浮在无形的轮廓框上。新增植物时补几行即可。
static int PlantSnowSpots(PlantType t, SnowSpot* spots, int maxSpots)
{
    const float soilY = -POT_H * 0.5f + 3.0f;    // 土面（与 DrawPlantLocal 一致）
    int n = 0;
    #define ADD_SPOT(px, py, phw) if (n < maxSpots) { spots[n].x = (px); spots[n].y = (py); spots[n].hw = (phw); ++n; }
    switch (t) {
    case PLANT_CACTUS:
        ADD_SPOT(  0.0f, soilY - 100.0f, 10.0f)     // 主干顶
        ADD_SPOT(-22.0f, soilY -  94.0f,  5.0f)     // 左臂顶
        ADD_SPOT( 22.0f, soilY -  78.0f,  5.0f)     // 右臂顶
        break;
    case PLANT_SUNFLOWER:
        ADD_SPOT(0.0f, soilY - 140.0f, 14.0f)       // 花盘顶
        break;
    case PLANT_FLYTRAP:
        ADD_SPOT(0.0f, soilY - 116.0f, 13.0f)       // 颚顶
        break;
    case PLANT_TULIP:
        ADD_SPOT(0.0f, soilY - 122.0f, 13.0f)       // 花苞顶
        break;
    case PLANT_MUSHROOM:
        ADD_SPOT(0.0f, soilY -  76.0f, 12.0f)       // 菌盖顶
        break;
    case PLANT_PINE:
        ADD_SPOT(0.0f, soilY -  72.0f, 20.0f)       // 三层树冠
        ADD_SPOT(0.0f, soilY - 102.0f, 15.0f)
        ADD_SPOT(0.0f, soilY - 130.0f, 11.0f)
        break;
    case PLANT_SUCCULENT:
        ADD_SPOT(0.0f, soilY -  66.0f, 28.0f)       // 莲座顶
        break;
    case PLANT_BAMBOO:
        ADD_SPOT(  0.0f, soilY - 158.0f, 3.0f)      // 三根竹竿顶
        ADD_SPOT(-16.0f, soilY -  98.0f, 3.0f)
        ADD_SPOT( 20.0f, soilY -  61.0f, 3.0f)
        break;
    case PLANT_ROSE:
        ADD_SPOT(0.0f, soilY - 156.0f, 11.0f)       // 花苞顶
        break;
    case PLANT_LAVENDER:
        ADD_SPOT(-22.0f, soilY - 124.0f, 3.0f)      // 四根花穗顶
        ADD_SPOT( -3.0f, soilY - 152.0f, 3.0f)
        ADD_SPOT( 10.0f, soilY -  76.0f, 3.0f)
        ADD_SPOT( 25.0f, soilY - 112.0f, 3.0f)
        break;
    case PLANT_CLOVER:
        ADD_SPOT(-16.0f, soilY - 62.0f, 9.0f)       // 三片心形叶顶
        ADD_SPOT(  0.0f, soilY - 70.0f, 9.0f)
        ADD_SPOT( 16.0f, soilY - 62.0f, 9.0f)
        break;
    case PLANT_CHILI:
        ADD_SPOT(0.0f, soilY - 90.0f, 3.0f)         // 茎顶
        break;
    case PLANT_DANDELION:
        ADD_SPOT(0.0f, soilY - 136.0f, 16.0f)       // 绒球顶
        break;
    case PLANT_DAISY:
        ADD_SPOT(0.0f, soilY - 124.0f, 14.0f)       // 花头顶
        break;
    case PLANT_ALOE:
        ADD_SPOT(  0.0f, soilY - 96.0f, 5.0f)       // 中间最高叶尖
        ADD_SPOT(-38.0f, soilY - 64.0f, 4.0f)       // 左侧叶尖
        ADD_SPOT(  8.0f, soilY - 80.0f, 4.0f)       // 前排叶尖
        ADD_SPOT( 40.0f, soilY - 58.0f, 4.0f)       // 右侧叶尖
        break;
    case PLANT_MORNINGGLORY:
        ADD_SPOT(0.0f, soilY - 162.0f, 14.0f)       // 喇叭口顶
        break;
    case PLANT_CHERRY:
        ADD_SPOT(   0.0f, soilY - 98.0f, 20.0f)     // 主树冠顶
        ADD_SPOT( -26.0f, soilY - 82.0f, 11.0f)     // 左侧树冠
        ADD_SPOT(  26.0f, soilY - 82.0f, 11.0f)     // 右侧树冠
        break;
    default:
        ADD_SPOT(0.0f, soilY - PlantHeight(t) + 6.0f, 10.0f)
        break;
    }
    #undef ADD_SPOT
    return n;
}

// 植物冠层上的积雪：只在真实冠层顶上铺交错白格，雪厚时再叠一行；盆沿再积一条。
// 画在花盆局部坐标系里（随花盆一起旋转、缩放）。
static void DrawSnowCapLocal(QPainter& g, const Pot& p)
{
    if (g_snowCover < 0.8f) return;
    const QColor snow  = C(246, 250, 254);
    const QColor shade = C(204, 216, 230);
    const float lv = Clampf(g_snowCover / SNOW_MAX, 0.0f, 1.0f);

    SnowSpot spots[6];
    const int n = PlantSnowSpots(p.plant, spots, 6);
    for (int i = 0; i < n; ++i) {
        const SnowSpot& s = spots[i];
        const float y = SnapPx(s.y);
        const int cells = (int)(s.hw / PIX) + 1;
        // 贴着冠层顶的一行（隔格留缝，露出下面的植物）
        for (int c = -cells; c <= cells; ++c) {
            if (((c + i) & 1) != 0) continue;
            const QColor& br = ((c & 3) == 0) ? shade : snow;
            FillRectF(g, br, SnapPx(s.x + (float)c * PIX), y - PIX, PIX, PIX);
        }
        // 雪厚时在冠层顶上方再叠一行错缝的
        if (lv > 0.45f) {
            for (int c = -cells + 1; c <= cells - 1; ++c) {
                if (((c + i) & 1) == 0) continue;
                FillRectF(g, snow, SnapPx(s.x + (float)c * PIX), y - PIX * 2.0f, PIX, PIX);
            }
        }
    }

    // 盆沿上的那条雪
    const float rimDepth = PIX * lv;
    FillRectF(g, snow, -POT_W * 0.5f, -POT_H * 0.5f - rimDepth, POT_W, rimDepth);
}

// 天气时的云：一整条云带盖住整个降水区域的落雨面，
// 云底正好压在粒子生成位置（z.top），像整片天空都在下雨下雪。
// 顶部凸包用确定性正弦起伏 + 缓慢漂移，无状态，可直接照搬。
static void DrawClouds(QPainter& g, const std::vector<Pot>& pots,
                       int winW, float floorY, float now)
{
    if (g_precip < 0.02f) return;
    const int alpha = (int)(Clampf(g_precip, 0.0f, 1.0f) * 185.0f);
    const QColor cloud      = C(210, 220, 238, alpha);   // 灰蓝云体
    const QColor cloudLight = C(248, 250, 254, alpha);   // 顶部受光
    const QColor cloudShade = C(168, 180, 202, alpha);   // 底部阴影

    RebuildWeatherZones(pots, winW, floorY);
    for (size_t zi = 0; zi < g_zones.size(); ++zi) {
        const WeatherZone& z = g_zones[zi];
        const float zw = z.r - z.l;
        if (zw < 60.0f) continue;

        const float base = z.top;                        // 云底 = 粒子起点
        FillRectF(g, cloud, z.l, base - 12.0f, zw, 12.0f);
        const int bumps = (int)(zw / 46.0f) + 1;
        for (int i = 0; i <= bumps; ++i) {
            const float bx = z.l + zw * (float)i / (float)bumps
                           + sinf(now * 0.10f + (float)i * 1.7f + (float)zi) * 6.0f;
            const float r = 20.0f + 8.0f * sinf((float)i * 2.4f + (float)zi * 1.3f);
            FillPixelEllipse(g, cloud,      bx, base - 12.0f, r, r * 0.55f);
            FillPixelEllipse(g, cloudLight, bx, base - 16.0f, r * 0.55f, r * 0.30f);
        }
        // 底部阴影条：压出云的厚度
        FillRectF(g, cloudShade, z.l, base - PIX, zw, PIX);
    }
}


#if defined(TBP_HAVE_X11)
// 桌面 / 面板 / 菜单 / 通知这类窗口不能当"地面"，否则花盆会被顶到屏幕最上方。
static bool tbpIsShellWindow(Display* dpy, ::Window w)
{
    if (!dpy || !w) return true;

    Atom wt = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", True);
    if (wt != None) {
        Atom type = None;
        int  fmt = 0;
        unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, w, wt, 0, 16, False, XA_ATOM,
                               &type, &fmt, &n, &after, &data) == Success && data) {
            bool skip = false;
            for (unsigned long i = 0; i < n; ++i) {
                char* nm = XGetAtomName(dpy, ((Atom*)data)[i]);
                if (!nm) continue;
                if (!strcmp(nm, "_NET_WM_WINDOW_TYPE_DESKTOP")       ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_DOCK")          ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_TOOLBAR")       ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_MENU")          ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU") ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_POPUP_MENU")    ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_TOOLTIP")       ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_COMBO")         ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_DND")           ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_SPLASH")        ||
                    !strcmp(nm, "_NET_WM_WINDOW_TYPE_NOTIFICATION"))
                    skip = true;
                XFree(nm);
                if (skip) break;
            }
            XFree(data);
            if (skip) return true;
        }
    }

    Atom st = XInternAtom(dpy, "_NET_WM_STATE", True);
    if (st != None) {
        Atom type = None;
        int  fmt = 0;
        unsigned long n = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, w, st, 0, 16, False, XA_ATOM,
                               &type, &fmt, &n, &after, &data) == Success && data) {
            bool skip = false;
            for (unsigned long i = 0; i < n; ++i) {
                char* nm = XGetAtomName(dpy, ((Atom*)data)[i]);
                if (!nm) continue;
                if (!strcmp(nm, "_NET_WM_STATE_SKIP_TASKBAR") ||
                    !strcmp(nm, "_NET_WM_STATE_SKIP_PAGER")   ||
                    !strcmp(nm, "_NET_WM_STATE_HIDDEN"))
                    skip = true;
                XFree(nm);
                if (skip) break;
            }
            XFree(data);
            if (skip) return true;
        }
    }
    return false;
}
#endif

// 收集当前可站立的窗口。返回的数量即"有多少候选面"。
static void collectWindows(std::vector<WinGeom>& out, int selfId)
{
    out.clear();

    // 首选：KWin 桥（覆盖 Wayland 原生窗口）
    if (g_kwinLive && !g_kwinGeoms.empty()) {
        out = g_kwinGeoms;
        return;
    }

#if defined(TBP_HAVE_X11)
    // 回退：X11 枚举（只有 XWayland 窗口）
    static Display* dpy = nullptr;
    static ::Window root = 0;
    static bool tried = false;
    if (!tried) {
        tried = true;
        dpy = XOpenDisplay(nullptr);
        if (dpy) {
            root = DefaultRootWindow(dpy);
            XSetErrorHandler(tbpXErrorHandler);
        }
    }
    if (!dpy) return;

    Atom listAtom = XInternAtom(dpy, "_NET_CLIENT_LIST", True);
    if (listAtom == None) return;

    Atom type = None;
    int  fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, root, listAtom, 0, 4096, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) != Success || !data) return;

    Window* wins = (Window*)data;
    for (unsigned long k = 0; k < n; ++k) {
        ::Window w = wins[k];
        if (selfId && (int)w == selfId) continue;
        if (tbpIsShellWindow(dpy, w)) continue;

        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, w, &wa)) continue;
        if (wa.map_state != IsViewable) continue;
        if (wa.width <= 0 || wa.height <= 0) continue;

        Window child = 0;
        int ix = 0, iy = 0;
        if (!XTranslateCoordinates(dpy, w, root, 0, 0, &ix, &iy, &child)) continue;

        // client 顶边要减去标题栏高度才是用户看到的窗口上沿
        int frameTop = 0;
        Atom fe = XInternAtom(dpy, "_NET_FRAME_EXTENTS", True);
        if (fe != None) {
            Atom ft = None; int ff = 0;
            unsigned long fn = 0, fa = 0;
            unsigned char* fd = nullptr;
            if (XGetWindowProperty(dpy, w, fe, 0, 4, False, XA_CARDINAL,
                                   &ft, &ff, &fn, &fa, &fd) == Success && fd) {
                if (fn >= 4) frameTop = (int)((long*)fd)[2];
                XFree(fd);
            }
        }

        WinGeom g;
        g.x = (float)ix;
        g.y = (float)(iy - frameTop);
        g.w = (float)wa.width;
        g.h = (float)wa.height;
        out.push_back(g);
    }
    XFree(data);
#else
    (void)selfId;
#endif
}

// =============================================================================
//  承托判定（无状态：每帧在候选窗口里找符合条件的那个）
// =============================================================================
struct SupportHit {
    bool  found = false;
    float top   = 0;    // 窗口外框顶边（绝对 y）
    float left  = 0;
    float width = 0;
    float shadowTop = 0; // 花盆正下方最近的平面 —— 专门给投影用
};

// absCx      花盆中心绝对 x
// prevLowest 上一帧花盆最低点绝对 y
// lowest     这一帧花盆最低点绝对 y
// fallback   没有任何窗口时的地面（屏幕工作区底边）
static SupportHit findSupport(float absCx, float prevLowest, float lowest,
                              float fallback, int selfId)
{
    std::vector<WinGeom> wins;
    collectWindows(wins, selfId);

    SupportHit hit;
    if (wins.empty()) {
        hit.top = fallback;
        hit.shadowTop = fallback;
        return hit;
    }

    // ---- 投影用：花盆正下方最近的那层平面（不考虑"接住"，只看谁在下面）----
    float shadowBest = fallback;
    for (const WinGeom& g : wins) {
        if (absCx < g.x || absCx >= g.x + g.w) continue;
        if (g.y < lowest - 2.0f) continue;      // 必须在花盆最低点下方（或齐平）
        if (g.y < shadowBest) shadowBest = g.y;
    }
    hit.shadowTop = shadowBest;

    float bestTop = 0.0f;
    for (const WinGeom& g : wins) {
        if (absCx < g.x || absCx >= g.x + g.w) continue;   // 水平方向不覆盖

        float top = g.y;
        if (top > lowest + 4.0f) continue;                 // 顶边在花盆下方
        if (top < prevLowest - 4.0f) continue;             // 上一帧就已经在它下面了

        if (!hit.found || top < bestTop) {
            bestTop = top;
            hit.found = true;
            hit.top = top;
            hit.left = g.x;
            hit.width = g.w;
        }
    }

    if (!hit.found) hit.top = fallback;

    // ---- 诊断：每秒输出一行，拖花盆时能看清判定过程 ----
    if (qEnvironmentVariableIsSet("TBP_TRACE")) {
        static int tick = 0;
        if ((tick++ % 60) == 0) {
            QString s = QStringLiteral("support: n=%1 kwin=%2 cx=%3 prev=%4 cur=%5")
                            .arg(wins.size())
                            .arg(g_kwinLive ? 1 : 0)
                            .arg((int)absCx)
                            .arg((int)prevLowest)
                            .arg((int)lowest);
            for (size_t i = 0; i < wins.size() && i < 6; ++i) {
                s += QStringLiteral(" |%1,%2 %3x%4%5%6%7")
                         .arg((int)wins[i].x).arg((int)wins[i].y)
                         .arg((int)wins[i].w).arg((int)wins[i].h)
                         .arg(absCx >= wins[i].x && absCx < wins[i].x + wins[i].w ? "H" : ".")
                         .arg(wins[i].y <= lowest + 4.0f ? "" : "L")
                         .arg(wins[i].y >= prevLowest - 4.0f ? "" : "A");
            }
            s += QStringLiteral(" => %1 %2")
                     .arg(hit.found ? "HIT" : "miss")
                     .arg((int)hit.top);
            tbpLog(s);
        }
    }
    return hit;
}



static void SpawnDust(std::vector<Crumb>& crumbs, float x, float y, int count)
{
    if (crumbs.size() > 150) return;
    for (int i = 0; i < count; ++i) {
        Crumb c;
        c.x = x + (float)((rand() % 41) - 20) * g_scale;
        c.y = y - (float)(rand() % 4) * g_scale;
        c.vx = (float)((rand() % 240) - 120) * g_scale;
        c.vy = -((float)(rand() % 260) + 40.0f) * g_scale;
        c.life = 0.45f + (float)(rand() % 30) / 100.0f;
        c.floorY = y;
        crumbs.push_back(c);
    }
}

// =============================================================================
//  四、四盆窗口
// =============================================================================
class Garden;

class PotWindow : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal fade READ fade WRITE setFade)

public:
    explicit PotWindow(Garden* garden, QWidget* parent = nullptr);

    qreal fade() const { return m_fade; }
    void  setFade(qreal f) { m_fade = f; update(); }

    void fadeOutAndClose();

    // 缩放变了要整屏重画：旧尺寸留下的像素不在新脏矩形里
    void requestFullRepaint() { m_lastDirty = QRect(); update(); }

    // 构建菜单但不弹出，返回它的建议尺寸。--probe-menu 用来在无头环境下
    // 验证双列布局没算错，不需要真的右键。
    QSize probeMenuSize();
    void  populateMenu(QMenu& menu, int pot, int* chosenOut);

    // 每帧根据花盆包围盒重设窗口区域：
    //   · setMask()            -> 裁剪"绘制"范围（X11 的 ShapeBounding）
    //   · applyInputShape()    -> 裁剪"鼠标接收"范围（X11 的 ShapeInput）
    // 两个必须分开设：Qt 的 setMask 在 xcb 后端只改 bounding，
    // input shape 仍是整窗，所以鼠标会被全屏窗口挡住 —— 必须自己调 XShape。
    void updateInputMask();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    void applyInputShape(const QRegion& region);

    Garden* m_garden = nullptr;
    qreal   m_fade = 1.0;
    QPropertyAnimation* m_anim = nullptr;
    QRegion m_lastMask;
    QRect   m_lastDirty;

#if defined(TBP_HAVE_X11)
    Display* m_xdpy = nullptr;          // 独立的 Xlib 连接，只用于设 input shape
    int    m_oldErrHandler = 0;
#endif
};

// =============================================================================
//  五、场景管理
// =============================================================================
class Garden
{
public:
    Garden();
    ~Garden();

    void start();
    void tick();

    float floorY() const { return m_floorY; }
    int   winW()   const { return m_winW; }
    int   winH()   const { return m_winH; }

    std::vector<Pot>& pots() { return m_pots; }
    std::vector<Crumb>& crumbs() { return m_crumbs; }
    std::vector<Crumb>& wilt()   { return m_wilt; }
    PotWindow* window() const { return m_win; }
    float now() const { return m_time; }

    void spawnPot(float x, float y, PlantType plant);
    void applyMenuChoice(int idx, float mx, float my);

    int  pickPot(float mx, float my, bool wantPlant, int* hitIdx);
    void beginDrag(int idx, float mx, float my);
    void dragTo(float mx, float my);
    void endDrag();

    int  activePot() const { return m_activePot; }
    void setActivePot(int i) { m_activePot = i; }

    // PotWindow 每帧把需要重绘的矩形登记进来，tick 末尾统一提交
    void markDirty(const QRect& r) { m_dirty = m_dirty.united(r); }
    QRect takeDirty() { QRect r = m_dirty; m_dirty = QRect(); return r; }

    void quitWithFade();

    // ---- 隐藏调试模式（上游 v3.8.2）----
    //
    // 菜单底部版本号连点 7 次开启。开启后弹出一个独立的小工具条窗口，
    // 里面是"干枯 / 下雨 / 下雪 / 关闭调试模式"。
    void enableDebugMode();
    void disableDebugMode();
    void doDebugAction(int action);

    bool debugMode() const { return m_debugMode; }
    void debugDryAll();

    // ---- 编辑模式（上游 v3.5）：场景改动只在内存里，点「保存」才写盘 ----
    bool editMode() const { return m_editMode; }
    void setEditMode(bool on);
    void editAddPot();          // + 新花盆
    void editCycleDry();        // 干枯时长档位循环
    void editSave();            // 把当前场景固化成布局快照
    void saveLayout(bool manual);   // 写 ini；manual=false 只回写水分

private:
    void updatePot(Pot& p, float dt);
    void updatePhysics(float dt);

    PotWindow* m_win = nullptr;
    std::vector<Pot>  m_pots;
    std::vector<Crumb> m_crumbs;
    std::vector<Crumb> m_wilt;      // 枯叶碎屑（缺水时从植株上掉落）

    int   m_activePot = -1;
    float m_time = 0.0f;

    int   m_winX = 0, m_winY = 0, m_winW = 0, m_winH = 0;
    float m_floorY = 0.0f;

    bool m_layoutReady = false;
    QRect m_dirty;

    // 布局快照：ini 里写的就是它。自动保存只改各槽位的 moisture。
    std::vector<PotSeed> m_savedLayout;

    // 隐藏调试模式：工具条是个独立顶层窗口，不塞进全屏穿透层
    bool      m_debugMode = false;
    QWidget*  m_dbgBar    = nullptr;

    // 编辑模式
    bool      m_editMode   = false;
    QWidget*  m_editBar    = nullptr;
    QLabel*   m_editStatus = nullptr;
    QToolButton* m_dryBtn  = nullptr;
    float     m_saveTimer  = 0.0f;
};

// -----------------------------------------------------------------------------
//  PotWindow
// -----------------------------------------------------------------------------
PotWindow::PotWindow(Garden* garden, QWidget* parent)
    : QWidget(parent), m_garden(garden)
{
    // 注意：这里必须用 Qt::Window 而不是 Qt::Tool。
    //
    // Qt 判断"最后一个主窗口是否关闭"时，Tool / Popup 类型的顶层窗口会被跳过。
    // 用 Tool 的话，打开导出对话框、再关掉菜单这一瞬间，Qt 会认为已经没有
    // primary window，于是触发 lastWindowClosed 并且整个应用退出 ——
    // 即使 setQuitOnLastWindowClosed(false) 也照样退。
    // 改成 Window + Dock 类型：算主窗口（不会误判），Dock 又不进任务栏。
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Window
                   | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_X11NetWmWindowTypeDock, true);   // 不进任务栏
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setWindowTitle(QStringLiteral("桌面电子盆栽"));
}

void PotWindow::fadeOutAndClose()
{
    if (m_anim) return;
    m_anim = new QPropertyAnimation(this, "fade", this);
    m_anim->setDuration(420);
    m_anim->setStartValue(1.0);
    m_anim->setEndValue(0.0);
    m_anim->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_anim, &QPropertyAnimation::finished, qApp, &QApplication::quit);
    m_anim->start();
}

void PotWindow::applyInputShape(const QRegion& region)
{
#if defined(TBP_HAVE_X11)
    if (!m_xdpy) {
        m_xdpy = XOpenDisplay(nullptr);
        if (!m_xdpy) return;
        m_oldErrHandler = 1;
        XSetErrorHandler(tbpXErrorHandler);
    }

    ::Window wid = (::Window)winId();
    if (!wid) return;

    QList<QRect> rl;
    for (const QRect& r : region) rl.append(r);

    std::vector<XRectangle> xr((size_t)rl.size());
    for (int i = 0; i < rl.size(); ++i) {
        xr[(size_t)i].x = (short)rl[i].x();
        xr[(size_t)i].y = (short)rl[i].y();
        xr[(size_t)i].width  = (unsigned short)qMax(0, rl[i].width());
        xr[(size_t)i].height = (unsigned short)qMax(0, rl[i].height());
    }

    // ShapeSet + 0 个矩形 = 整窗都不接收鼠标（完全穿透）
    XShapeCombineRectangles(m_xdpy, wid, ShapeInput, 0, 0,
                            xr.empty() ? nullptr : xr.data(),
                            (int)xr.size(), ShapeSet, Unsorted);
    XFlush(m_xdpy);
#else
    (void)region;
#endif
}

void PotWindow::updateInputMask()
{
    if (!m_garden) return;

    // ------------------------------------------------------------------------
    //  精确输入区域：跑一遍绘制逻辑（只收集矩形、不真正落笔），得到"有像素"的范围。
    //  悬浮层不设 bounding shape —— 那会裁掉重绘、留下残影。
    // ------------------------------------------------------------------------
    QImage probe(1, 1, QImage::Format_ARGB32_Premultiplied);
    probe.fill(Qt::transparent);
    QPainter pg(&probe);
    pg.setRenderHint(QPainter::Antialiasing, false);

    QRegion region;

    const float hh = POT_H * 0.5f;
    const std::vector<Pot>& pots = m_garden->pots();

    g_capRegion = &region;
    g_capOnly = true;
    for (size_t i = 0; i < pots.size(); ++i) {
        const Pot& p = pots[i];
        const Body& b = p.body;
        const float phh1 = POT_H * 0.5f * p.scale;
        pg.save();
        pg.translate(b.x, b.y - phh1);
        pg.rotate(b.angle * 57.2957795f);
        g_scale = g_baseScale * p.scale;
        DrawPlantLocal(pg, -phh1 + 3.0f, p.plant, p.sway, p.poking, m_garden->now());
        DrawPotLocal(pg);
        g_scale = g_baseScale;
        pg.restore();
    }
    g_capRegion = nullptr;
    g_capOnly = false;
    pg.end();

    region = region.intersected(rect());

    // ------------------------------------------------------------------------
    //  当前帧需要重绘的范围
    //
    //  除了花盆本身，还必须包含"投影"—— 投影画在承托面上（surfaceY-6），
    //  花盆被拖到半空时投影在很下面，跟花盆的包围盒完全不重叠。
    //  只按花盆算脏矩形的话，投影永远清不掉，移动时会拖出一串黑的残影。
    // ------------------------------------------------------------------------
    QRect cur = region.isEmpty() ? QRect() : region.boundingRect().adjusted(-4, -4, 4, 4);

    for (size_t i = 0; i < pots.size(); ++i) {
        const Pot& p = pots[i];
        const Body& b = p.body;

        // 投影的最大范围（取最宽的情况，lift=0 时 92*scale 宽、上下各 12px）
        float swHalf = (92.0f * g_scale) * 0.5f + 4.0f;
        QRect shadowRc((int)floorf(b.x - swHalf),
                       (int)floorf(p.shadowY - 14.0f),
                       (int)ceilf(swHalf * 2.0f) + 2,
                       20);
        cur = cur.united(shadowRc);
    }

    if (!m_garden->crumbs().empty()) {
        for (const Crumb& c : m_garden->crumbs())
            cur = cur.united(QRect((int)c.x - 10, (int)c.y - 10, 20, 20));
    }
    if (!m_garden->wilt().empty()) {
        for (const Crumb& c : m_garden->wilt())
            cur = cur.united(QRect((int)c.x - 8, (int)c.y - 8, 16, 16));
    }

    // ---- 天气：粒子、地面水雪、云都在动，每帧都要重绘 ----
    if (!g_rain.empty()) {
        for (const RainDrop& d : g_rain)
            cur = cur.united(QRect((int)d.x - 6, (int)d.y - 8, 14, 24));
    }
    if (!g_flakes.empty()) {
        for (const SnowFlake& f : g_flakes)
            cur = cur.united(QRect((int)f.x - 8, (int)f.y - 8, 16, 16));
    }
    if (!g_splash.empty()) {
        for (const Splash& s : g_splash)
            cur = cur.united(QRect((int)s.x - 8, (int)s.y - 8, 16, 16));
    }
    if (g_puddle > 0.5f || g_snowCover > 0.5f) {
        RebuildWeatherZones(pots, m_garden->winW(), m_garden->floorY());
        for (const WeatherZone& z : g_zones) {
            cur = cur.united(QRect((int)z.l - 4,
                                   (int)(m_garden->floorY() - g_snowCover - g_puddle - 8.0f),
                                   (int)(z.r - z.l) + 8,
                                   (int)(g_snowCover + g_puddle) + 16));
        }
    }
    if (g_precip > 0.02f) {
        RebuildWeatherZones(pots, m_garden->winW(), m_garden->floorY());
        for (const WeatherZone& z : g_zones) {
            // 云横向会伸出区域：凸包中心最远偏到 z.l-6 / z.r+6，半径最大 28
            // → 左右各留 40 才盖得住。只留 4px 的话，露在外面的云瓣永远
            // 进不了脏矩形，雨停后那几块就一直挂在那儿褪不掉。
            cur = cur.united(QRect((int)z.l - 40, (int)z.top - 46,
                                   (int)(z.r - z.l) + 80, 50));
        }
    }

    cur = cur.intersected(rect());

    QRect dirty = cur;
    if (!m_lastDirty.isEmpty()) dirty = dirty.united(m_lastDirty);
    m_lastDirty = cur;

    if (dirty.isValid() && !dirty.isEmpty())
        m_garden->markDirty(dirty);

    if (region != m_lastMask) {
        m_lastMask = region;
        applyInputShape(region);

        if (qEnvironmentVariableIsSet("TBP_DEBUG")) {
            QFile lf("/tmp/tbp_mask.txt");
            if (lf.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                QTextStream ts(&lf);
                ts << "inputRects=" << region.rectCount()
                   << " bounding=" << region.boundingRect().x() << ","
                   << region.boundingRect().y() << " "
                   << region.boundingRect().width() << "x"
                   << region.boundingRect().height() << "\n";
                ts.flush();
            }
        }
    }
}


void PotWindow::paintEvent(QPaintEvent* ev)
{
    if (!m_garden) return;

    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing, false);
    g.setRenderHint(QPainter::SmoothPixmapTransform, false);
    g.setRenderHint(QPainter::TextAntialiasing, true);

    // 关键：先把本次要重绘的矩形整块清成透明。
    // 不清的话，上一帧画在这个位置的花盆像素会留在 backing store 里，
    // 表现为快速移动时的残影，以及"拖过去把下面的窗口内容擦掉"。
    g.setCompositionMode(QPainter::CompositionMode_Source);
    g.fillRect(ev->rect(), Qt::transparent);
    g.setCompositionMode(QPainter::CompositionMode_SourceOver);

    g.setOpacity(m_fade);

    const float hh = POT_H * 0.5f;

    const std::vector<Pot>& pots = m_garden->pots();
    const int   winW   = m_garden->winW();
    const float floorY = m_garden->floorY();
    const float now    = m_garden->now();

    // 云画在最底层：一整条云带盖住降水区域，云底压在粒子起点
    DrawClouds(g, pots, winW, floorY, now);

    // 雨线画在花盆后面：雨从植物身后落下，碰到植物的一瞬才溅起
    if (!g_rain.empty()) {
        const QColor rain = C(120, 176, 228, 220);
        for (size_t i = 0; i < g_rain.size(); ++i)
            FillRectF(g, rain, g_rain[i].x, g_rain[i].y, PIX, PIX * 2.5f);
    }

    // 泥土碎屑
    const std::vector<Crumb>& crumbs = m_garden->crumbs();
    if (!crumbs.empty()) {
        const QColor crumb = C(96, 66, 42);
        for (size_t i = 0; i < crumbs.size(); ++i)
            FillRectF(g, crumb, crumbs[i].x, crumbs[i].y, 3.0f * g_scale, 3.0f * g_scale);
    }

    // 天气地面层：积水 + 积雪（画在花盆后面，盆底会压在水雪之上）
    DrawWeatherFloor(g, pots, winW, floorY);

    // 枯叶碎屑
    const std::vector<Crumb>& wilt = m_garden->wilt();
    if (!wilt.empty()) {
        const QColor leafA = C(176, 146, 60);
        const QColor leafB = C(132, 100, 42);
        for (size_t i = 0; i < wilt.size(); ++i)
            FillRectF(g, (i & 1) ? leafA : leafB, wilt[i].x, wilt[i].y, PIX, PIX);
    }

    for (size_t i = 0; i < pots.size(); ++i) {
        const Pot& p = pots[i];
        const Body& b = p.body;

        float lowest = b.y - hh + PotMaxY(b.angle);

        // 承托面上的投影：只有拿在手里 / 落在半空时才画（v3.8），
        // 稳稳放在任务栏或窗口标题栏上不再显示。
        // 落点仍用 shadowY（正下方最近的平面），不然投影会跑到屏幕底部的面板上。
        if (lowest < p.surfaceY - 8.0f) {
            float lift = Clampf((p.shadowY - lowest) / 160.0f, 0.0f, 1.0f);
            float sw = (92.0f - lift * 44.0f) * g_scale;
            int   sa = (int)(100.0f - lift * 70.0f);
            FillPixelEllipse(g, C(0, 0, 0, sa), b.x, p.shadowY - 6.0f, sw * 0.5f, 4.0f);
        }

        // 花盆 + 植物共用同一个变换。单盆缩放通过临时改 g_scale 实现，
        // 这样 DrawPotLocal / DrawPlantLocal 都不必改签名 —— 它们内部全是 PIX 的倍数。
        const float phh = POT_H * 0.5f * p.scale;
        const float savedScale = g_scale;
        const float savedWilt  = g_wiltAmount;

        g_scale = g_baseScale * p.scale;
        g_wiltAmount = (g_wiltEnabled && p.moisture < DRY_THRESHOLD)
                       ? (DRY_THRESHOLD - p.moisture) / DRY_THRESHOLD
                       : 0.0f;

        g.save();
        g.translate(b.x, b.y - phh);
        g.rotate(b.angle * 57.2957795f);
        DrawPlantLocal(g, -phh + 3.0f, p.plant, p.sway, p.poking, now);
        DrawPotLocal(g);
        DrawSnowCapLocal(g, p);      // 植物冠层 / 盆沿上的积雪
        g.restore();

        g_scale = savedScale;
        g_wiltAmount = savedWilt;
    }

    // 天气粒子：雪花 / 水珠（雨线已在下层画过）
    if (!g_flakes.empty()) {
        const QColor flake = C(240, 246, 252);
        for (size_t i = 0; i < g_flakes.size(); ++i)
            FillRectF(g, flake, g_flakes[i].x, g_flakes[i].y, PIX, PIX);
    }
    if (!g_splash.empty()) {
        const QColor drop = C(150, 208, 244, 210);
        for (size_t i = 0; i < g_splash.size(); ++i)
            FillRectF(g, drop, g_splash[i].x, g_splash[i].y, PIX, PIX);
    }
}

static float clampToWin(float v, float lo, float hi) { return Clampf(v, lo, hi > lo ? hi : lo); }

void PotWindow::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { e->ignore(); return; }
    QPointF pos = e->position();
    float mx = (float)pos.x();
    float my = (float)pos.y();

    int idx = -1;
    int pot = m_garden->pickPot(mx, my, false, &idx);
    if (pot >= 0) {
        m_garden->beginDrag(pot, mx, my);
        e->accept();
        return;
    }

    idx = -1;
    pot = m_garden->pickPot(mx, my, true, &idx);
    if (pot >= 0) {
        Pot& p = m_garden->pots()[pot];
        m_garden->setActivePot(pot);
        p.poking = true;
        p.body.dragged = false;
        p.swayVel = 0.0f;
        e->accept();
        return;
    }
    e->ignore();
}

void PotWindow::mouseMoveEvent(QMouseEvent* e)
{
    QPointF pos = e->position();
    m_garden->dragTo((float)pos.x(), (float)pos.y());
    e->accept();
}

void PotWindow::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { e->ignore(); return; }
    m_garden->endDrag();
    e->accept();
}

// 菜单内容（植物两列 + 命令单列）。chosenOut 在用户点选后写入编号，
// 不点选则保持 -2。抽成独立函数是为了 --probe-menu 能在无头环境下量尺寸。
void PotWindow::populateMenu(QMenu& menu, int pot, int* chosenOut)
{
    const int kCols = 2;
    const int kCellW = 112, kCellH = 30, kPad = 8;
    const int kPanelW = kCellW * kCols + kPad * 2;

    static const QColor kDots[PLANT_COUNT] = {
        C(78, 154, 78), C(247, 197, 49), C(150, 60, 70),
        C(226, 78, 106), C(212, 62, 62), C(46, 116, 66),
        C(126, 180, 134), C(150, 194, 96), C(206, 52, 78),
        C(152, 106, 202), C(96, 176, 76), C(212, 58, 46),
        C(240, 240, 226),
        C(244, 196, 58), C(96, 168, 118), C(108, 122, 214), C(244, 178, 196)
    };
    auto dotIcon = [](const QColor& col) {
        QPixmap pm(10, 10);
        pm.fill(Qt::transparent);
        QPainter pp(&pm);
        pp.fillRect(QRect(0, 0, 10, 10), col);
        pp.end();
        return QIcon(pm);
    };

    menu.setWindowFlags(menu.windowFlags() | Qt::FramelessWindowHint
                        | Qt::NoDropShadowWindowHint);
    menu.setAttribute(Qt::WA_TranslucentBackground, true);
    menu.setStyleSheet("QMenu { background: transparent; border: none; padding: 0px; }");

    QWidget* panel = new QWidget;
    panel->setObjectName("menuPanel");
    panel->setFixedWidth(kPanelW);
    panel->setStyleSheet(QString(
        "#menuPanel {"
        "  background-color: rgb(38,26,18);"
        "  border: 2px solid rgb(198,120,68);"
        "}"
        "QToolButton {"
        "  background: transparent;"
        "  color: rgb(216,188,152);"
        "  border: none;"
        "  text-align: left;"
        "  padding-left: 5px;"
        "  font-size: 13px;"
        "}"
        "QToolButton:hover {"
        "  background-color: rgb(122,74,42);"
        "  color: rgb(255,240,210);"
        "}"
        "QToolButton:disabled { color: rgb(105,88,72); }"
        "QFrame { color: rgb(120,74,42); }"
    ));

    QVBoxLayout* v = new QVBoxLayout(panel);
    v->setContentsMargins(kPad, kPad, kPad, kPad);
    v->setSpacing(0);

    QFont mf = panel->font();
    mf.setPixelSize(13);
    mf.setFamily(QStringLiteral("Noto Sans CJK SC"));
    panel->setFont(mf);

    auto mkBtn = [&](const QString& text, const QColor& dot) {
        QToolButton* b = new QToolButton(panel);
        b->setText(QStringLiteral(" ") + text);
        b->setIcon(dotIcon(dot));
        b->setIconSize(QSize(10, 10));
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        b->setAutoRaise(true);
        b->setFixedHeight(kCellH);
        b->setFont(mf);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };

    auto bind = [&](QToolButton* b, int id) {
        connect(b, &QToolButton::clicked, [&menu, chosenOut, id]() {
            *chosenOut = id;
            menu.close();
        });
    };

    QWidget* plantBox = new QWidget(panel);
    QGridLayout* grid = new QGridLayout(plantBox);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    for (int i = 0; i < PLANT_COUNT; ++i) {
        QToolButton* b = mkBtn(QString::fromUtf8(kPlantNames[i]), kDots[i]);
        bind(b, i);
        grid->addWidget(b, i / kCols, i % kCols);
    }
    v->addWidget(plantBox);

    QFrame* line = new QFrame(panel);
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(9);
    v->addWidget(line);

    struct Cmd { int id; const char* text; };
    const Cmd cmds[] = {
        { MENU_CMD_WATER,      "浇水" },
        { MENU_CMD_EDIT,       "编辑模式" },
        { MENU_CMD_SCALE_UP,   "花盆调大" },
        { MENU_CMD_SCALE_DOWN, "花盆调小" },
        { MENU_CMD_SPAWN,      "放置新花盆" },
        { MENU_CMD_REMOVE,     "移除这个花盆" },
        { MENU_CMD_EXPORT,     "导出植物图片" },
        { MENU_CMD_QUIT,       "退出程序" },
    };
    const QColor grey = C(150, 150, 150);
    for (const Cmd& c : cmds) {
        QToolButton* b = mkBtn(QString::fromUtf8(c.text), grey);
        bind(b, c.id);

        const bool needsPot = (c.id == MENU_CMD_WATER
                            || c.id == MENU_CMD_SCALE_UP
                            || c.id == MENU_CMD_SCALE_DOWN
                            || c.id == MENU_CMD_EXPORT);
        if (needsPot && pot < 0) b->setEnabled(false);
        if (c.id == MENU_CMD_REMOVE)
            b->setEnabled(pot >= 0 && m_garden->pots().size() > 1);

        v->addWidget(b);
    }

    // ---- 底部版本号（上游 v3.8.2）----
    //
    // 这一项不关菜单：连点 7 次才能开出隐藏调试模式，中间关掉菜单就数不满了。
    // 计数窗口 3 秒，静默无提示。点满 7 次后调用方负责关菜单并开面板。
    //
    // 不用 mkBtn()：那个会给每一项配一个左侧色块图标、文本左对齐，
    // 版本号是一行脚注，居中、无图标、更小更灰才对。
    {
        QPushButton* b = new QPushButton(VersionText(), panel);
        b->setObjectName(QStringLiteral("versionLabel"));
        b->setFlat(true);
        b->setFixedHeight(22);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);   // 别抢菜单的键盘焦点
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        connect(b, &QPushButton::clicked, [this, &menu, chosenOut]() {
            // 静默计数：7 次点完才跳出循环，中间任何提示都不给
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - g_versionLastClick > kVersionClickWindowMs) g_versionClicks = 0;
            g_versionLastClick = now;
            if (++g_versionClicks >= kVersionClicksToDebug) {
                g_versionClicks = 0;
                *chosenOut = MENU_CMD_VERSION;   // 让 exec() 返回后由调用方开面板
                menu.close();
            }
        });
        v->addWidget(b);

        panel->setStyleSheet(panel->styleSheet() + QStringLiteral(
            "#versionLabel {"
            "  background: transparent;"
            "  border: none;"
            "  color: rgb(120, 110, 98);"
            "  font-size: 11px;"
            "  text-align: center;"
            "  padding: 0px;"
            "}"
            "#versionLabel:hover { color: rgb(168, 152, 132); }"));
    }

    QWidgetAction* wa = new QWidgetAction(&menu);
    wa->setDefaultWidget(panel);
    menu.addAction(wa);
}

QSize PotWindow::probeMenuSize()
{
    QMenu menu(this);
    int chosen = -2;
    populateMenu(menu, 0, &chosen);
    QSize sz = menu.sizeHint();
    if (sz.isEmpty()) sz = menu.geometry().size();

    // TBP_MENU_PNG=<path>：把菜单渲染成图片，用来无头检查字体/对齐/配色
    const QByteArray png = qgetenv("TBP_MENU_PNG");
    if (!png.isEmpty() && !sz.isEmpty()) {
        menu.resize(sz);
        menu.grab().save(QString::fromLocal8Bit(png));
    }
    return sz;
}

void PotWindow::contextMenuEvent(QContextMenuEvent* e)
{
    QPointF pos = e->pos();
    float mx = (float)pos.x();
    float my = (float)pos.y();

    // 右键点中哪个花盆就操作哪个；点空白处只保留"放置新花盆 / 退出"
    int idx = -1;
    int pot = m_garden->pickPot(mx, my, false, &idx);
    if (pot < 0) {
        idx = -1;
        pot = m_garden->pickPot(mx, my, true, &idx);
    }
    m_garden->setActivePot(pot);

    QMenu menu(this);
    int chosen = -2;
    populateMenu(menu, pot, &chosen);
    menu.exec(e->globalPos());
    if (chosen != -2) m_garden->applyMenuChoice(chosen, mx, my);
    e->accept();
}

// -----------------------------------------------------------------------------
//  交互式导出：弹保存对话框后把当前花盆写成 PNG
//
//  两个坑：
//   1. 父窗口必须给 nullptr。PotWindow 是全屏透明穿透窗口，拿它当父窗口时
//      对话框会被它的输入区域/置顶属性牵连，表现是弹不出来或者一闪就没。
//   2. 不能用系统原生对话框。KDE 下 Qt 会转给 xdg-desktop-portal，
//      portal 不可用时进程直接退出，终端里连错误都没有。
// -----------------------------------------------------------------------------
static void exportPotInteractive(Pot& pot, QWidget* parent)
{
    const QString name = QString::fromUtf8(kPlantNames[pot.plant])
                       + QStringLiteral(".png");

    tbpLog(QStringLiteral("export: opening dialog"));

    // 用实例而不是静态函数，才能强制置顶。PotWindow 是 WindowStaysOnTop 的
    // 全屏窗口，普通对话框会被它压在下面 —— 表现就是"点了导出没反应"。
    QFileDialog dlg(parent, QStringLiteral("导出植物图片"),
                    QDir::homePath() + QStringLiteral("/") + name,
                    QStringLiteral("PNG 图片 (*.png)"));
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    dlg.setDefaultSuffix(QStringLiteral("png"));
    dlg.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    dlg.setWindowModality(Qt::WindowModal);

    if (dlg.exec() != QDialog::Accepted) {
        tbpLog(QStringLiteral("export: cancelled"));
        return;
    }
    QString path = dlg.selectedFiles().value(0);
    tbpLog(QStringLiteral("export: chosen '%1'").arg(path));

    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");

    const bool ok = ExportPotPng(pot, path);
    tbpLog(QStringLiteral("export: save %1 -> %2").arg(ok ? QStringLiteral("ok")
                                                          : QStringLiteral("FAILED"),
                                                       path));
}

// -----------------------------------------------------------------------------
//  Garden
// -----------------------------------------------------------------------------
Garden::Garden() {}

Garden::~Garden() {}

// -----------------------------------------------------------------------------
//  探测桌面面板（任务栏）占用的边
//
//  Wayland 下 QScreen::availableGeometry() 往往等于整块屏幕 —— 面板是 shell
//  自己合成的一层表面，普通客户端拿不到它的尺寸。所以这里按桌面环境自己算：
//    · KDE Plasma：读 ~/.config/plasmashellrc 的 panelHeight / 面板 thickness，
//      以及 plasma-org.kde.plasma.desktop-appletsrc 里的 location。
//    · GNOME：读 org.gnome.shell.extensions.dash-to-dock 或默认顶栏 32px。
//    · 其它：查环境变量 / 常见任务栏高度，取保守值。
//  location: 0=上 1=上 2=下 3=左 4=下（Plasma 的 Panel location 枚举）
// -----------------------------------------------------------------------------
struct PanelInset { int top = 0, bottom = 0, left = 0, right = 0; };

static int readIniKey(const QString& path, const QString& group, const QString& key, int defVal)
{
    QSettings s(path, QSettings::IniFormat);
    s.beginGroup(group);
    bool ok = false;
    int v = s.value(key, defVal).toInt(&ok);
    s.endGroup();
    return ok ? v : defVal;
}

static PanelInset detectPanelInset(const QRect& screenGeom)
{
    PanelInset in;
    const QString home = QDir::homePath();

    // ---- KDE Plasma ----
    const QString plasmaRc = home + "/.config/plasmashellrc";
    const QString appletsRc = home + "/.config/plasma-org.kde.plasma.desktop-appletsrc";
    if (QFile::exists(plasmaRc) || QFile::exists(appletsRc)) {
        // 面板厚度：plasmashellrc 里 [PlasmaViews] 或 [PlasmaViews][Panel N] 下的 thickness
        int thickness = 0;
        {
            QSettings ps(plasmaRc, QSettings::IniFormat);
            int t = ps.value("PlasmaViews/thickness", 0).toInt();
            if (t > thickness) thickness = t;
            for (const QString& g : ps.childGroups()) {
                if (!g.startsWith("PlasmaViews")) continue;
                int t2 = ps.value(g + "/thickness", 0).toInt();
                if (t2 > thickness) thickness = t2;
                // 细分到 Horizontal/Vertical 子组
                for (const QString& sub : ps.childGroups()) {
                    if (!sub.startsWith(g + "/")) continue;
                    int t3 = ps.value(sub + "/thickness", 0).toInt();
                    if (t3 > thickness) thickness = t3;
                }
            }
        }
        if (thickness <= 0) thickness = 46;   // Plasma Breeze 默认面板厚度

        // 面板位置：优先 appletsrc 里 org.kde.panel 容器的 location，
        // 取不到时按 "底部面板" 处理（Plasma 的默认布局）
        int location = -1;
        {
            QSettings as(appletsRc, QSettings::IniFormat);
            for (const QString& g : as.childGroups()) {
                if (!g.startsWith("Containments")) continue;
                if (as.value(g + "/plugin").toString() != "org.kde.panel") continue;
                location = as.value(g + "/location", -1).toInt();
                if (location >= 0) break;
            }
        }
        // location: 0/1=上 2/3=下 4=左 5=右（Plasma 6）；兼容旧值 3=左 4=下
        switch (location) {
        case 0: case 1: in.top = thickness; break;
        case 4: in.bottom = thickness; break;
        case 3: in.left = thickness; break;
        case 5: in.right = thickness; break;
        default: in.bottom = thickness; break;   // 未读到就按底部
        }

        if (in.top || in.bottom || in.left || in.right) return in;
    }

    // ---- GNOME（顶栏 + 可选 dash）----
    const QString gnomeShell = home + "/.config/gnome-shell/gnome-shell.ini";
    if (QFile::exists(gnomeShell) || QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP"))
                                        .contains("GNOME", Qt::CaseInsensitive)) {
        int topBar = readIniKey(gnomeShell, "org/gnome/shell", "panel-height", 32);
        if (topBar <= 0) topBar = 32;
        in.top = topBar;
        // dash-to-dock 底部时再补一条
        const QString dtd = home + "/.config/dash-to-dock.ini";
        if (QFile::exists(dtd)) {
            QSettings d(dtd, QSettings::IniFormat);
            if (d.value("dock-position").toString() == "BOTTOM") {
                int h = d.value("dash-max-icon-size", 48).toInt();
                in.bottom = h + 8;
            }
        }
        return in;
    }

    // ---- 兜底：把 availableGeometry 的差值当面板 ----
    QScreen* scr = QGuiApplication::primaryScreen();
    if (scr) {
        QRect avail = scr->availableGeometry();
        if (avail.height() > 0 && avail.height() < screenGeom.height()) {
            int d = screenGeom.height() - avail.height();
            if (avail.top() > screenGeom.top()) in.top = d;
            else                                in.bottom = d;
        }
    }
    return in;
}

void Garden::start()
{
    QScreen* scr = QGuiApplication::primaryScreen();
    QRect geom = scr->geometry();
    PanelInset ins = detectPanelInset(geom);

    // ---- 决定像素块缩放 ----
    // 原版的 PIX=4 比例就是设计时的意图，放大后反而失掉像素画的精致感。
    // 默认保持 1.0，只有用户显式给了 TBP_SCALE 才改。
    if (qEnvironmentVariableIsSet("TBP_SCALE")) {
        bool ok = false;
        float v = qgetenv("TBP_SCALE").toFloat(&ok);
        if (ok && v > 0.3f && v < 5.0f) g_baseScale = v;
    } else {
        g_baseScale = 1.0f;
    }
    g_scale = g_baseScale;

    // 以整块屏幕为基准，按探到的面板向内收。
    // 只有在完全探不到面板时才退回 availableGeometry（有些 WM 会正确报告）。
    QRect area = geom;
    if (ins.top || ins.bottom || ins.left || ins.right) {
        area.setTop(area.top() + ins.top);
        area.setBottom(area.bottom() - ins.bottom);
        area.setLeft(area.left() + ins.left);
        area.setRight(area.right() - ins.right);
    } else {
        const QRect avail = scr->availableGeometry();
        if (avail.width() > 0 && avail.height() > 0 && avail != geom)
            area = avail;
        else
            area.setBottom(area.bottom() - 44);   // 最后兜底：按一个常见面板厚度让开
    }

    m_winX = area.left();
    m_winY = area.top();
    m_winW = area.width();
    m_winH = area.height();
    m_floorY = (float)m_winH;

    m_win = new PotWindow(this);
    m_win->setGeometry(area);
    m_win->show();
    m_win->raise();

    if (qEnvironmentVariableIsSet("TBP_DEBUG")) {
        QFile lf("/tmp/tbp_debug.txt");
        if (lf.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream ts(&lf);
            QPoint gp = m_win->mapToGlobal(QPoint(0, 0));
            ts << "window requested=" << area.x() << "," << area.y()
               << " " << area.width() << "x" << area.height()
               << " actual=" << m_win->geometry().x() << "," << m_win->geometry().y()
               << " " << m_win->width() << "x" << m_win->height()
               << " globalOrigin=" << gp.x() << "," << gp.y()
               << " floorY=" << m_floorY << "\n";
            ts.flush();
        }
    }

    if (!m_layoutReady) {
        // 布局从 ini 恢复；命令行 --dry-seconds 的优先级高于 ini 里的 dry
        const float cliDry = g_drySeconds;
        const bool  dryFromCli = g_dryFromCli;
        LoadConfigFile(m_savedLayout, &g_drySeconds);
        if (dryFromCli) g_drySeconds = cliDry;

        if (!m_savedLayout.empty()) {
            for (size_t i = 0; i < m_savedLayout.size(); ++i) {
                const PotSeed& s = m_savedLayout[i];
                spawnPot(s.x, s.y, (PlantType)s.plant);
                Pot& np = m_pots.back();
                np.body.vy = 0.0f;          // 恢复的盆不要"跳"一下
                np.scale = Clampf(s.scale, POT_SCALE_MIN, POT_SCALE_MAX);
                np.moisture = Clampf(s.moisture, 0.0f, 1.0f);
                np.slot = (int)i;           // 绑定布局槽位，自动保存靠它回写水分
                np.prevLowestAbs = np.body.y;
            }
            tbpLog(QStringLiteral("config: 从 %1 恢复 %2 盆，dry=%3s")
                       .arg(ConfigPath()).arg(m_pots.size()).arg((int)g_drySeconds));
        } else {
            spawnPot((float)(m_winW / 2), m_floorY, PLANT_SUNFLOWER);
            Pot& np = m_pots.back();
            np.body.vy = 0.0f;
            np.slot = 0;                    // 首次运行的默认盆视为已保存

            PotSeed s;
            s.plant = PLANT_SUNFLOWER;
            s.x = np.body.x;
            s.y = np.body.y;
            s.scale = np.scale;
            s.moisture = np.moisture;
            m_savedLayout.push_back(s);
            tbpLog(QStringLiteral("config: 没有布局文件（%1），使用默认单盆")
                       .arg(ConfigPath()));
        }

        // 调试钩子：TBP_POT_X / TBP_POT_Y 指定初始位置（用于验证承托与跟随，
        // 不需要模拟鼠标操作）。只动第一个盆。
        if (!m_pots.empty()) {
            if (qEnvironmentVariableIsSet("TBP_POT_X"))
                m_pots[0].body.x = qgetenv("TBP_POT_X").toFloat();
            if (qEnvironmentVariableIsSet("TBP_POT_Y"))
                m_pots[0].body.y = qgetenv("TBP_POT_Y").toFloat();
        }

        m_layoutReady = true;
    }
}

void Garden::spawnPot(float x, float y, PlantType plant)
{
    Pot p;
    p.body.x = Clampf(x, POT_W * 0.5f, (float)m_winW - POT_W * 0.5f);
    p.body.y = Clampf(y, POT_H, (float)m_winH);
    p.body.vy = -160.0f * g_scale;          // 放下来时轻轻一跳
    p.body.angVel = 0.0f;
    p.plant = plant;
    p.swayVel = 3.0f;
    p.surfaceY = m_floorY;
    p.shadowY = m_floorY;
    // 穿越检测的"上一帧最低点"必须初始化成当前位置。
    // 留成 -1e9 的话第一帧会把任何顶边都当成"刚跨过去"，花盆会被吸到屏幕最上方。
    p.prevLowestAbs = p.body.y;
    // 每盆风相位错开，否则多盆会整齐划一地摇，很假
    p.windPh = (float)(rand() % 1000) * 0.00628f;      // 0 ~ 2π
    // 运行时新放的盆还没有进过布局快照
    p.slot = -1;
    m_pots.push_back(p);
}

int Garden::pickPot(float mx, float my, bool wantPlant, int* hitIdx)
{
    for (int i = (int)m_pots.size() - 1; i >= 0; --i) {
        const Pot& p = m_pots[i];
        // 每盆缩放不同，尺寸必须按 p.scale 算，否则缩放后点不准
        const float sc = g_scale * p.scale;
        const float hh = POT_H * 0.5f * p.scale;
        float cx = p.body.x;
        float cy = p.body.y - hh;
        float dx = mx - cx;
        float dy = my - cy;
        float c = cosf(p.body.angle);
        float s = sinf(p.body.angle);
        float lx =  dx * c + dy * s;
        float ly = -dx * s + dy * c;

        bool hit;
        if (wantPlant)
            hit = fabsf(lx) <= 42.0f * sc
               && ly >= -hh - PlantHeight(p.plant) * sc && ly <= -hh;
        else
            hit = fabsf(lx) <= POT_W * 0.5f * p.scale + 6.0f * sc
               && ly >= -hh - 10.0f && ly <= hh + 4.0f;

        if (hit) { if (hitIdx) *hitIdx = i; return i; }
    }
    if (hitIdx) *hitIdx = -1;
    return -1;
}

void Garden::beginDrag(int idx, float mx, float my)
{
    Pot& p = m_pots[idx];
    m_activePot = idx;
    p.body.dragged = true;
    p.poking = false;
    p.dragOffX = mx - p.body.x;
    p.dragOffY = my - p.body.y;
    p.body.angVel *= 0.2f;

    // 抓住花盆就打断上一轮的"躺倒 / 爬起"状态。
    // 不清的话，之前爬起来过的那盆，rising 会一直是 true，
    // 之后无论拖到多斜的角度松手都会被"爬起"分支强行立正。
    p.uprightDelay = 0.0f;
    p.rising = false;
}

void Garden::dragTo(float mx, float my)
{
    if (m_activePot < 0 || m_activePot >= (int)m_pots.size()) return;
    Pot& p = m_pots[m_activePot];
    if (!p.body.dragged && !p.poking) return;

    if (p.body.dragged) {
        const float pw = POT_W * 0.5f * p.scale;
        float tx = Clampf(mx - p.dragOffX, pw, (float)m_winW - pw);
        float ty = Clampf(my - p.dragOffY, POT_H, m_floorY);

        // 用相邻两帧位移估算甩出速度（限幅，避免快速拖动变成"发射炮弹"）
        float instVx = (tx - p.body.x) / DT;
        float instVy = (ty - p.body.y) / DT;
        p.body.vx = Clampf(p.body.vx * 0.4f + instVx * 0.6f, -1500.0f * g_scale, 1500.0f * g_scale);
        p.body.vy = Clampf(p.body.vy * 0.4f + instVy * 0.6f, -1500.0f * g_scale, 1500.0f * g_scale);
        p.body.x = tx;
        p.body.y = ty;
        p.body.angVel = Clampf(p.body.vx * 0.0009f, -2.5f, 2.5f);

        // 植物被拖着甩，产生惯性摆动
        p.swayVel = Clampf(p.swayVel - p.body.vx * 0.0045f, -16.0f, 16.0f);
    } else if (p.poking) {
        // 直接拖着植物走：茎被鼠标拉弯
        float bend = Clampf((mx - p.body.x) / (55.0f * g_scale), -1.3f, 1.3f);
        p.sway += (bend - p.sway) * 0.35f;
        p.swayVel = 0.0f;
    }
}

void Garden::endDrag()
{
    if (m_activePot >= 0 && m_activePot < (int)m_pots.size()) {
        Pot& p = m_pots[m_activePot];
        p.body.dragged = false;
        if (p.poking) {
            p.poking = false;
            p.swayVel -= p.sway * 4.0f;      // 松手后弹回去
        }
        p.body.vx = Clampf(p.body.vx, -1500.0f * g_scale, 1500.0f * g_scale);
        p.body.vy = Clampf(p.body.vy, -1500.0f * g_scale, 1500.0f * g_scale);
        p.body.angVel = Clampf(p.body.angVel, -7.0f, 7.0f);
    }
}

void Garden::applyMenuChoice(int idx, float mx, float my)
{
    Pot* p = (m_activePot >= 0 && m_activePot < (int)m_pots.size()) ? &m_pots[m_activePot] : nullptr;

    if (idx >= 0 && idx < PLANT_COUNT) {
        if (!p) return;
        p->plant = (PlantType)idx;
        p->sway = 0.0f;
        p->swayVel = 3.0f;          // 换植物时晃一下
        p->body.vy = -90.0f * g_scale;        // 轻轻一跳
    } else if (idx == MENU_CMD_WATER) {
        if (p) {
            p->waterAnim = WATER_DURATION;
            p->swayVel += 6.0f;      // 一盆水浇下去的那一下
        }
    } else if (idx == MENU_CMD_EDIT) {
        setEditMode(!m_editMode);
    } else if (idx == MENU_CMD_SCALE_UP || idx == MENU_CMD_SCALE_DOWN) {
        if (p) {
            float ns = p->scale + ((idx == MENU_CMD_SCALE_UP) ? POT_SCALE_STEP : -POT_SCALE_STEP);
            p->scale = Clampf(ns, POT_SCALE_MIN, POT_SCALE_MAX);
            p->body.vy = -90.0f * g_baseScale;   // 变大小后跳一下
            // 尺寸变了，上一帧的穿越检测基准跟着变，否则会误判"刚跨过顶边"
            p->prevLowestAbs = p->body.y - POT_Hf() * 0.5f * p->scale
                             + PotMaxY(p->body.angle, p->scale);
            m_win->requestFullRepaint();
        }
    } else if (idx == MENU_CMD_EXPORT) {
        // 推迟到事件循环的下一轮再开对话框。
        // 此刻 QMenu::exec 可能刚刚返回，菜单的鼠标/键盘 grab 还没完全释放，
        // 立刻再进一个模态循环容易把整个事件循环搞死。
        const int potIdx = m_activePot;
        QTimer::singleShot(0, m_win, [this, potIdx]() {
            if (potIdx >= 0 && potIdx < (int)m_pots.size())
                exportPotInteractive(m_pots[potIdx], m_win);
        });
    } else if (idx == MENU_CMD_SPAWN) {
        spawnPot(mx, my, p ? p->plant : PLANT_SUNFLOWER);
    } else if (idx == MENU_CMD_REMOVE) {
        if (p && m_pots.size() > 1) {
            m_pots.erase(m_pots.begin() + m_activePot);
            m_activePot = -1;
        }
    } else if (idx == MENU_CMD_QUIT) {
        quitWithFade();
    } else if (idx == MENU_CMD_VERSION) {
        // 版本号连点 7 次：开隐藏调试模式（面板在下一轮事件循环里开，
        // 此刻菜单的 grab 还没完全释放）
        QTimer::singleShot(0, m_win, [this]() { enableDebugMode(); });
    }
}

void Garden::quitWithFade()
{
    // 退出时补一次自动保存（只回写水分，不动布局）
    if (!m_editMode && !m_savedLayout.empty()) saveLayout(false);

    if (m_win) m_win->fadeOutAndClose();
    else qApp->quit();
}

// =============================================================================
//  隐藏调试模式（上游 v3.8.2）
//
//  上游是画在全屏分层窗口里的自绘工具条。移植版这里换成独立的 Qt 顶层窗口：
//  主窗口是"全屏 + 精确输入区域 + 点击穿透"，往里面塞 UI 要把工具条区域并进
//  输入区域，牵扯每帧的 region 计算，得不偿失。独立窗口有自己的输入区域，
//  关闭时也不留任何持久状态。
// =============================================================================
void Garden::enableDebugMode()
{
    if (m_debugMode) return;
    m_debugMode = true;

    if (!m_dbgBar) {
        QWidget* bar = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                              | Qt::Tool | Qt::NoDropShadowWindowHint);
        bar->setAttribute(Qt::WA_TranslucentBackground, true);
        bar->setAttribute(Qt::WA_DeleteOnClose, false);

        QWidget* panel = new QWidget(bar);
        panel->setObjectName(QStringLiteral("dbgPanel"));

        // 整张样式表一次设完。
        //
        // 之前分两次 setStyleSheet 拼接的做法是错的：第一段是"没有选择器的裸声明"
        // （还缺闭合花括号），Qt 的解析器会把后面拼上来的 QToolButton 规则当成
        // 非法声明吞掉 —— 结果是背景和边框生效、按钮配色全没生效，
        // 按钮退回系统调色板：启用态黑字、禁用态浅灰，在深色面板上正好反过来。
        panel->setStyleSheet(QStringLiteral(
            "#dbgPanel {"
            "  background-color: rgb(26,20,16);"
            "  border: 2px solid rgb(196,148,92);"
            "}"
            "QToolButton {"
            "  background-color: rgb(74,52,36);"
            "  color: rgb(242,234,222);"
            "  border: 2px solid rgb(150,112,72);"
            "  padding: 0px;"
            "}"
            "QToolButton:hover {"
            "  background-color: rgb(122,74,42);"
            "  border-color: rgb(236,190,128);"
            "  color: rgb(255,248,236);"
            "}"
            "QToolButton:pressed {"
            "  background-color: rgb(56,38,26);"
            "}"
            // 禁用态：明显更暗但字还看得清，不要暗到发黑
            "QToolButton:disabled {"
            "  background-color: rgb(48,38,30);"
            "  color: rgb(150,132,114);"
            "  border-color: rgb(96,78,62);"
            "}"));

        QHBoxLayout* row = new QHBoxLayout(panel);
        row->setContentsMargins(8, 8, 8, 8);
        row->setSpacing(8);

        QFont bf;
        bf.setPixelSize(14);
        bf.setFamily(QStringLiteral("Noto Sans CJK SC"));

        struct Item { int action; const char* label; bool usable; };
        const Item items[] = {
            { DBG_DRY,  "干枯",     true },
            { DBG_RAIN, "下雨",     true },
            { DBG_SNOW, "下雪",     true },
            { DBG_OFF,  "关闭调试", true },
        };
        for (const Item& it : items) {
            QToolButton* b = new QToolButton(panel);
            b->setText(QString::fromUtf8(it.label));
            b->setFont(bf);
            b->setFixedSize(110, 36);
            b->setCursor(Qt::PointingHandCursor);
            b->setEnabled(it.usable);
            const int act = it.action;
            QObject::connect(b, &QToolButton::clicked, bar, [this, act]() { doDebugAction(act); });
            row->addWidget(b);
        }

        QVBoxLayout* outer = new QVBoxLayout(bar);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->addWidget(panel);

        bar->adjustSize();

        // 摆在工作区顶部居中，压不到底部面板
        QScreen* scr = QGuiApplication::primaryScreen();
        const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 720);
        bar->move(avail.center().x() - bar->width() / 2, avail.top() + 20);

        m_dbgBar = bar;
    }

    m_dbgBar->show();
    m_dbgBar->raise();

    // TBP_DEBUG_BAR_PNG=<path>：把工具条渲染成图片，无头检查配色/字号/对齐。
    // 和 TBP_MENU_PNG 一个用途 —— 样式问题不能靠读代码确认。
    const QByteArray shot = qgetenv("TBP_DEBUG_BAR_PNG");
    if (!shot.isEmpty()) {
        m_dbgBar->adjustSize();
        m_dbgBar->grab().save(QString::fromLocal8Bit(shot));
    }

    tbpLog(QStringLiteral("debug: mode on (version %1)").arg(VersionText()));
}

void Garden::disableDebugMode()
{
    m_debugMode = false;
    if (m_dbgBar) {
        m_dbgBar->hide();
        m_dbgBar->deleteLater();     // 下次开启重建，不留旧状态
        m_dbgBar = nullptr;
    }
    tbpLog(QStringLiteral("debug: mode off"));
}

void Garden::doDebugAction(int action)
{
    switch (action) {
    case DBG_DRY:
        debugDryAll();
        break;
    case DBG_RAIN:
        g_weather = WEATHER_RAIN;
        g_weatherLeft = 30.0f;
        g_weatherTimer = 90.0f;
        g_lastWeather = WEATHER_RAIN;
        g_flakes.clear();
        tbpLog(QStringLiteral("debug: force rain"));
        break;
    case DBG_SNOW:
        g_weather = WEATHER_SNOW;
        g_weatherLeft = 30.0f;
        g_weatherTimer = 90.0f;
        g_lastWeather = WEATHER_SNOW;
        g_rain.clear();
        tbpLog(QStringLiteral("debug: force snow"));
        break;
    case DBG_OFF:
        disableDebugMode();
        break;
    default:
        break;
    }
}

// 把每一盆的泥土水分清零 —— 立刻进入枯萎状态，不用等 30 分钟。
void Garden::debugDryAll()
{
    for (size_t i = 0; i < m_pots.size(); ++i) {
        m_pots[i].moisture = 0.0f;
        m_pots[i].waterAnim = 0.0f;
    }
    if (m_win) m_win->requestFullRepaint();
    tbpLog(QStringLiteral("debug: dried all %1 pots").arg(m_pots.size()));
}

// =============================================================================
//  编辑模式（上游 v3.5，持久化在 v3.8.3 重做过）
//
//  规则很简单，但很容易做错：
//    · 拖动 / 抛掷 / 放新盆 / 移除盆，改动**只在内存里**
//    · 每 5 秒的自动保存**只回写各槽位的水分**，位置、数量、缩放一概不动
//    · 只有编辑模式里点「保存」，才把当前场景整体固化成布局快照
//  不这么分的话，随手把花盆甩到屏幕中间，重启后就永远待在那儿了。
// =============================================================================
void Garden::saveLayout(bool manual)
{
    if (manual) {
        m_savedLayout.clear();
        for (size_t i = 0; i < m_pots.size(); ++i) {
            const Pot& p = m_pots[i];
            PotSeed s;
            s.plant    = (int)p.plant;
            s.x        = p.body.x;
            s.y        = p.body.y;
            s.scale    = p.scale;
            s.moisture = p.moisture;
            m_savedLayout.push_back(s);
            m_pots[i].slot = (int)i;      // 每盆重新绑定到新布局的槽位
        }
    } else {
        for (size_t i = 0; i < m_pots.size(); ++i) {
            const Pot& p = m_pots[i];
            if (p.slot >= 0 && p.slot < (int)m_savedLayout.size())
                m_savedLayout[p.slot].moisture = p.moisture;
        }
    }
    SaveConfigFile(m_savedLayout, g_drySeconds);
}

// 干枯时长档位（上游同一组）：5 分钟 ~ 4 小时
static const float kDrySteps[] = { 300.0f, 900.0f, 1800.0f, 3600.0f, 7200.0f, 14400.0f };
static const int   kDryStepCount = (int)(sizeof(kDrySteps) / sizeof(kDrySteps[0]));

static QString DryLabel()
{
    const int ds = (int)g_drySeconds;
    if (ds >= 60 && ds % 60 == 0)
        return QStringLiteral("干枯 %1 分钟").arg(ds / 60);
    return QStringLiteral("干枯 %1 秒").arg(ds);
}

// 工具条统一摆位：编辑工具条在最上面，调试工具条排在它下面，避免互相压住
static void PlaceBar(QWidget* w, int yTop)
{
    if (!w) return;
    w->adjustSize();
    QScreen* scr = QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 720);
    w->move(avail.center().x() - w->width() / 2, avail.top() + yTop);
}

void Garden::setEditMode(bool on)
{
    if (m_editMode == on) return;
    m_editMode = on;

    if (on) {
        if (!m_editBar) {
            QWidget* bar = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                                                  | Qt::Tool | Qt::NoDropShadowWindowHint);
            bar->setAttribute(Qt::WA_TranslucentBackground, true);

            QWidget* panel = new QWidget(bar);
            panel->setObjectName(QStringLiteral("editPanel"));
            panel->setStyleSheet(QStringLiteral(
                "#editPanel {"
                "  background-color: rgb(26,20,16);"
                "  border: 2px solid rgb(196,148,92);"
                "}"
                "QToolButton {"
                "  background-color: rgb(74,52,36);"
                "  color: rgb(242,234,222);"
                "  border: 2px solid rgb(150,112,72);"
                "  padding: 0px;"
                "}"
                "QToolButton:hover {"
                "  background-color: rgb(122,74,42);"
                "  border-color: rgb(236,190,128);"
                "  color: rgb(255,248,236);"
                "}"
                "QToolButton:pressed { background-color: rgb(56,38,26); }"
                "QLabel { color: rgb(246,240,230); background: transparent; border: none; }"));

            QVBoxLayout* col = new QVBoxLayout(panel);
            col->setContentsMargins(8, 8, 8, 8);
            col->setSpacing(6);

            QHBoxLayout* row = new QHBoxLayout;
            row->setSpacing(8);

            QFont bf;
            bf.setPixelSize(14);
            bf.setFamily(QStringLiteral("Noto Sans CJK SC"));

            auto mk = [&](const QString& text, int w) {
                QToolButton* b = new QToolButton(panel);
                b->setText(text);
                b->setFont(bf);
                b->setFixedSize(w, 36);
                b->setCursor(Qt::PointingHandCursor);
                row->addWidget(b);
                return b;
            };

            QToolButton* addBtn = mk(QStringLiteral("+ 新花盆"), 110);
            m_dryBtn            = mk(DryLabel(), 120);
            QToolButton* saveBtn = mk(QStringLiteral("保存"), 90);
            QToolButton* doneBtn = mk(QStringLiteral("完成"), 90);

            col->addLayout(row);

            // 状态行：保存结果 / "标题栏上的花盆只能存在任务栏" 的提醒
            m_editStatus = new QLabel(bar);
            m_editStatus->setAlignment(Qt::AlignHCenter);
            m_editStatus->setFont(bf);
            m_editStatus->setStyleSheet(QStringLiteral("color: rgb(246,214,160);"));
            m_editStatus->setVisible(false);
            col->addWidget(m_editStatus);

            QObject::connect(addBtn,  &QToolButton::clicked, bar, [this]() { editAddPot(); });
            QObject::connect(m_dryBtn, &QToolButton::clicked, bar, [this]() { editCycleDry(); });
            QObject::connect(saveBtn, &QToolButton::clicked, bar, [this]() { editSave(); });
            QObject::connect(doneBtn, &QToolButton::clicked, bar, [this]() { setEditMode(false); });

            QVBoxLayout* outer = new QVBoxLayout(bar);
            outer->setContentsMargins(0, 0, 0, 0);
            outer->addWidget(panel);

            m_editBar = bar;
        }
        m_dryBtn->setText(DryLabel());
        PlaceBar(m_editBar, 20);
        m_editBar->show();
        m_editBar->raise();
        if (m_dbgBar) PlaceBar(m_dbgBar, 20 + m_editBar->height() + 8);
        tbpLog(QStringLiteral("edit: mode on"));

        // TBP_EDIT_PNG=<path>：把工具条渲染成图，无头检查配色/文案
        const QByteArray shot = qgetenv("TBP_EDIT_PNG");
        if (!shot.isEmpty()) {
            m_editBar->adjustSize();
            m_editBar->grab().save(QString::fromLocal8Bit(shot));
        }
    } else {
        if (m_editBar) m_editBar->hide();
        if (m_editStatus) m_editStatus->setVisible(false);
        if (m_dbgBar) PlaceBar(m_dbgBar, 20);
        tbpLog(QStringLiteral("edit: mode off"));
    }

    if (m_win) m_win->requestFullRepaint();
}

void Garden::editAddPot()
{
    // 屏幕中间附近随机撒一盆
    const float cx = (float)m_winW * 0.5f + (float)((rand() % 401) - 200);
    spawnPot(cx, m_floorY, (PlantType)(rand() % PLANT_COUNT));
    if (m_win) m_win->requestFullRepaint();
    tbpLog(QStringLiteral("edit: spawned pot, now %1").arg(m_pots.size()));
}

void Garden::editCycleDry()
{
    int cur = 2;                       // 找不到就按默认 30 分钟算
    for (int i = 0; i < kDryStepCount; ++i)
        if (fabsf(kDrySteps[i] - g_drySeconds) < 1.0f) { cur = i; break; }
    g_drySeconds = kDrySteps[(cur + 1) % kDryStepCount];
    if (m_dryBtn) m_dryBtn->setText(DryLabel());
}

void Garden::editSave()
{
    saveLayout(true);

    // ini 只记窗口坐标：站在标题栏上的花盆，重启后会落回任务栏
    bool onTitleBar = false;
    for (size_t i = 0; i < m_pots.size(); ++i) {
        if (m_pots[i].surfaceY < m_floorY - 1.0f) { onTitleBar = true; break; }
    }

    if (m_editStatus) {
        m_editStatus->setText(onTitleBar
            ? QStringLiteral("已保存 · 标题栏上的花盆重启后会落回任务栏")
            : QStringLiteral("已保存"));
        m_editStatus->setVisible(true);
        QTimer::singleShot(2600, m_editStatus, [this]() {
            if (m_editStatus) m_editStatus->setVisible(false);
        });
    }
    if (m_dryBtn) m_dryBtn->setText(DryLabel());
    tbpLog(QStringLiteral("edit: saved %1 pots, dry=%2s")
               .arg(m_savedLayout.size()).arg((int)g_drySeconds));
}

void Garden::updatePot(Pot& p, float dt)
{
    Body& b = p.body;
    const float hw = POT_W * 0.5f * p.scale;
    const float hh = POT_H * 0.5f * p.scale;

    // ---- 水分：平时慢慢蒸发，浇水后快速吸饱 ----
    // 编辑模式暂停蒸发：摆位置的时候眼看着植物变枯没道理，
    // 而且这时候水分也不该被写进快照。
    if (p.waterAnim > 0.0f) {
        p.waterAnim -= dt;
        if (p.waterAnim < 0.0f) p.waterAnim = 0.0f;
        p.moisture = Clampf(p.moisture + dt / 1.2f, 0.0f, 1.0f);
        // 晃动只在浇下的那一下给一次冲量（见 applyMenuChoice），
        // 之后交给弹簧自然衰减。持续正弦激励会让人觉得一直在抖。
    } else if (m_editMode) {
        // 什么都不做
    } else if (g_wiltEnabled) {
        p.moisture = Clampf(p.moisture - dt / g_drySeconds, 0.0f, 1.0f);
    } else {
        p.moisture = 1.0f;          // 关掉枯萎就当作永远浇透了
    }

    // ---- 缺水：从植株上不断掉落枯叶碎片 ----
    if (!m_editMode && g_wiltEnabled && p.moisture < DRY_THRESHOLD && m_wilt.size() < 240) {
        p.shedTimer -= dt;
        if (p.shedTimer <= 0.0f) {
            const float dry = (DRY_THRESHOLD - p.moisture) / DRY_THRESHOLD;
            p.shedTimer = 0.55f - 0.40f * dry;              // 越干掉得越快

            // 在植物本地坐标里随机取一点，再按花盆的旋转和缩放转到世界坐标
            const float lx = (float)((rand() % 41) - 20);
            const float ly = -10.0f
                - (float)(rand() % (int)(PlantHeight(p.plant) * 0.75f + 1.0f));
            const float ca = cosf(b.angle), sa = sinf(b.angle);

            Crumb w;
            w.x = b.x + (lx * ca - ly * sa) * p.scale;
            w.y = (b.y - hh) + (lx * sa + ly * ca) * p.scale;
            w.vx = (float)((rand() % 70) - 35);
            w.vy = 10.0f + (float)(rand() % 40);
            w.life = 3.0f;
            w.floorY = m_floorY;
            m_wilt.push_back(w);
        }
    }

    if (b.dragged) {
        b.angle += b.angVel * dt;
        b.angVel *= 0.96f;
    } else {
        // 重力随缩放同步放大，保证"看起来"的下落节奏和原版一致
        b.vy += GRAVITY * g_scale * dt;
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        b.angle += b.angVel * dt;
    }

    while (b.angle > PI_F)  b.angle -= 2.0f * PI_F;
    while (b.angle < -PI_F) b.angle += 2.0f * PI_F;

    if (b.x < hw) {
        b.x = hw;
        if (b.vx < 0.0f) b.vx = -b.vx * 0.45f;
    }
    if (b.x > (float)m_winW - hw) {
        b.x = (float)m_winW - hw;
        if (b.vx > 0.0f) b.vx = -b.vx * 0.45f;
    }

    // ---------- 承托面 ----------
    float maxY = PotMaxY(b.angle, p.scale);
    float lowest = b.y - hh + maxY;
    float surface = m_floorY;
    float shadowSurface = m_floorY;
    if (!b.dragged) {
        float absCx    = (float)m_winX + b.x;
        float absLow   = (float)m_winY + lowest;
        float absFloor = (float)m_winY + m_floorY;

        SupportHit hit = findSupport(absCx, p.prevLowestAbs, absLow, absFloor,
                                     (int)m_win->winId());

        // 窗口横向移动时花盆跟着平移（要求还是同一个承托面）
        if (hit.found && p.supportWidth > 0.0f
            && fabsf(hit.width - p.supportWidth) < 2.0f
            && fabsf(hit.top - p.supportTop) < 2.0f) {
            float dx = hit.left - p.supportLeft;
            if (dx != 0.0f) {
                b.x += dx;
                b.vx = 0.0f;
            }
        }
        p.supportTop   = hit.found ? hit.top   : 0.0f;
        p.supportLeft  = hit.found ? hit.left  : 0.0f;
        p.supportWidth = hit.found ? hit.width : 0.0f;

        surface = hit.top - (float)m_winY;
        shadowSurface = hit.shadowTop - (float)m_winY;
    }
    // 记录本帧最低点，下一帧用它做穿越检测
    p.prevLowestAbs = (float)m_winY + (b.y - hh + PotMaxY(b.angle, p.scale));

    // 任务栏上沿永远是"最底的地面"
    if (surface > m_floorY) surface = m_floorY;
    if (surface < 4.0f) surface = 4.0f;
    p.surfaceY = surface;

    if (shadowSurface > m_floorY) shadowSurface = m_floorY;
    if (shadowSurface < 4.0f) shadowSurface = 4.0f;
    p.shadowY = shadowSurface;

    const float limit = surface + hh - maxY;
    if (b.y > limit) {
        b.y = limit;
        if (b.vy > 0.0f) {
            if (b.vy > 260.0f * g_scale) SpawnDust(m_crumbs, b.x, surface, 10);
            float impact = b.vy;
            // 砸进积水里会溅起水花（v3.7）
            if (g_puddle > 1.5f && surface >= m_floorY - 1.0f && impact > 200.0f * g_scale)
                SpawnSplash(b.x, surface - g_puddle, 9);
            b.vy = -b.vy * RESTITUTION;
            if (fabsf(b.vy) < 55.0f * g_scale) b.vy = 0.0f;
            b.vx *= 0.90f;
            b.angVel += b.vx * 0.0016f;
            // 重摔时再补一点旋转。必须按当前转向取符号 ——
            // 原来是无条件 += 0.4f（恒为正），结果只有往右摔能躺平，
            // 往左摔会被这个正向角速度顶回去，看着像"站起来了"。
            if (impact > 1000.0f * g_scale)
                b.angVel += 0.4f * (b.angVel >= 0.0f ? 1.0f : -1.0f);
            b.angVel = Clampf(b.angVel, -9.0f, 9.0f);
        }

        if (!b.dragged) {
            b.vx *= 0.93f;
            if (fabsf(b.vx) < 2.0f * g_scale) b.vx = 0.0f;

            // ------------------------------------------------------------------
            //  自动回正：三种情况
            //
            //  angle 已归一化到 ±180°，所以 |angle| 就是倾角：
            //      0° = 正立   90° = 侧躺   180° = 倒扣
            //
            //  · 轻微倾斜（< 50°）：欠阻尼弹簧，像不倒翁。
            //      回正力矩 ∝ 倾角，绕过竖直位置时速度最大 → 惯性冲过头
            //      → 反向再晃 → 收敛。ζ≈0.46，过冲约 19%，周期约 1.1 秒。
            //
            //  · 重心出界（>= 50°）：真花盆的重心一旦越过底面边缘就再也拉不回来，
            //      只能继续倒下去。这里让它加速倒向侧躺（90°），然后走翻车流程。
            //      没有这一条的话，斜到 89° 还会自己站回来，很假。
            //      （几何上 atan(36/24)≈56°，取 50° 让"摔倒"更干脆，手感优先）
            //
            //  · 翻车（>= 90°）：摊着不动，攒够 --upright-delay 秒，
            //      再用过阻尼慢慢撑起来（约 3 秒），爬的过程不左右摇。
            //
            //  注意：这里只改 angVel，angle 的积分和边界归一化都在上面的物理段，
            //  这样撞到窗口/地面时旋转动量才能正确传递。
            // ------------------------------------------------------------------
            const float deg = fabsf(b.angle) * 57.2957795f;
            const bool flipped  = deg >= 90.0f;
            const bool unstable = deg >= 50.0f;   // 重心已出界，站不回来

            const bool settled = fabsf(b.vx) < 1.0f
                              && fabsf(b.vy) < 5.0f
                              && fabsf(b.angVel) < 0.05f;

            if (flipped) {
                if (settled) p.uprightDelay += dt;
                if (p.uprightDelay >= g_uprightDelay) p.rising = true;
            } else {
                p.uprightDelay = 0.0f;
                // 快到竖直了才认为"爬起来了"，否则爬的过程中一进入失稳区
                // 就会被失稳逻辑重新推倒，来回拉锯
                if (deg < 30.0f) p.rising = false;
            }

            if (flipped && !p.rising) {
                // 摊着不动：强阻尼按住，避免残余角速度让它慢慢滑
                b.angVel *= expf(-14.0f * dt);
                if (fabsf(b.angVel) < 0.02f) b.angVel = 0.0f;
            } else if (p.rising) {
                // 费力撑起来：过阻尼（ζ≈1.2），从 180° 倒扣转回竖直约 3 秒，
                // 一次到位不左右摇 —— 爬起来要是晃，就成了诈尸。
                b.angVel -= b.angle * 6.5f * dt;
                b.angVel *= expf(-6.1f * dt);
            } else if (unstable) {
                // 重心出界：倒向侧躺。重力接管，比回正更有劲。
                const float target = (b.angle > 0.0f ? 1.0f : -1.0f) * (PI_F * 0.5f);
                b.angVel += (target - b.angle) * 18.0f * dt;
                b.angVel *= expf(-3.5f * dt);
            } else {
                // 摇晃回正：欠阻尼弹簧
                b.angVel -= b.angle * 42.0f * dt;
                b.angVel *= expf(-6.0f * dt);

                // 角度和角速度都够小才算站稳，避免长期微抖
                if (fabsf(b.angle) < 0.006f && fabsf(b.angVel) < 0.15f) {
                    b.angle = 0.0f;
                    b.angVel = 0.0f;
                }
            }
        }
    }

    float hardLimit = m_floorY + hh - maxY;
    if (b.y > hardLimit) {
        b.y = hardLimit;
        if (b.vy > 0.0f) b.vy = 0.0f;
    }

    b.x = Clampf(b.x, -hw, (float)m_winW + hw);
    b.vx = Clampf(b.vx, -3200.0f * g_scale, 3200.0f * g_scale);
    b.vy = Clampf(b.vy, -3200.0f * g_scale, 3200.0f * g_scale);

    // ---- 状态日志：每秒一行，看清花盆到底停在哪 ----
    if (qEnvironmentVariableIsSet("TBP_TRACE")) {
        static int st = 0;
        if ((st++ % 60) == 0) {
            tbpLog(QStringLiteral("pot: x=%1 y=%2 surf=%3 shadow=%4 vy=%5 drag=%6 hit=%7 deg=%8 rise=%9 wait=%10")
                       .arg((int)b.x).arg((int)b.y)
                       .arg((int)p.surfaceY).arg((int)p.shadowY)
                       .arg(b.vy, 0, 'f', 0)
                       .arg(b.dragged ? 1 : 0)
                       .arg(p.supportWidth > 0.0f ? 1 : 0)
                       .arg(fabsf(b.angle) * 57.2957795f, 0, 'f', 0)
                       .arg(p.rising ? 1 : 0)
                       .arg(p.uprightDelay, 0, 'f', 1)
                       + QStringLiteral(" moist=%1 wilt=%2")
                             .arg(p.moisture, 0, 'f', 2)
                             .arg((int)m_wilt.size()));
        }
    }

    // 植物摆动：阻尼弹簧（上游 v3.6 调软：更慢、更柔）
    const float stiffness = 72.0f;               // 原 110：弯得少了，回弹慢了
    const float damping = 4.6f;                  // 原 6.5：欠阻尼更明显，像有韧性
    p.swayVel += (-stiffness * p.sway - damping * p.swayVel) * dt;
    // 常年微风：静止时也轻轻摇。驱动频率 0.7 rad/s 远低于固有频率
    // √72 ≈ 8.5 rad/s，响应近似准静态，表现为缓慢的整体倾倒而不是抖动。
    p.swayVel += WindForce((float)m_time + p.windPh) * (1.0f + g_storm * 2.2f) * dt;
    p.sway += p.swayVel * dt;
    const float swayLimit = 1.3f + g_storm * 0.5f;   // 大风天允许摆得更多
    p.sway = Clampf(p.sway, -swayLimit, swayLimit);
}

void Garden::updatePhysics(float dt)
{
    // 天气先更新：它会给花盆补水，也会更新风雨增幅 g_storm
    UpdateWeather(dt, m_pots, m_winW, m_floorY, m_time);

    for (size_t i = 0; i < m_pots.size(); ++i)
        updatePot(m_pots[i], dt);

    // 枯叶碎屑：比重力加速度小得多，还会左右飘，落地就贴住
    for (size_t i = 0; i < m_wilt.size();) {
        Crumb& w = m_wilt[i];
        w.vy += 420.0f * g_scale * dt;
        w.vx += sinf(m_time * 2.4f + w.y * 0.03f) * 24.0f * g_scale * dt;
        w.x += w.vx * dt;
        w.y += w.vy * dt;
        if (w.y > w.floorY - 3.0f) {
            w.y = w.floorY - 3.0f;
            w.vx = 0.0f;
            w.vy = 0.0f;
        }
        w.life -= dt;
        if (w.life <= 0.0f) {
            m_wilt[i] = m_wilt.back();
            m_wilt.pop_back();
        } else {
            ++i;
        }
    }

    for (size_t i = 0; i < m_crumbs.size();) {
        Crumb& c = m_crumbs[i];
        c.vy += 1800.0f * g_scale * dt;
        c.x += c.vx * dt;
        c.y += c.vy * dt;
        if (c.y > c.floorY) {
            c.y = c.floorY;
            c.vy *= -0.3f;
            c.vx *= 0.6f;
        }
        c.life -= dt;
        if (c.life <= 0.0f) {
            m_crumbs[i] = m_crumbs.back();
            m_crumbs.pop_back();
        } else {
            ++i;
        }
    }
}

void Garden::tick()
{
    m_time += DT;

    // 自动保存：每 5 秒一次，但**只回写水分**（位置/数量/缩放不动）。
    // 编辑模式里不写盘 —— 摆到一半的东西不该落进 ini。
    if (!m_editMode) {
        m_saveTimer += DT;
        if (m_saveTimer >= 5.0f) {
            m_saveTimer = 0.0f;
            saveLayout(false);
        }
    }

    // ------------------------------------------------------------------------
    //  兜底：左键已经松开了，但花盆还标记着"被拖拽" —— 强制结束。
    //
    //  输入区域（XShape ShapeInput）每帧都在跟着花盆重设，拖拽过程中偶尔会丢掉
    //  mouseRelease 事件，结果花盆被"冻"在松手的位置，既不受重力也不落地。
    //  这里靠全局按键状态兜住，代价可以忽略。
    // ------------------------------------------------------------------------
    if (!(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        for (size_t i = 0; i < m_pots.size(); ++i) {
            Pot& p = m_pots[i];
            if (!p.body.dragged && !p.poking) continue;
            p.body.dragged = false;
            if (p.poking) {
                p.poking = false;
                p.swayVel -= p.sway * 4.0f;
            }
            p.body.vx = Clampf(p.body.vx, -1500.0f * g_scale, 1500.0f * g_scale);
            p.body.vy = Clampf(p.body.vy, -1500.0f * g_scale, 1500.0f * g_scale);
            p.body.angVel = Clampf(p.body.angVel, -7.0f, 7.0f);
        }
    }

    updatePhysics(DT);

    // 天气状态：每秒一行（只在 TBP_TRACE 下）
    if (qEnvironmentVariableIsSet("TBP_TRACE")) {
        static int wt = 0;
        if ((wt++ % 60) == 0) {
            const char* kind = g_weather == WEATHER_RAIN ? "rain"
                             : (g_weather == WEATHER_SNOW ? "snow" : "clear");
            tbpLog(QStringLiteral("weather: %1 left=%2 precip=%3 storm=%4 zones=%5 "
                                  "rain=%6 flakes=%7 splash=%8 puddle=%9 snow=%10")
                       .arg(kind)
                       .arg(g_weatherLeft, 0, 'f', 0)
                       .arg(g_precip, 0, 'f', 2)
                       .arg(g_storm, 0, 'f', 2)
                       .arg(g_zones.size())
                       .arg(g_rain.size())
                       .arg(g_flakes.size())
                       .arg(g_splash.size())
                       .arg(g_puddle, 0, 'f', 2)
                       .arg(g_snowCover, 0, 'f', 2));
            for (size_t zi = 0; zi < g_zones.size(); ++zi)
                tbpLog(QStringLiteral("weather zone[%1]: l=%2 r=%3 top=%4 (w=%5)  winW=%6")
                           .arg(zi)
                           .arg((int)g_zones[zi].l).arg((int)g_zones[zi].r)
                           .arg((int)g_zones[zi].top)
                           .arg((int)(g_zones[zi].r - g_zones[zi].l))
                           .arg(m_winW));
        }
    }

    if (m_win) {
        m_dirty = QRect();
        // 1) 重算精确输入区域（只有真正有像素的地方接收鼠标）
        m_win->updateInputMask();
        // 2) 按脏矩形重绘，避免每帧整屏合成
        QRect d = takeDirty();
        if (d.isValid() && !d.isEmpty())
            m_win->update(d);
    }
}

// =============================================================================
//  KWin 桥：注册 D-Bus 服务 + 安装/加载 KWin 脚本
// =============================================================================
#if defined(TBP_HAVE_DBUS)

class GeometryBridge : public QObject
{
    Q_OBJECT
public:
    static const char* kService;
    static const char* kPath;
    static const char* kIface;
    static const char* kPluginId;

    bool installed() const { return m_ok; }

    // 注册服务（供 KWin 脚本调用）并安装 / 加载 KWin 脚本
    bool install()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        tbpLog(QStringLiteral("dbus: connected=%1 name=%2")
                   .arg(bus.isConnected() ? 1 : 0)
                   .arg(bus.name()));
        if (!bus.isConnected()) {
            tbpLog(QStringLiteral("dbus: FAIL no session bus"));
            return false;
        }

        // 清掉上一次崩溃/强杀留下的同名服务，否则 registerService 会一直失败
        bus.unregisterService(QString::fromLatin1(kService));

        if (!bus.registerService(QString::fromLatin1(kService))) {
            tbpLog(QStringLiteral("dbus: FAIL registerService %1 (%2)")
                       .arg(QString::fromLatin1(kService), bus.lastError().message()));
            return false;
        }
        // 关键：必须显式指定接口名。
        // 两参数版 registerObject(path, obj, ExportAllSlots) 会把 slot 注册到
        // **类名**接口（"GeometryBridge"），而 KWin 脚本 callDBus 时指定的接口是
        // "org.taskbarplant.Geometry"，名称不匹配就报 UnknownMethod ——
        // 表现就是脚本一直在推送、程序一条都收不到。
        if (!bus.registerObject(QString::fromLatin1(kPath),
                                QString::fromLatin1(kIface),
                                this,
                                QDBusConnection::ExportAllSlots)) {
            tbpLog(QStringLiteral("dbus: FAIL registerObject (%1)")
                       .arg(bus.lastError().message()));
            bus.unregisterService(QString::fromLatin1(kService));
            return false;
        }
        tbpLog(QStringLiteral("dbus: OK service registered iface=%1")
                   .arg(QString::fromLatin1(kIface)));

        // ------------------------------------------------------------------
        //  1. 安装成正式的 KWin 脚本包
        //     这样"系统设置 → KWin 脚本"里能看到它，用户也能自己关掉。
        //     运行时 loadScript 虽然能立刻生效，但不会出现在那个列表里。
        // ------------------------------------------------------------------
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        const QString pkgDir  = dataDir + "/kwin/scripts/" + kPluginId;
        const QString codeDir = pkgDir + "/contents/code";
        QDir().mkpath(codeDir);

        QString js = readResource(":/kwin_bridge.js");
        if (js.isEmpty()) js = fallbackScript();

        QString meta = readResource(":/metadata.json");
        if (meta.isEmpty()) meta = fallbackMetadata();

        bool w1 = writeFile(pkgDir + "/metadata.json", meta);
        m_mainJs = codeDir + "/main.js";
        bool w2 = writeFile(m_mainJs, js);
        tbpLog(QStringLiteral("script: pkg=%1 meta=%2 js=%3 len=%4")
                   .arg(pkgDir).arg(w1 ? 1 : 0).arg(w2 ? 1 : 0).arg(js.size()));

        // ------------------------------------------------------------------
        //  2. 在 kwinrc 里启用（只在没配置过时写，尊重用户的选择）
        //
        //  KDE 的真实键名是 "<id>Enabled"，不是 "<id>"。
        //  写成后者完全无效 —— 系统设置里看不到勾选，reconfigure 也不会加载。
        // ------------------------------------------------------------------
        QSettings kwinrc(QDir::homePath() + "/.config/kwinrc", QSettings::IniFormat);
        const QString key = QStringLiteral("Plugins/%1Enabled").arg(QString::fromLatin1(kPluginId));
        if (!kwinrc.contains(key)) {
            kwinrc.setValue(key, true);
            kwinrc.sync();
            tbpLog(QStringLiteral("script: kwinrc enabled %1").arg(key));
        } else if (!kwinrc.value(key).toBool()) {
            // 用户在"系统设置 → KWin 脚本"里明确关掉了，不要强行打开。
            // D-Bus 服务保留，程序会退回 X11 枚举（只能看到 XWayland 窗口）。
            tbpLog(QStringLiteral("script: user disabled it, skip loading"));
            m_ok = true;
            return true;
        }

        QDBusInterface kwin(QStringLiteral("org.kde.KWin"),
                            QStringLiteral("/KWin"),
                            QStringLiteral("org.kde.KWin"),
                            bus);
        if (kwin.isValid())
            kwin.call(QStringLiteral("reconfigure"));

        // ------------------------------------------------------------------
        //  3. 强制重新加载，确保 KWin 内存里跑的是刚写下去的这份
        //
        //     不能"已加载就跳过"：用户从系统设置启用过的话，KWin 里跑的是
        //     上一次启动时读进内存的旧版本，文件更新了但内存没变。
        // ------------------------------------------------------------------
        QDBusInterface scripting(QStringLiteral("org.kde.KWin"),
                                 QStringLiteral("/Scripting"),
                                 QStringLiteral("org.kde.kwin.Scripting"),
                                 bus);
        if (!scripting.isValid()) {
            tbpLog(QStringLiteral("kwin: FAIL scripting interface invalid (%1)")
                       .arg(scripting.lastError().message()));
            return false;
        }

        scripting.call(QStringLiteral("unloadScript"), QString::fromLatin1(kPluginId));
        QDBusReply<int> id = scripting.call(QStringLiteral("loadScript"),
                                            m_mainJs,
                                            QString::fromLatin1(kPluginId));
        tbpLog(QStringLiteral("kwin: loadScript valid=%1 id=%2 err=%3")
                   .arg(id.isValid() ? 1 : 0)
                   .arg(id.isValid() ? id.value() : -1)
                   .arg(id.error().message()));

        scripting.call(QStringLiteral("start"));

        QDBusReply<bool> loaded = scripting.call(QStringLiteral("isScriptLoaded"),
                                                 QString::fromLatin1(kPluginId));
        tbpLog(QStringLiteral("kwin: isScriptLoaded=%1").arg(loaded.value() ? 1 : 0));

        m_ok = true;
        return true;
    }

    ~GeometryBridge()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (m_ok) {
            QDBusInterface scripting(QStringLiteral("org.kde.KWin"),
                                     QStringLiteral("/Scripting"),
                                     QStringLiteral("org.kde.kwin.Scripting"),
                                     bus);
            if (scripting.isValid())
                scripting.call(QStringLiteral("unloadScript"),
                               QString::fromLatin1(kPluginId));
            bus.unregisterObject(QString::fromLatin1(kPath));
            bus.unregisterService(QString::fromLatin1(kService));
        }
    }

signals:
    void geometryChanged();

public slots:
    // KWin 脚本每 33ms 调一次：JSON 数组，每项 [x, y, w, h]
    void update(const QString& json)
    {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) return;
        if (!doc.isArray()) return;

        std::vector<WinGeom> out;
        const QJsonArray arr = doc.array();
        out.reserve((size_t)arr.size());
        int skippedMin = 0;
        for (const QJsonValue& v : arr) {
            if (!v.isArray()) continue;
            QJsonArray a = v.toArray();
            if (a.size() < 4) continue;

            // 第 5 位 = minimized，第 6 位 = hidden。任一为真就不是可站立的面。
            if (a.size() >= 5 && a[4].toInt() != 0) { ++skippedMin; continue; }
            if (a.size() >= 6 && a[5].toInt() != 0) { ++skippedMin; continue; }

            WinGeom g;
            g.x = (float)a[0].toDouble();
            g.y = (float)a[1].toDouble();
            g.w = (float)a[2].toDouble();
            g.h = (float)a[3].toDouble();
            if (g.w <= 0 || g.h <= 0) continue;
            out.push_back(g);
        }

        g_kwinGeoms = out;
        g_kwinLive = true;

        // 只在第一次和每次数量变化时记录，避免每 33ms 刷屏
        static int lastN = -1;
        static int lastMin = -1;
        if ((int)out.size() != lastN || skippedMin != lastMin) {
            lastN = (int)out.size();
            lastMin = skippedMin;
            QString s = QStringLiteral("kwin push: %1 windows (minimized skipped %2)")
                            .arg(out.size()).arg(skippedMin);
            for (size_t i = 0; i < out.size() && i < 8; ++i) {
                s += QStringLiteral(" [%1,%2 %3x%4]")
                         .arg((int)out[i].x).arg((int)out[i].y)
                         .arg((int)out[i].w).arg((int)out[i].h);
            }
            tbpLog(s);
        }

        emit geometryChanged();
    }

private:
    static QString readResource(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        return QString::fromUtf8(f.readAll());
    }

    static bool writeFile(const QString& path, const QString& text)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        const bool ok = (f.write(text.toUtf8()) >= 0);
        f.close();
        return ok;
    }

    static QString fallbackMetadata()
    {
        return QStringLiteral(
            "{\n"
            "  \"KPackageStructure\": \"KWin/Script\",\n"
            "  \"KPlugin\": {\n"
            "    \"Id\": \"taskbarplant-geometry\",\n"
            "    \"Name\": \"TaskbarPlant Geometry Bridge\",\n"
            "    \"Description\": \"Pushes window geometry to TaskbarPlant\",\n"
            "    \"Icon\": \"preferences-system-windows-script-test\",\n"
            "    \"License\": \"MIT\",\n"
            "    \"Version\": \"1.0\"\n"
            "  },\n"
            "  \"X-Plasma-API\": \"javascript\",\n"
            "  \"X-Plasma-MainScript\": \"code/main.js\"\n"
            "}\n");
    }

    // 万一 Qt 资源没编译进去，用这份内联版兜底。逻辑必须和 src/kwin_bridge.js 一致：
    // 不能用 w.visible 过滤（恒 false，会清空列表），要过滤小窗口和桥接窗口。
    static QString fallbackScript()
    {
        return QStringLiteral(
            "var t = new QTimer();\n"
            "t.interval = 33;\n"
            "t.timeout.connect(function(){\n"
            "  var o = [];\n"
            "  var l = workspace.windowList();\n"
            "  for (var i=0;i<l.length;++i) {\n"
            "    var w = l[i];\n"
            "    try {\n"
            "      if (w.deleted) continue;\n"
            "      if (w.windowType !== 0) continue;\n"
            "      var rc = String(w.resourceClass).toLowerCase();\n"
            "      if (rc.indexOf('taskbarplant') >= 0) continue;\n"
            "      if (rc.indexOf('plasmashell') >= 0) continue;\n"
            "      if (rc.indexOf('kwin') >= 0) continue;\n"
            "      if (rc.indexOf('xwaylandvideobridge') >= 0) continue;\n"
            "      var g = w.frameGeometry;\n"
            "      if (g.width < 120 || g.height < 80) continue;\n"
            "      var mn = 0, hd = 0;\n"
            "      try { if (w.minimized) mn = 1; } catch(e) {}\n"
            "      try { if (w.hidden) hd = 1; } catch(e) {}\n"
            "      o.push([Math.round(g.x),Math.round(g.y),"
            "Math.round(g.width),Math.round(g.height),mn,hd]);\n"
            "    } catch(e) {}\n"
            "  }\n"
            "  callDBus('org.taskbarplant.Geometry','/Geometry',"
            "'org.taskbarplant.Geometry','update', JSON.stringify(o));\n"
            "});\n"
            "t.start();\n");
    }

    bool    m_ok = false;
    QString m_mainJs;
};

const char* GeometryBridge::kService = "org.taskbarplant.Geometry";
const char* GeometryBridge::kPath    = "/Geometry";
const char* GeometryBridge::kIface   = "org.taskbarplant.Geometry";
const char* GeometryBridge::kPluginId = "taskbarplant-geometry";
#endif  // TBP_HAVE_DBUS

// =============================================================================
//  入口
// =============================================================================
int main(int argc, char** argv)
{
    // 最早的启动标记：不依赖 Qt，任何阶段崩溃都能看出"至少进到 main 了"
    fprintf(stderr, "[tbp] === start ===\n");
    fflush(stderr);

    // ------------------------------------------------------------------------
    //  强制走 X11(xcb) 后端
    //
    //  原因：本程序需要两个 Wayland 协议层不提供的能力 ——
    //    a) 精确的输入区域（只有花盆接收鼠标，其余穿透）—— setMask 在 xcb 下
    //       会同时设置 XShape 的 bounding 与 input shape，行为可靠；
    //    b) 枚举其它窗口的几何（花盆要站在窗口标题栏上并跟着窗口移动）。
    //  在 KDE/GNOME 上 xcb 走 XWayland，窗口照常显示、置顶、可交互，代价可忽略。
    //  想强制用原生 Wayland 可以设 TBP_PLATFORM=wayland，但会失去上面两个能力。
    // ------------------------------------------------------------------------
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        QByteArray forced = qgetenv("TBP_PLATFORM");
        if (!forced.isEmpty()) {
            qputenv("QT_QPA_PLATFORM", forced);
        } else if (!qgetenv("DISPLAY").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "xcb");
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("TaskbarPlant"));
    app.setApplicationDisplayName(QStringLiteral("桌面电子盆栽"));
    app.setApplicationVersion(VersionText());
    app.setQuitOnLastWindowClosed(false);

    // ---- 命令行参数 ----
    {
        QCommandLineParser cmd;
        cmd.setApplicationDescription(QStringLiteral("桌面电子盆栽 —— 会摔跤、会站在窗口上的像素盆栽"));
        cmd.addHelpOption();
        cmd.addVersionOption();

        QCommandLineOption optDelay(
            QStringList{QStringLiteral("u"), QStringLiteral("upright-delay")},
            QStringLiteral("摔倒后躺多久再自己立起来（秒）。设为 0 表示立刻回正。"),
            QStringLiteral("sec"),
            QStringLiteral("3"));
        QCommandLineOption optScale(
            QStringLiteral("scale"),
            QStringLiteral("整体缩放，默认 1.0（原版比例）。"),
            QStringLiteral("factor"),
            QStringLiteral("1.0"));
        QCommandLineOption optNoWilt(
            QStringLiteral("no-wilt"),
            QStringLiteral("关闭枯萎：叶子永远保持绿色，不掉枯叶、不需要浇水。"),
            QString(),
            QString());
        QCommandLineOption optDry(
            QStringLiteral("dry-seconds"),
            QStringLiteral("泥土从湿透到干透的秒数，默认 1800（30 分钟）。调小可以快速看到枯萎。"),
            QStringLiteral("sec"),
            QStringLiteral("1800"));
        QCommandLineOption optExport(
            QStringLiteral("export"),
            QStringLiteral("直接把第一个花盆导出成 PNG 后退出（不弹对话框，测试用）。"),
            QStringLiteral("path"));
        QCommandLineOption optProbe(
            QStringLiteral("probe-menu"),
            QStringLiteral("构建右键菜单但不弹出，输出尺寸后退出（无头测试用）。"));
        QCommandLineOption optDump(
            QStringLiteral("dump-plants"),
            QStringLiteral("把十三种植物渲染成一张 PNG 后退出。"),
            QStringLiteral("path"));
        QCommandLineOption optWeather(
            QStringLiteral("weather"),
            QStringLiteral("强制天气：rain / snow / clear。不给就按 20 秒一次的随机调度。"),
            QStringLiteral("kind"));
        cmd.addOption(optDelay);
        cmd.addOption(optScale);
        cmd.addOption(optNoWilt);
        cmd.addOption(optDry);
        cmd.addOption(optExport);
        cmd.addOption(optProbe);
        cmd.addOption(optDump);
        cmd.addOption(optWeather);
        cmd.process(app);

        // --export <path>：直接把第一盆导出成 PNG，不弹对话框。
        // 用来验证渲染和存盘本身，把"对话框"这个变量排除掉。
        if (cmd.isSet(QStringLiteral("export"))) {
            Garden g;
            g.start();
            if (g.pots().empty()) return 3;
            const bool ok = ExportPotPng(g.pots()[0], cmd.value(QStringLiteral("export")));
            tbpLog(QStringLiteral("export: direct %1").arg(ok ? QStringLiteral("ok")
                                                              : QStringLiteral("FAILED")));
            return ok ? 0 : 4;
        }

        if (cmd.isSet(QStringLiteral("probe-menu"))) {
            Garden g;
            g.start();
            QSize sz = g.window() ? g.window()->probeMenuSize() : QSize();
            tbpLog(QStringLiteral("menu: %1x%2").arg(sz.width()).arg(sz.height()));
            return sz.isEmpty() ? 2 : 0;
        }

        if (cmd.isSet(optDump)) {
            g_baseScale = 1.0f;
            g_scale = g_baseScale;
            return DumpPlants(cmd.value(optDump)) ? 0 : 1;
        }

        bool ok = false;

        g_wiltEnabled = !cmd.isSet(optNoWilt);

        // 注意：cmd.value() 在参数没给时返回的是默认值 "1800"，
        // 所以必须再判一次 isSet —— 否则默认值会一直被当成"用户显式指定"，
        // 把 ini 里存的 dry 顶掉。
        float dry = cmd.value(optDry).toFloat(&ok);
        if (ok && cmd.isSet(optDry) && dry >= 5.0f && dry <= 86400.0f) {
            g_drySeconds = dry;
            g_dryFromCli = true;
        } else if (ok) {
            fprintf(stderr, "[tbp] --dry-seconds 需要 5~86400 之间的秒数，已用默认 1800\n");
        }

        float d = cmd.value(optDelay).toFloat(&ok);
        if (ok && d >= 0.0f && d <= 120.0f) {
            g_uprightDelay = d;
        } else {
            fprintf(stderr, "[tbp] --upright-delay 需要 0~120 之间的秒数，已用默认值 %.0f\n",
                    g_uprightDelay);
            fflush(stderr);
        }

        d = cmd.value(optScale).toFloat(&ok);
        if (ok && d > 0.3f && d < 5.0f) {
            // Garden::start() 里会重新定 g_scale（它会读 TBP_SCALE），
            // 所以命令行值必须写回环境变量才不会被覆盖。
            g_baseScale = d;
            g_scale = d;
            qputenv("TBP_SCALE", QByteArray::number(d, 'f', 2));
        }

        fprintf(stderr, "[tbp] upright-delay=%.1fs scale=%.2f dry=%.0fs wilt=%s\n",
                g_uprightDelay, g_scale, g_drySeconds,
                g_wiltEnabled ? "on" : "off");
        fflush(stderr);

        // --weather rain|snow|clear：强制天气，用来截图 / 无头验证
        if (cmd.isSet(optWeather)) {
            const QString w = cmd.value(optWeather).toLower();
            if (w == QStringLiteral("rain")) {
                g_weather = WEATHER_RAIN;  g_weatherLeft = 1e9f;  g_lastWeather = WEATHER_RAIN;
            } else if (w == QStringLiteral("snow")) {
                g_weather = WEATHER_SNOW;  g_weatherLeft = 1e9f;  g_lastWeather = WEATHER_SNOW;
            } else if (w == QStringLiteral("clear")) {
                g_weather = WEATHER_CLEAR; g_weatherLeft = 0.0f;
            } else {
                fprintf(stderr, "[tbp] --weather 只认 rain / snow / clear\n");
            }
            fprintf(stderr, "[tbp] weather forced: %s\n", qPrintable(w));
            fflush(stderr);
        }
    }

    srand((unsigned)time(nullptr));

#if defined(TBP_HAVE_DBUS)
    tbpLog(QStringLiteral("qt: platform=%1").arg(QGuiApplication::platformName()));
    {
        QScreen* s = QGuiApplication::primaryScreen();
        PanelInset ins = detectPanelInset(s->geometry());
        tbpLog(QStringLiteral("screen: geom=%1x%2 avail=%3x%4 inset(t=%5 b=%6 l=%7 r=%8)")
                   .arg(s->geometry().width()).arg(s->geometry().height())
                   .arg(s->availableGeometry().width())
                   .arg(s->availableGeometry().height())
                   .arg(ins.top).arg(ins.bottom).arg(ins.left).arg(ins.right));
    }
#else
    qInfo("[tbp] qt: platform=%s",
          qPrintable(QGuiApplication::platformName()));
#endif

    Garden garden;

#if defined(TBP_HAVE_DBUS)
    // ---- KWin 窗口几何桥 ----
    // Wayland 下只有合成器知道别的应用窗口在哪，所以让 KWin 主动推过来。
    GeometryBridge bridge;
    bridge.install();

    // 自检：3 秒后还没收到任何推送，说明桥没通，直接在终端里说出来
    QTimer::singleShot(3000, []() {
        if (g_kwinLive) {
            tbpLog(QStringLiteral("selfcheck: OK 收到 KWin 推送，共 %1 个窗口")
                       .arg(g_kwinGeoms.size()));
        } else {
            tbpLog(QStringLiteral("selfcheck: FAIL 3 秒内没收到 KWin 推送 —— "
                                  "花盆只能停在屏幕底边"));
        }
    });
#endif

    garden.start();

    // 开发用：TBP_DEBUG_BAR=1 直接开隐藏调试面板，跳过连点 7 次。
    // 没有这个开关的话，每次调样式都得手动右键点七下，没法自动化验证。
    if (qEnvironmentVariableIsSet("TBP_DEBUG_BAR"))
        garden.enableDebugMode();

    // 开发用：TBP_EDIT=1 直接进编辑模式
    if (qEnvironmentVariableIsSet("TBP_EDIT"))
        garden.setEditMode(true);

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&garden]() { garden.tick(); });
    timer.start(ANIM_INTERVAL);

    // TBP_SCENE_PNG=<path> [TBP_SCENE_AFTER=秒]：跑若干秒后把整个画面截图再退出。
    // 天气这种东西（云 / 雨 / 雪 / 积水）只能看渲染结果验收，读代码没用。
    {
        const QByteArray shot = qgetenv("TBP_SCENE_PNG");
        if (!shot.isEmpty()) {
            int secs = qEnvironmentVariableIntValue("TBP_SCENE_AFTER");
            if (secs <= 0) secs = 4;
            QTimer::singleShot(secs * 1000, [&garden, shot]() {
                if (garden.window())
                    garden.window()->grab().save(QString::fromLocal8Bit(shot));
                tbpLog(QStringLiteral("scene: saved '%1'").arg(QString::fromLocal8Bit(shot)));
                qApp->quit();
            });
        }
    }

    // 诊断：lastWindowClosed 若在这里触发，说明 Qt 认为没有"主窗口"了。
    // PotWindow 是 Qt::Tool，按 Qt 的定义不算 primary window。
    QObject::connect(&app, &QGuiApplication::lastWindowClosed, []() {
        tbpLog(QStringLiteral("app: last window closed, exiting"));
    });
    app.setQuitOnLastWindowClosed(false);

    const int rc = app.exec();
    tbpLog(QStringLiteral("event loop exited rc=%1").arg(rc));
    return rc;
}

#include "main.moc"
