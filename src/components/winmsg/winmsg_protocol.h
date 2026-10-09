/* ============================================================================
 *  winmsg_protocol.h —— winmsg (GUI broker) 与客户端之间的协议
 *
 *  ★ 纯 C99, 不含任何 Windows 头文件。
 *    客户端插件 include 这一个头就够了, 完全不需要知道 HWND/HDC/MSG 长什么样。
 *
 *  两条通道 (一条都不能省, 因为它们解决的问题不同):
 *
 *    画面 = 状态 :  "最新一版"才有意义, 中间的被覆盖掉无所谓 -> 走 State + content_seq
 *    事件 = 消息 :  每一条都要处理 (按下/抬起/滚轮/按键/关闭) -> 走 emit 到客户端的 handle
 *    高频输入    :  鼠标移动一秒上百条, 走状态快照 (slot 里的 mx/my/buttons/input_seq)
 *
 *  ★ 渲染管线 (三段, 顺序不能换):
 *
 *      ① 缩放   surf.scale_x / scale_y   (0 = 1.0; 负值 = 镜像)
 *      ② 偏置   surf.img_pos             (**原图**左上角落在客户区的哪个点)
 *      ③ 写画布 画布和窗口一样大, 初始 alpha=0
 *      ④ 系统裁剪: 画出画布之外的部分根本不写, 窗口之外的部分由系统裁掉
 *         —— 所以"出界"不需要任何特判: 没写到的地方就是 alpha=0 = 透明。
 *
 *    坐标原点统一是**左上角** (和 Win32 一致): 客户区原点在窗口左上角, x 向右,
 *    y 向下; img_pos 是"原图左上角"在这个坐标系里的位置, 缩放以该点为锚点向右下展开。
 *
 *  资源分工 (完全遵守框架的借用约定, 没有任何跨插件裸指针的所有权转移):
 *
 *    winmsg.Screen   (broker 的 State, 公用)  目录: 窗口名 -> 目录项
 *    winmsg.Shelf    (broker 的 Object, 私有) HWND 表 + GUI 线程 (别人借不到)
 *    winmsg.Control  (broker 的 Handle)      控制入口 (启动 GUI 线程等)
 *    <客户端>.Surface (客户端自己的 State)    画面: 图像指针 + 缩放 + 偏置 + 序号
 *
 *  ★ 为什么画面必须放在"客户端自己的 State"里:
 *    broker 贴图时要 acquire 它 —— 那一把锁既是互斥, 更是"我正在用它"的声明,
 *    它把客户端钉住不让它卸载 (卸载方抢不到锁就 BUSY 重试), 所以 pixels 这个
 *    裸指针在贴图期间永远有效。跳过 acquire 直接读 pixels = 拿悬垂指针。
 * ==========================================================================*/
#ifndef WINMSG_PROTOCOL_H
#define WINMSG_PROTOCOL_H

#include "mdpsr/abi.h"

/* ---- 条目名约定 ---- */
#define WINMSG_SCREEN_NAME   "winmsg.Screen"    /* broker 的目录 (State) */
#define WINMSG_SHELF_NAME    "winmsg.Shelf"     /* broker 的 HWND 架 (Object) */
#define WINMSG_CONTROL_NAME  "winmsg.Control"   /* broker 的控制入口 (Handle) */
#define WINMSG_SURFACE_SUFFIX ".Surface"        /* 客户端必须声明的 State: "<插件名>.Surface" */

#define WINMSG_MAGIC         0x57494d34u   /* "WIM4" */
#define WINMSG_SURFACE_MAGIC 0x57495334u   /* "WIS4" */
#define WINMSG_VERSION       2u

#define WINMSG_MAX_WINDOWS   16u           /* 目录定容; 满了 winmsg_open 返回 NO_SPACE */
#define WINMSG_PIXEL_FORMAT_BGRA 0u        /* 32bpp, 字节序 B,G,R,A, 自上而下 */

/* "这一维不要动" 的哨兵 (0 是合法的坐标, 所以不能用 0 当"不改") */
#define WINMSG_KEEP          0x7FFFFFFF

/* ---- 两个几何参数的类型 ---- */
/* RECT 风格: 窗口在屏幕上的**绝对**位置与尺寸 */
typedef struct winmsg_rect { int32_t x, y, w, h; } winmsg_rect;
/* POINT 风格: **原图左上角**在窗口客户区里的位置 (缩放以它为锚点, 图像向右下展开) */
typedef struct winmsg_point { int32_t x, y; } winmsg_point;

/* ---- 命令号 (组件自己的空间, 从框架保留区之后开始) ---- */
#define WINMSG_CMD_EVENT     (MDPSR_CMD_USER_BASE + 0)   /* 16: broker -> 客户端 (窗口事件) */
#define WINMSG_CMD_STATS     (MDPSR_CMD_USER_BASE + 1)   /* 17: 问 broker 要统计 (Control) */
#define WINMSG_CMD_CLOSE_ALL (MDPSR_CMD_USER_BASE + 2)   /* 18: 让 broker 关掉所有窗口 */
#define WINMSG_CMD_INJECT    (MDPSR_CMD_USER_BASE + 3)   /* 19: ★只给自测用: 注入合成输入 */
#define WINMSG_CMD_PROBE     (MDPSR_CMD_USER_BASE + 4)   /* 20: ★只给自测用: 读窗口某个像素 */

/* ---- slot.flags (客户端写) ---- */
#define WINMSG_F_ALIVE        0x0001u
#define WINMSG_F_WANT_CREATE  0x0002u
#define WINMSG_F_WANT_CLOSE   0x0004u
/* ★ 整窗可拖: 只有客户端点名要了, broker 才把命中测试返回成"标题栏"。
 *   代价写在 doc/GUI.md: 那扇窗的真实鼠标按下/抬起会被系统当拖拽吃掉。
 *   默认不开 —— 默认情况下窗口是普通客户区, 点击/抬起/滚轮全都发到客户端。 */
#define WINMSG_F_DRAGGABLE    0x0008u

/* ---- slot.state (broker 写, 客户端轮询) ---- */
#define WINMSG_S_EMPTY   0u
#define WINMSG_S_PENDING 1u   /* 请求已登记, 窗口还没建出来 */
#define WINMSG_S_READY   2u   /* 窗口在, 可以提交画面 */
#define WINMSG_S_FAILED  3u   /* 建窗失败, 看 last_error */

/* ---- 注入种类 (只给自测用: slot.inject_req 的取值) ---- */
#define WINMSG_INJECT_BURST 1u   /* 一批合成输入 + 一次方向键 */
#define WINMSG_INJECT_CLICK 2u   /* 在客户区中心来一次真实的左键按下+抬起 */
#define WINMSG_INJECT_CLOSE 3u   /* 给窗口发一条 WM_CLOSE (等于 Alt+F4) */

/* ---- 事件种类 ---- */
#define WINMSG_EV_READY       1   /* 窗口建好 (发一次; 客户端可以立刻反应, 自测也靠它) */
#define WINMSG_EV_CLOSE       2   /* 用户要关窗 (Alt+F4 / 关闭) */
#define WINMSG_EV_DESTROYED   3   /* 窗口已销毁 (最后一次) */
#define WINMSG_EV_FOCUS_IN    4
#define WINMSG_EV_FOCUS_OUT   5
#define WINMSG_EV_KEY_DOWN    6
#define WINMSG_EV_KEY_UP      7
#define WINMSG_EV_CHAR        8
#define WINMSG_EV_MOUSE_DOWN  9
#define WINMSG_EV_MOUSE_UP   10
#define WINMSG_EV_WHEEL      11
#define WINMSG_EV_MOUSE_MOVE 12   /* 默认不发 (走状态快照); 只有客户端点名要才发 */
#define WINMSG_EV__COUNT     13

#define WINMSG_EV_BIT(k)  (1u << (k))

/* ---- 按钮 / 修饰键 ---- */
#define WINMSG_BTN_LEFT    0x1u
#define WINMSG_BTN_RIGHT   0x2u
#define WINMSG_BTN_MIDDLE  0x4u
#define WINMSG_MOD_SHIFT   0x1u
#define WINMSG_MOD_CTRL    0x2u
#define WINMSG_MOD_ALT     0x4u

/* ==========================================================================
 *  事件体 (broker -> 客户端)。固定长度 POD, 没有指针 / 没有 HWND / 没有 MSG。
 * ==========================================================================*/
typedef struct winmsg_event {
    int32_t  _cmd;      /* = WINMSG_CMD_EVENT, 遵守"body 前 4 字节是 _cmd"的约定 */
    int32_t  kind;      /* WINMSG_EV_* */
    int32_t  code;      /* 按 kind 解释: VK 码 / 字符 / 按钮位 / 滚轮增量 */
    int32_t  x, y;      /* 客户区坐标 (鼠标类事件) */
    uint32_t mods;      /* WINMSG_MOD_* */
    uint32_t pad;
    uint64_t window;    /* 哪个窗口 (窗口名哈希) */
} winmsg_event;

/* ==========================================================================
 *  目录项 (客户端写第一组, broker 写第二组)
 * ==========================================================================*/
typedef struct winmsg_slot {
    /* --- 客户端写 --- */
    uint64_t name;          /* 窗口名 = mdpsr_hash64("插件名.窗口名"), 全局唯一 */
    uint64_t owner;         /* 自己 = current_plugin() */
    uint64_t surface_key;   /* 自己那张画面条目的键 */
    uint64_t handle;        /* 窗口事件发到哪个 handle; 0 = 不要任何消息 */
    uint32_t events;        /* 要哪些事件 (WINMSG_EV_BIT 或起来) */

    /* ★ 参数 1: 窗口的绝对位置与尺寸 (RECT 风格)。
     *   想挪窗口/改大小就填这里, 然后 win_seq++。
     *   用序号而不是布尔标志, 是为了不丢请求 (和 content_seq 同一个道理)。 */
    winmsg_rect win;
    uint32_t    win_seq;

    /* --- broker 写 --- */
    uint32_t owner_gen;     /* 请求方代际; 对不上就说明它换代了 -> 拆窗回收 */
    uint32_t flags;         /* WINMSG_F_xxx: ALIVE / WANT_CREATE / WANT_CLOSE / DRAGGABLE */
    uint32_t state;         /* WINMSG_S_* */
    uint32_t last_error;
    int32_t  mx, my;        /* ★ 高频输入快照: 客户区坐标 */
    uint32_t buttons;       /* ★ WINMSG_BTN_* 位图 */
    uint32_t input_seq;     /* ★ 上面三样每次变化 ++ */
    uint32_t events_sent;   /* 累计发出的事件数 */
    uint32_t events_dropped;/* 客户端队列满而丢掉的事件数 */
    uint32_t visible;       /* ★ broker 写: 这扇窗现在真的 IsWindowVisible 吗 (0/1) */
    uint32_t inject_req;    /* ★只给自测用: WINMSG_INJECT_* */
    int32_t  probe_x, probe_y;   /* ★只给自测用: 要采样的客户区坐标 */
    uint32_t probe_seq;          /* ★只给自测用: ++ 表示"请采一次" */
    uint32_t probe_done;         /* 已经采到哪一版 */
    uint32_t probe_color;        /* 采到的颜色 0x00BBGGRR; 0xFFFFFFFF = 采样失败 */
    uint32_t win_done;      /* 已经执行到哪一版 window RECT */
    uint32_t win_moves;     /* 累计真的挪过几次 (诊断) */
    winmsg_rect cur;        /* 窗口**实际**的矩形 (从系统读回来的, 客户端可以查) */
} winmsg_slot;

/* ==========================================================================
 *  目录 (broker 的 State 载荷)
 * ==========================================================================*/
typedef struct winmsg_screen {
    uint32_t magic;
    uint32_t version;
    uint32_t slots_used;
    uint32_t windows_open;       /* broker 当前真正开着几个窗口 */
    uint32_t creates_total;
    uint32_t destroys_total;
    uint32_t paints_total;
    uint32_t invalidates_total;
    uint32_t events_sent_total;
    uint32_t events_dropped_total;
    uint32_t scans;              /* GUI 线程转了 compat 多少圈 (诊断) */
    uint32_t frame_ms;           /* broker 的帧间隔 */
    uint32_t last_error;
    uint32_t pad;
    winmsg_slot slots[WINMSG_MAX_WINDOWS];
} winmsg_screen;

/* ==========================================================================
 *  画面 (客户端自己的 State 载荷)
 *
 *  ★ 内存图像的格式约定 (4 通道, 不要给 3 通道 BGR):
 *      · 每像素 4 字节, 内存里从低到高是 B, G, R, A  (当 uint32_t 读 = 0xAARRGGBB)
 *      · A = 0 完全透明 / 255 完全不透明 / 中间值按比例混合
 *      · 行序自上而下 (第 0 行是图像的顶行); stride = 字节/行, 0 表示 img_w*4
 *      · 给的是"直通" alpha, 不用预乘 (预乘由 broker 贴图时做)
 *      · broker 1:1 采样后按 scale 缩放, 不做平滑 (最近邻), 所以放大就是像素块
 *
 *  ★ 渲染管线 (改完任意一项就 content_seq++, 这就是"发布了新一版"):
 *
 *      dst = img_pos + (src - 0) * scale        (src 是原图坐标, 左上角为原点)
 *      也就是: ① 以**原图左上角**为锚点缩放  ② 把锚点摆到 img_pos  ③ 写画布
 *      ④ 画布之外不写 (= 透明), 窗口之外由系统裁掉
 *
 *    img_pos 用的是**客户区坐标** (左上角原点, x 右 y 下), 和 Win32 一致。
 *    scale 传 0 表示 1.0; 负值 = 镜像 (x 或 y 方向翻转)。
 * ==========================================================================*/
typedef struct winmsg_surface {
    uint32_t magic;
    uint32_t version;
    /* --- 客户端写 (必须在 acquire/release 之间改) --- */
    void*    pixels;        /* 图像基址, BGRA 四通道, 自上而下 */
    uint32_t img_w, img_h;  /* 整张图尺寸 */
    uint32_t stride;        /* 字节/行; 0 = img_w * 4 */
    uint32_t format;        /* WINMSG_PIXEL_FORMAT_BGRA */

    /* ★ 参数 2 之一: 缩放 (先缩放)。
     *   0 表示 1.0; 负值 = 镜像。缩放锚点是**原图左上角**。 */
    float    scale_x, scale_y;

    /* ★ 参数 2 之二: 原图左上角在窗口客户区里的位置 (后偏置)。
     *   窗口是透明画布, 画布之外(含窗口外)天然什么都不画 -> 出界即透明。 */
    winmsg_point img_pos;

    uint32_t content_seq;   /* 以上任意一项改完 ++   <- broker 靠它判"要不要重画" */

    /* --- broker 写 (客户端可读, 自测也读它) --- */
    uint32_t painted_seq;   /* broker 已经贴过的那一版 */
    uint32_t paints;        /* 本窗口被贴图次数 */
    uint32_t invalidates;   /* 本窗口被请求重绘次数 */
    int32_t  last_dst_x, last_dst_y, last_dst_w, last_dst_h;  /* 真正写进画布的矩形 */
    int32_t  last_img_x, last_img_y;                          /* 这次用的 img_pos */
    float    last_scale_x, last_scale_y;                       /* 这次用的缩放 */
    uint32_t last_error;
} winmsg_surface;

/* ==========================================================================
 *  State 工厂里用的分配助手 (客户端调): 一块分配"载荷 + 像素", 像素 16 字节对齐
 * ==========================================================================*/
static inline void* winmsg_surface_make_ex(const mdpsr_factory_ctx* ctx,
                                           uint32_t img_w, uint32_t img_h,
                                           uint32_t extra_bytes) {
    if (!ctx || !ctx->pool || !ctx->host || img_w == 0 || img_h == 0) return NULL;
    const size_t head = sizeof(winmsg_surface);
    const size_t pix = (size_t)img_w * 4u * (size_t)img_h;
    const size_t need = ((head + 15u) & ~(size_t)15u) + pix + extra_bytes;
    void* mem = mdpsr_fctx_alloc(ctx, need, 16);
    if (!mem) return NULL;
    unsigned char* base = (unsigned char*)mem;
    winmsg_surface* s = (winmsg_surface*)mem;
    for (size_t i = 0; i < sizeof(*s); ++i) base[i] = 0;
    s->magic     = WINMSG_SURFACE_MAGIC;
    s->version   = WINMSG_VERSION;
    s->pixels    = base + ((head + 15u) & ~(size_t)15u);   /* 像素区紧跟在头后面 */
    s->img_w     = img_w;
    s->img_h     = img_h;
    s->stride    = img_w * 4u;
    s->format    = WINMSG_PIXEL_FORMAT_BGRA;
    s->scale_x   = 1.0f;   /* 默认原尺寸 */
    s->scale_y   = 1.0f;
    s->img_pos.x = 0;      /* 默认: 原图左上角贴客户区左上角 */
    s->img_pos.y = 0;
    s->content_seq = 1;    /* 第一版从 1 开始, 0 表示"还没发布过" */
    return mem;
}

static inline void* winmsg_surface_make(const mdpsr_factory_ctx* ctx,
                                        uint32_t img_w, uint32_t img_h) {
    return winmsg_surface_make_ex(ctx, img_w, img_h, 0);
}

/* 客户端自己带的扩展数据 (统计计数之类) 紧跟在像素区之后 */
static inline void* winmsg_surface_extra(winmsg_surface* s) {
    if (!s || !s->pixels) return NULL;
    return (unsigned char*)s->pixels + (size_t)s->stride * (size_t)s->img_h;
}

#endif /* WINMSG_PROTOCOL_H */
