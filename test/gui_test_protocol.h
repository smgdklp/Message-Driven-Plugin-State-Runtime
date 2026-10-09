/* ============================================================================
 *  gui_test_protocol.h —— GUI 测试插件与 GUI 自测共用的"一张表"
 *
 *  ★ 这个头文件同时被 [(test/) 三个测试插件] 和 [GUI 自测 test/gui_selftest.cpp]
 *    include。三个插件的几何/颜色/节奏只在下面 kGtcProfiles[] 里写一遍, 插件和
 *    自测各自读同一份 —— 不会出现"测试里写的期望值和插件实际画的不一样"。
 *
 *  三个插件的定位 (都是"透明底 + 一个纯色圆"):
 *      circ_a  图比窗口小    -> 窗口里没图的地方必须透明; 换色动画; 可点击关闭
 *      circ_b  图比窗口大    -> 系统裁剪; 缩放动画; 整窗可拖 (客户端点名)
 *      circ_c  放大 2 倍     -> 缩放 + 偏置 (含负偏置, 出界部分透明); 可点击关闭
 *
 *  这不是框架的一部分: 删掉 test/ 整个目录, 框架与 winmsg 一样跑。
 * ==========================================================================*/
#ifndef MDPSR_GUI_TEST_PROTOCOL_H
#define MDPSR_GUI_TEST_PROTOCOL_H

#include "mdpsr/abi.h"

#include <math.h>

/* ---- 命令: 测试客户端自己的 Tick 之外, 再加两条"给自测用"的 ---- */
#define GTC_CMD_TICK   (MDPSR_CMD_USER_BASE + 10)   /* 自己给自己的一拍 (和 paint 一致) */
#define GTC_CMD_PAUSE  (MDPSR_CMD_USER_BASE + 11)   /* body = uint32 on; 暂停/继续改画面 */
#define GTC_CMD_REOPEN (MDPSR_CMD_USER_BASE + 12)   /* 主动把窗口关掉再要一个 (测自愈) */

/* ---- 行为模式 ---- */
#define GTC_MODE_ANIM   0u   /* 颜色随帧变 + 偏置来回摆 */
#define GTC_MODE_SCALE  1u   /* 缩放来回变 (顺带颜色变) */
#define GTC_MODE_CLIP   2u   /* 放大固定倍数 + 偏置摆到出界 */

#define GTC_STAT_MAGIC 0x47544331u   /* "GTC1" */

/* ==========================================================================
 *  画像: 一个测试客户端的全部参数
 * ==========================================================================*/
typedef struct GtcProfile {
    const char* plugin;        /* 插件名 (也是条目名前缀) */
    const char* window;        /* 窗口名 */
    uint32_t img_w, img_h;     /* 内存图片尺寸 */
    int32_t  win_x, win_y;     /* 窗口初始位置 (屏幕绝对坐标) */
    int32_t  win_w, win_h;     /* 窗口初始尺寸 */
    uint32_t pace_ms;          /* 队列限速 = 动画时钟 */
    uint32_t base_argb;        /* 基础颜色 0xAARRGGBB */
    float    scale_x, scale_y; /* 固定缩放 (0 = 1.0); GTC_MODE_SCALE 时由 tick 改 */
    int32_t  off_x0, off_x1;   /* 偏置 (原图左上角) 的摆动范围, 含端点 */
    int32_t  off_y0, off_y1;
    uint32_t mode;
    uint32_t draggable;        /* 客户端要不要整窗可拖 */
    uint32_t close_on_click;   /* 收到 MOUSE_DOWN 就 winmsg_close */
} GtcProfile;

static const GtcProfile kGtcProfiles[3] = {
    /* 0 */ { "circ_a", "circ_a.Window",  96u,  96u, 120, 120, 240, 180,  8u,
              0x0060A0FFu, 1.0f, 1.0f,   0, 144,   0,  84, GTC_MODE_ANIM,  0u, 1u },
    /* 1 */ { "circ_b", "circ_b.Window", 160u, 120u, 420, 140, 120,  90, 12u,
              0x0020C0FFu, 1.0f, 1.0f,   0,   0,   0,   0, GTC_MODE_SCALE, 1u, 0u },
    /* 2 */ { "circ_c", "circ_c.Window",  48u,  48u, 200, 380, 200, 150, 16u,
              0x0040FF40u, 2.0f, 2.0f, -24,  60, -24,  40, GTC_MODE_CLIP,  0u, 1u },
};
#define GTC_PROFILE_COUNT 3u

/* ==========================================================================
 *  客户端统计 (存在画面载荷的扩展区里, 自测用 winmsg_surface_extra 取)
 * ==========================================================================*/
typedef struct GtcStat {
    uint32_t magic;
    uint32_t ticks;           /* 发布过多少版画面 */
    uint32_t paused;          /* 自测让它暂停了 (改画面的只有自测) */
    uint32_t reopens;         /* 窗口被回收后又重新要了几次 */
    uint32_t events_rx;
    uint32_t ev_ready;
    uint32_t ev_close;
    uint32_t ev_destroyed;
    uint32_t ev_key;
    uint32_t ev_mouse;
    uint32_t ev_wheel;
    uint32_t ev_char;
    uint32_t ev_focus;
    uint32_t img_pos_x, img_pos_y;   /* 最后一版用的偏置 */
    float    scale_x, scale_y;       /* 最后一版用的缩放 */
    uint32_t color;                  /* 最后一版画进去的颜色 0xAARRGGBB */
    uint32_t last_mx, last_input_seq;
    uint32_t seen_visible;           /* 客户端自己从状态里读到的 visible */
    uint32_t draggable;              /* 已经点开整窗可拖了吗 */
    uint32_t closed_by_click;        /* 因为点击而请求关窗的次数 */
    uint32_t closed_by_close;        /* 因为 CLOSE 事件而请求关窗的次数 */
} GtcStat;

/* ==========================================================================
 *  参考光栅器 —— "如果 broker 是对的, 画布上这个点应该是什么颜色"
 *
 *  和 winmsg 的实现在同一套约定上:
 *      dst = img_pos + src * scale      (src 是原图坐标; 最近邻, floor 取整)
 *      画布之外不写 (= 0), 圆外 alpha=0 (= 0)
 *  broker 的实现见 src/components/winmsg/winmsg.cpp 的 winmsg_paint_one。
 * ==========================================================================*/

/* 原图坐标 (isx, isy) 是不是落在那个纯色圆里 —— 和 winmsg_draw_circle 同一公式 */
static inline int gtc_solid(const GtcProfile* p, int isx, int isy) {
    const float cx = (float)p->img_w * 0.5f, cy = (float)p->img_h * 0.5f;
    const float rad = (float)(p->img_w < p->img_h ? p->img_w : p->img_h) * 0.42f;
    const float dx = (float)isx - cx, dy = (float)isy - cy;
    return (dx * dx + dy * dy) <= rad * rad;
}
/* 离圆边缘还有多远 (像素): >0 在圆里, <0 在圆外。自测用它避开边界像素。 */
static inline float gtc_edge_margin(const GtcProfile* p, int isx, int isy) {
    const float cx = (float)p->img_w * 0.5f, cy = (float)p->img_h * 0.5f;
    const float rad = (float)(p->img_w < p->img_h ? p->img_w : p->img_h) * 0.42f;
    const float dx = (float)isx - cx, dy = (float)isy - cy;
    return rad - sqrtf(dx * dx + dy * dy);
}

typedef struct GtcExpect {
    int      in_image;     /* 这个画布点在缩放后的图像范围内吗 */
    int      solid;        /* 在范围内的话: 落点的原图像素是圆内(不透明)还是圆外(透明) */
    int      isx, isy;     /* 反查出来的原图坐标 */
    float    margin;       /* 离圆边缘的距离 (像素) */
    uint32_t argb;         /* 期望的画布像素 (0 = 透明) */
} GtcExpect;

static inline GtcExpect gtc_expect(const GtcProfile* p, uint32_t color,
                                   float sx, float sy, int32_t ox, int32_t oy,
                                   int x, int y) {
    GtcExpect e;
    e.in_image = 0; e.solid = 0; e.isx = -1; e.isy = -1; e.margin = -1e9f; e.argb = 0u;
    if (sx == 0.0f) sx = 1.0f;
    if (sy == 0.0f) sy = 1.0f;
    const int isx = (int)floorf(((float)x - (float)ox) / sx);
    const int isy = (int)floorf(((float)y - (float)oy) / sy);
    if (isx < 0 || isy < 0 || isx >= (int)p->img_w || isy >= (int)p->img_h) return e;
    e.in_image = 1;
    e.isx = isx; e.isy = isy;
    e.margin = gtc_edge_margin(p, isx, isy);
    e.solid  = gtc_solid(p, isx, isy);
    /* 纯色圆: 圆内 = 不透明纯色 (预乘后 BGR 不变, alpha=255), 圆外 = 一个像素都不写 */
    e.argb = e.solid ? (0xFF000000u | (color & 0x00FFFFFFu)) : 0u;
    return e;
}

#endif /* MDPSR_GUI_TEST_PROTOCOL_H */
