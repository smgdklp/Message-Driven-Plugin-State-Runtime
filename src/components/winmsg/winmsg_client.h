/* ============================================================================
 *  winmsg_client.h —— 客户端插件用的助手 (纯 C99, 无 Windows 头文件)
 *
 *  客户端要做的全部事情就是这四个动作:
 *
 *      1. 在清单里声明一个 State:  "State": ["<插件名>.Surface"]
 *         工厂里调 winmsg_surface_make(ctx, 图宽, 图高)  ← 图像内存从自己的池里出
 *
 *      2. winmsg_open(...)      登记一个窗口 (位置/尺寸)
 *         winmsg_subscribe(...)  声明"窗口事件发到我哪个 handle, 要哪些事件"
 *
 *      3. 每一帧: winmsg_lock_ex() -> 改像素 / 缩放 / 偏置 -> winmsg_unlock(changed=1)
 *         (改完 content_seq++ 才算"发布了新一版", broker 靠它决定要不要重画)
 *
 *      4. 事件到了 (自己的队列线程上收到 cmd = WINMSG_CMD_EVENT):
 *         把 body 按 winmsg_event 解析, 按 kind 处理
 *         高频输入 (鼠标在哪、哪个键按着) 不用消息, 用 winmsg_read_input() 轮询
 *
 *  ★ 两条硬规则:
 *    a) pixels 是裸指针, 但**只在 winmsg_lock_ex 到 winmsg_unlock 之间有效**。
 *       那把锁同时是"我正在用它"的声明, 它把客户端钉住不让它卸载 —— 绕过锁
 *       直接读 pixels 就是拿悬垂指针。
 *    b) winmsg_state() 返回 0 = 窗口没了 (用户关了 / broker 回收了)。这时**重新
 *       winmsg_open(同一个名字)** 就能再要一个 —— 名字就是句柄, 不需要新名字。
 *       热插拔(重载)之后客户端就是这么自己把窗口要回来的。
 * ==========================================================================*/
#ifndef WINMSG_CLIENT_H
#define WINMSG_CLIENT_H

#include <string.h>
#include "winmsg_protocol.h"

/* 借到目录表 (内部用) */
static inline winmsg_screen* winmsg__screen_lock(const mdpsr_host* H) {
    void* p = NULL;
    uint32_t g = 0;
    if (mdpsr_acquire(H, mdpsr_hash64(WINMSG_SCREEN_NAME), 0, &p, &g) != MDPSR_OK) return NULL;
    winmsg_screen* sc = (winmsg_screen*)p;
    if (!sc || sc->magic != WINMSG_MAGIC) {
        mdpsr_release(H, mdpsr_hash64(WINMSG_SCREEN_NAME));
        return NULL;
    }
    return sc;
}
static inline void winmsg__screen_unlock(const mdpsr_host* H) {
    mdpsr_release(H, mdpsr_hash64(WINMSG_SCREEN_NAME));
}
static inline winmsg_slot* winmsg__find(winmsg_screen* sc, uint64_t name) {
    for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
        if ((sc->slots[i].flags & WINMSG_F_ALIVE) && sc->slots[i].name == name) return &sc->slots[i];
    }
    return NULL;
}
static inline winmsg_slot* winmsg__find_free(winmsg_screen* sc) {
    for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
        if (!(sc->slots[i].flags & WINMSG_F_ALIVE)) return &sc->slots[i];
    }
    return NULL;
}
static inline uint64_t winmsg__self(const mdpsr_host* H) {
    return H ? H->current_plugin(H->self) : 0;
}
static inline uint32_t winmsg__self_gen(const mdpsr_host* H, uint64_t self) {
    mdpsr_plugin_info pi;
    memset(&pi, 0, sizeof(pi));
    pi.struct_size = sizeof(pi);
    if (!self || H->plugin_info(H->self, self, &pi) != MDPSR_OK) return 0;
    return pi.gen;
}

/* --------------------------------------------------------------------------
 *  画面: 借 -> 改 -> 还 (放在最前面, 因为下面的助手要用)
 *
 *  winmsg_lock_ex 会告诉你**为什么**没借到 —— "忙"(BUSY) 是正常的, 下轮再来;
 *  "没了"(NOT_FOUND / ENTRY_INVALID) 才是窗口/资源真的走了。
 *  winmsg_lock 是老写法的糖: 借不到就 NULL (演示用, 别用它写关键路径)。
 * ------------------------------------------------------------------------*/
static inline int winmsg_lock_ex(const mdpsr_host* H, uint64_t surface_key, winmsg_surface** out) {
    if (out) *out = NULL;
    if (!H || !surface_key) return MDPSR_ERR_NULLPTR;
    void* p = NULL;
    uint32_t g = 0;
    const int rc = mdpsr_acquire(H, surface_key, 0, &p, &g);
    if (rc != MDPSR_OK) return rc;
    winmsg_surface* s = (winmsg_surface*)p;
    if (!s || s->magic != WINMSG_SURFACE_MAGIC || !s->pixels) {
        mdpsr_release(H, surface_key);
        return MDPSR_ERR_BAD_STATE;
    }
    if (out) *out = s;
    return MDPSR_OK;
}
static inline winmsg_surface* winmsg_lock(const mdpsr_host* H, uint64_t surface_key) {
    winmsg_surface* s = NULL;
    return winmsg_lock_ex(H, surface_key, &s) == MDPSR_OK ? s : NULL;
}
/* changed != 0 才会 content_seq++ (没变化就别让 broker 白重画一次) */
static inline void winmsg_unlock(const mdpsr_host* H, uint64_t surface_key,
                                winmsg_surface* s, int changed) {
    if (s && changed) s->content_seq++;
    mdpsr_release(H, surface_key);
}

/* 推导自己那张画面条目的键: "<我的插件名>.Surface"。
 * 由 key 反查名字是宿主 API 提供的能力, 所以客户端不用自己记名字。 */
static inline uint64_t winmsg_my_surface_key(const mdpsr_host* H) {
    if (!H) return 0;
    const uint64_t self = winmsg__self(H);
    if (!self) return 0;
    mdpsr_plugin_info pi;
    memset(&pi, 0, sizeof(pi));
    pi.struct_size = sizeof(pi);
    if (H->plugin_info(H->self, self, &pi) != MDPSR_OK || !pi.name[0]) return 0;
    char buf[MDPSR_NAME_MAX + 16];
    size_t n = 0;
    for (const char* p = pi.name; *p && n < sizeof(buf) - 12; ++p) buf[n++] = *p;
    for (const char* p = WINMSG_SURFACE_SUFFIX; *p && n < sizeof(buf) - 1; ++p) buf[n++] = *p;
    buf[n] = '\0';
    return mdpsr_hash64(buf);
}

/* --------------------------------------------------------------------------
 *  登记窗口。返回 MDPSR_OK 表示"请求已登记", 窗口是 broker 稍后建出来的,
 *  所以要轮询 winmsg_state() 等它变 WINMSG_S_READY。
 *
 *  ★ 幂等: 同一个名字重复调用是安全的
 *      · 已经登记好 (READY + 几何一样) -> 只刷新代际, 不动窗口
 *      · 几何变了 -> 填新的 RECT + win_seq++, broker 下一帧挪
 *      · 之前 FAILED -> 重新登记, 给它一次重试的机会
 *      · 名字被别的插件占着 -> ALREADY_EXISTS (绝不抢别人的格子)
 *      · 窗口被关了 (state 0) -> 上一次登记已经被回收, 这里会重新登记一个新的
 * ------------------------------------------------------------------------*/
static inline int winmsg_open(const mdpsr_host* H, uint64_t name,
                             int x, int y, int w, int h, uint64_t* out_surface_key) {
    if (!H || !name || w <= 0 || h <= 0) return MDPSR_ERR_BAD_CONFIG;

    const uint64_t skey = winmsg_my_surface_key(H);
    if (!skey) return MDPSR_ERR_NOT_READY;
    if (mdpsr_gen_of(H, skey) == 0) return MDPSR_ERR_NOT_FOUND;   /* 忘了在清单里声明 State */
    if (out_surface_key) *out_surface_key = skey;

    const uint64_t self = winmsg__self(H);
    const uint32_t gen  = winmsg__self_gen(H, self);
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return MDPSR_ERR_NOT_FOUND;

    winmsg_slot* s = winmsg__find(sc, name);
    if (s && s->owner != self) {
        winmsg__screen_unlock(H);          /* ★ 别人的窗口: 硬失败, 不抢格子 */
        return MDPSR_ERR_ALREADY_EXISTS;
    }
    if (s) {
        /* 自己已经登记过这个名字: 刷新代际 (换代了 broker 才认得出我们),
         * 几何/状态按需要补齐 —— 全程不 memset, 绝不抹掉一扇活着的窗。 */
        s->owner_gen   = gen;
        s->surface_key = skey;
        if (s->state == WINMSG_S_FAILED) {
            s->win.x = x; s->win.y = y; s->win.w = w; s->win.h = h;
            ++s->win_seq;
            s->state = WINMSG_S_PENDING;
            s->flags |= WINMSG_F_WANT_CREATE;
        } else if (s->win.x != x || s->win.y != y || s->win.w != w || s->win.h != h) {
            s->win.x = x; s->win.y = y; s->win.w = w; s->win.h = h;
            ++s->win_seq;
        }
        winmsg__screen_unlock(H);
        return MDPSR_OK;
    }

    s = winmsg__find_free(sc);
    if (!s) {
        winmsg__screen_unlock(H);
        return MDPSR_ERR_NO_SPACE;         /* 目录满了 (WINMSG_MAX_WINDOWS) */
    }
    memset(s, 0, sizeof(*s));
    s->name = name;
    s->owner = self;
    s->owner_gen = gen;                    /* ★ 让 broker 那一条代际检查真的生效 */
    s->surface_key = skey;
    s->win.x = x; s->win.y = y; s->win.w = w; s->win.h = h;
    s->win_seq = 1;                        /* 第 1 版窗口 RECT */
    s->flags = WINMSG_F_ALIVE | WINMSG_F_WANT_CREATE;
    s->state = WINMSG_S_PENDING;           /* 握手: 登记完就是"待建" */
    winmsg__screen_unlock(H);
    return MDPSR_OK;
}

/* 声明"这个窗口的事件发到哪个 handle, 要哪些" (events=0 或 handle=0 表示不要消息) */
static inline int winmsg_subscribe(const mdpsr_host* H, uint64_t name,
                                  uint64_t handle, uint32_t events) {
    if (!H) return MDPSR_ERR_NULLPTR;
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return MDPSR_ERR_NOT_FOUND;
    winmsg_slot* s = winmsg__find(sc, name);
    int rc = MDPSR_ERR_NOT_FOUND;
    if (s) { s->handle = handle; s->events = events; rc = MDPSR_OK; }
    winmsg__screen_unlock(H);
    return rc;
}

/* ★ 整窗可拖 (代价: 那扇窗的真实鼠标按下/抬起会被系统当标题栏拖拽吃掉)。
 *   默认不开 —— 默认是普通客户区, 点击/抬起/滚轮都会发到客户端。 */
static inline int winmsg_set_draggable(const mdpsr_host* H, uint64_t name, int on) {
    if (!H) return MDPSR_ERR_NULLPTR;
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return MDPSR_ERR_NOT_FOUND;
    winmsg_slot* s = winmsg__find(sc, name);
    int rc = MDPSR_ERR_NOT_FOUND;
    if (s) {
        if (on) s->flags |= WINMSG_F_DRAGGABLE;
        else    s->flags &= ~WINMSG_F_DRAGGABLE;
        rc = MDPSR_OK;
    }
    winmsg__screen_unlock(H);
    return rc;
}

/* ★ 一步到位: 要窗口 + 声明事件 + (可选)整窗可拖。
 *
 *  为什么要有这个: 格子是**跟着窗口一起回收**的 —— 窗口被关掉/被回收之后,
 *  目录里那一格被清零, 于是 handle/events/draggable 全没了。客户端发现窗口没了
 *  重新要一个时, 如果只调 winmsg_open, 新窗口就是"没有任何订阅"的: 事件会静静
 *  地消失 (handle==0 -> broker 直接不发)。这个坑自测抓到过一次 (点击关窗自愈之后
 *  WM_CLOSE 就收不到了)。
 *
 *  所以: 登记窗口一律用这个函数, 自愈路径和首次路径走同一段代码。 */
static inline int winmsg_open_ex(const mdpsr_host* H, uint64_t name,
                                int x, int y, int w, int h,
                                uint64_t event_handle, uint32_t events, int draggable,
                                uint64_t* out_surface_key) {
    const int rc = winmsg_open(H, name, x, y, w, h, out_surface_key);
    if (rc != MDPSR_OK) return rc;
    if (event_handle && events) winmsg_subscribe(H, name, event_handle, events);
    if (draggable) winmsg_set_draggable(H, name, 1);
    return MDPSR_OK;
}

/* 轮询窗口状态: 0 = 没有这个窗口了 (关掉了/被回收了), 或 WINMSG_S_* */
static inline uint32_t winmsg_state(const mdpsr_host* H, uint64_t name) {
    if (!H) return 0;
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return 0;
    winmsg_slot* s = winmsg__find(sc, name);
    const uint32_t st = s ? s->state : 0;
    winmsg__screen_unlock(H);
    return st;
}

/* ★ 这扇窗现在真的在屏幕上吗 (broker 从系统读回来的 IsWindowVisible)。
 *   注意它和 state 不是一回事: state==READY 只说明"目录上登记好了",
 *   visible 才是"用户看得见"。自测就是靠它抓到"重建了但没显示"那个 bug 的。 */
static inline int winmsg_is_visible(const mdpsr_host* H, uint64_t name) {
    if (!H) return 0;
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return 0;
    winmsg_slot* s = winmsg__find(sc, name);
    const uint32_t v = s ? s->visible : 0;
    winmsg__screen_unlock(H);
    return (int)v;
}

/* 请求关窗 (broker 会在它自己的线程上销毁) */
static inline int winmsg_close(const mdpsr_host* H, uint64_t name) {
    if (!H) return MDPSR_ERR_NULLPTR;
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return MDPSR_ERR_NOT_FOUND;
    winmsg_slot* s = winmsg__find(sc, name);
    int rc = MDPSR_ERR_NOT_FOUND;
    if (s) { s->flags |= WINMSG_F_WANT_CLOSE; rc = MDPSR_OK; }
    winmsg__screen_unlock(H);
    return rc;
}

/* --------------------------------------------------------------------------
 *  参数 1: 窗口的绝对位置与尺寸 (RECT 风格, 屏幕坐标)
 *
 *  填 slot.win 然后 win_seq++, broker 下一帧 SetWindowPos, 并把**实际**矩形写回
 *  slot.cur (从系统读回来的, 所以它证明"真的动了")。
 *  窗口没建出来 (state != READY) 时改是没用的 —— 那时用 winmsg_open 的 x/y/w/h。
 * ------------------------------------------------------------------------*/
static inline int winmsg_get_rect(const mdpsr_host* H, uint64_t name, winmsg_rect* out) {
    if (!H || !out) return MDPSR_ERR_NULLPTR;
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return MDPSR_ERR_NOT_FOUND;
    winmsg_slot* s = winmsg__find(sc, name);
    int rc = MDPSR_ERR_NOT_FOUND;
    if (s) { *out = s->cur; rc = MDPSR_OK; }
    winmsg__screen_unlock(H);
    return rc;
}

/* 想动的维填 WINMSG_KEEP, 其余填目标值 */
static inline int winmsg_set_rect(const mdpsr_host* H, uint64_t name,
                                 int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!H) return MDPSR_ERR_NULLPTR;
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return MDPSR_ERR_NOT_FOUND;
    winmsg_slot* s = winmsg__find(sc, name);
    int rc = MDPSR_ERR_NOT_FOUND;
    if (s) {
        if (x != WINMSG_KEEP) s->win.x = x;
        if (y != WINMSG_KEEP) s->win.y = y;
        if (w != WINMSG_KEEP) s->win.w = w;
        if (h != WINMSG_KEEP) s->win.h = h;
        s->win_seq++;
        rc = MDPSR_OK;
    }
    winmsg__screen_unlock(H);
    return rc;
}

static inline int winmsg_move(const mdpsr_host* H, uint64_t name, int32_t x, int32_t y) {
    return winmsg_set_rect(H, name, x, y, WINMSG_KEEP, WINMSG_KEEP);
}
static inline int winmsg_resize(const mdpsr_host* H, uint64_t name, int32_t w, int32_t h) {
    return winmsg_set_rect(H, name, WINMSG_KEEP, WINMSG_KEEP, w, h);
}

/* --------------------------------------------------------------------------
 *  参数 2: 缩放 + 偏置 (都在画面这张 State 里, 改完 content_seq++ 触发重绘)
 *
 *  winmsg_set_offset : 原图**左上角**在客户区里的位置 (偏置)
 *  winmsg_set_scale  : 缩放倍数 (0 = 1.0, 负 = 镜像); 锚点是原图左上角
 *
 *  管线顺序固定是"先缩放, 再偏置, 再写画布, 窗口外交给系统裁":
 *      dst = img_pos + src * scale
 *  写一幅新画面时用 winmsg_lock_ex / winmsg_unlock 一次改完, 只重绘一次。
 * ------------------------------------------------------------------------*/
static inline int winmsg_set_offset(const mdpsr_host* H, uint64_t surface_key,
                                   int32_t x, int32_t y) {
    winmsg_surface* s = NULL;
    const int rc = winmsg_lock_ex(H, surface_key, &s);
    if (rc != MDPSR_OK) return rc;
    s->img_pos.x = x;
    s->img_pos.y = y;
    winmsg_unlock(H, surface_key, s, 1);
    return MDPSR_OK;
}
/* 老名字, 等价于 winmsg_set_offset (POINT 参数) */
static inline int winmsg_place_image(const mdpsr_host* H, uint64_t surface_key,
                                    int32_t x, int32_t y) {
    return winmsg_set_offset(H, surface_key, x, y);
}
static inline int winmsg_set_scale(const mdpsr_host* H, uint64_t surface_key,
                                  float sx, float sy) {
    winmsg_surface* s = NULL;
    const int rc = winmsg_lock_ex(H, surface_key, &s);
    if (rc != MDPSR_OK) return rc;
    s->scale_x = sx;
    s->scale_y = sy;
    winmsg_unlock(H, surface_key, s, 1);
    return MDPSR_OK;
}

/* 读 broker 实际画到哪一块 (自测/诊断用; 证明参数真的生效了) */
static inline int winmsg_last_dst(const mdpsr_host* H, uint64_t surface_key,
                                 winmsg_rect* out, winmsg_point* offset, float* scale) {
    if (!H || !surface_key) return MDPSR_ERR_NULLPTR;
    winmsg_surface* s = NULL;
    const int rc = winmsg_lock_ex(H, surface_key, &s);
    if (rc != MDPSR_OK) return rc;
    if (out) { out->x = s->last_dst_x; out->y = s->last_dst_y;
               out->w = s->last_dst_w; out->h = s->last_dst_h; }
    if (offset) { offset->x = s->last_img_x; offset->y = s->last_img_y; }
    if (scale) { *scale = s->last_scale_x; }
    mdpsr_release(H, surface_key);
    return MDPSR_OK;
}

/* --------------------------------------------------------------------------
 *  高频输入快照 (鼠标位置/按键状态) —— 不用消息, 主动来轮询
 * ------------------------------------------------------------------------*/
typedef struct winmsg_input {
    int32_t  mx, my;
    uint32_t buttons;
    uint32_t input_seq;
    uint32_t state;
    uint32_t visible;
    uint32_t events_sent;
    uint32_t events_dropped;
} winmsg_input;

static inline int winmsg_read_input(const mdpsr_host* H, uint64_t name, winmsg_input* out) {
    if (!H || !out) return MDPSR_ERR_NULLPTR;
    winmsg_screen* sc = winmsg__screen_lock(H);
    if (!sc) return MDPSR_ERR_NOT_FOUND;
    winmsg_slot* s = winmsg__find(sc, name);
    int rc = MDPSR_ERR_NOT_FOUND;
    if (s) {
        out->mx = s->mx; out->my = s->my;
        out->buttons = s->buttons;
        out->input_seq = s->input_seq;
        out->state = s->state;
        out->visible = s->visible;
        out->events_sent = s->events_sent;
        out->events_dropped = s->events_dropped;
        rc = MDPSR_OK;
    }
    winmsg__screen_unlock(H);
    return rc;
}

/* --------------------------------------------------------------------------
 *  演示用的画法 (自测/示例共用): 透明底 + 一个纯色圆
 * ------------------------------------------------------------------------*/
/* 把整张图刷成一个纯色 (BGRA) —— 演示/自测最省事的画法 */
static inline void winmsg_fill_solid(winmsg_surface* s, uint32_t b, uint32_t g, uint32_t r) {
    if (!s || !s->pixels) return;
    const uint32_t px = (b & 0xFFu) | ((g & 0xFFu) << 8) | ((r & 0xFFu) << 16) | (0xFFu << 24);
    uint32_t* p = (uint32_t*)s->pixels;
    const size_t n = (size_t)s->img_w * (size_t)s->img_h;
    for (size_t i = 0; i < n; ++i) p[i] = px;
}
/* 透明底 + 居中的纯色圆: 圆内 alpha=255 (不透明), 圆外 alpha=0 (完全透明)。
 * 半径 = min(宽,高) * 0.42 —— 所以**图像四角一定在圆外**, 自测拿它验"没画的地方
 * 真的是透明"。argb 按 0xAARRGGBB 给 (和内存里的 uint32 读法一致; alpha 会被强制 255)。 */
static inline void winmsg_draw_circle(winmsg_surface* s, uint32_t argb) {
    if (!s || !s->pixels) return;
    uint32_t* p = (uint32_t*)s->pixels;
    const float cx = s->img_w * 0.5f, cy = s->img_h * 0.5f;
    const float rad = (s->img_w < s->img_h ? s->img_w : s->img_h) * 0.42f;
    const uint32_t solid = 0xFF000000u | (argb & 0x00FFFFFFu);
    for (uint32_t y = 0; y < s->img_h; ++y) {
        for (uint32_t x = 0; x < s->img_w; ++x) {
            const float dx = (float)x - cx, dy = (float)y - cy;
            p[(size_t)y * s->img_w + x] = (dx * dx + dy * dy <= rad * rad) ? solid : 0u;
        }
    }
}

#endif /* WINMSG_CLIENT_H */
