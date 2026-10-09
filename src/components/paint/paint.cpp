/* ============================================================================
 *  paint.cpp —— GUI 演示客户端
 *
 *  ★ 这个文件里没有一行 Windows 代码 —— 没有 windows.h / HWND / HDC / WndProc。
 *    它只知道三件事:
 *      · 一个窗口名 (uint64)
 *      · 自己的一张画面 State (图像指针 + 缩放 + 偏置 + content_seq)
 *      · 自己的两个 handle (刷新用 / 收窗口事件用)
 *
 *  它演示的东西:
 *      State 画面托管   paint.Surface 是自己的资源, 图像内存从自己的池里出
 *      队列限速当动画钟   Queue_paint 的 pace_ms = 8, 一拍刷一版
 *      高频改重绘        每一拍都改纯色 + 挪偏置 -> content_seq++
 *      消息分流          窗口事件走 paint.Events, 业务刷新走 paint.Handle
 *                        (两个 handle 绑同一条队列 -> 天然串行, 客户端一行锁都不用写)
 *      高频输入走状态    鼠标位置/按键状态从 winmsg_read_input 轮询, 不产生消息
 *      窗口被收回能自愈  窗口没了 (state == 0) 就重新 winmsg_open 要一个 —— 热插拔
 *                        之后 GUI 自己回来靠的就是这条
 *      卸载安全          画面在池里, broker 贴图时 acquire 它 -> 卸载会被推迟而不是悬垂
 * ==========================================================================*/
#include "mdpsr/abi.h"
#include "demo_protocol.h"
#include "winmsg_client.h"

#include <cstring>
#include <string>

#define PAINT_WIN_X   160
#define PAINT_WIN_Y   160
#define PAINT_WIN_W   220
#define PAINT_WIN_H   160

#define PAINT_EVENTS_MASK (WINMSG_EV_BIT(WINMSG_EV_READY) |        \
                           WINMSG_EV_BIT(WINMSG_EV_CLOSE) |        \
                           WINMSG_EV_BIT(WINMSG_EV_DESTROYED) |    \
                           WINMSG_EV_BIT(WINMSG_EV_KEY_DOWN) |     \
                           WINMSG_EV_BIT(WINMSG_EV_KEY_UP) |       \
                           WINMSG_EV_BIT(WINMSG_EV_CHAR) |         \
                           WINMSG_EV_BIT(WINMSG_EV_MOUSE_DOWN) |   \
                           WINMSG_EV_BIT(WINMSG_EV_WHEEL) |        \
                           WINMSG_EV_BIT(WINMSG_EV_FOCUS_IN) |     \
                           WINMSG_EV_BIT(WINMSG_EV_FOCUS_OUT))

/* 要窗口 + 订阅事件 —— 首次点火和"窗口被回收之后自己又要一个"走同一段代码。
 * ★ 必须一起做: 窗口被拆时目录格子会被清零, 订阅也跟着没了, 只 winmsg_open
 *   的话新窗口收不到任何事件 (自测抓到过)。见 winmsg_open_ex。 */
static int paint_claim(const mdpsr_host* H, uint64_t* out_skey) {
    const uint64_t wname = mdpsr_hash64(DEMO_PAINT_WINDOW);
    const uint64_t evs   = mdpsr_hash64(DEMO_PAINT_EVENTS);
    return winmsg_open_ex(H, wname, PAINT_WIN_X, PAINT_WIN_Y, PAINT_WIN_W, PAINT_WIN_H,
                          evs, PAINT_EVENTS_MASK, 0, out_skey);
}

static void plog(const mdpsr_host* H, int lv, const std::string& s) {
    if (H && H->log) H->log(H->self, lv, s.c_str());
}

/* --------------------------------------------------------------------------
 *  State: paint.Surface —— 载荷 = winmsg_surface + 像素块 + paint_stat
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT void* mdpsr_state_paint_Surface(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = winmsg_surface_make_ex(ctx, DEMO_PAINT_IMG_W, DEMO_PAINT_IMG_H,
                                       (uint32_t)sizeof(paint_stat));
    if (!mem) return nullptr;
    winmsg_surface* s = (winmsg_surface*)mem;
    /* 一上来先画个圆, 让窗口第一次贴图就有东西 */
    winmsg_draw_circle(s, 0x00906030u);
    plog(ctx->host, 0, "[paint] State 'paint.Surface' 已挂载 (" +
                           std::to_string(DEMO_PAINT_IMG_W) + "x" +
                           std::to_string(DEMO_PAINT_IMG_H) + " BGRA)");
    return mem;
}

/* 扩展区 (统计计数) —— 读写都在 surface 的锁里面 */
static paint_stat* stats_of(winmsg_surface* s) {
    return (paint_stat*)winmsg_surface_extra(s);
}

/* --------------------------------------------------------------------------
 *  队列工厂: Queue_paint —— pace_ms 就是动画时钟 (8ms ≈ 125 帧/秒)
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_queue_Queue_paint(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out) {
    if (!ctx || !out) return MDPSR_ERR_NULLPTR;
    out->struct_size = sizeof(mdpsr_queue_desc);
    out->capacity    = 8192;
    out->pace_ms     = 8;
    out->flags       = 0;
    return MDPSR_OK;
}

/* --------------------------------------------------------------------------
 *  Handle: paint.Events —— 只收窗口事件 (broker emit 过来的 cmd=16)
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_handle_paint_Events(const mdpsr_msg* msg, const uint8_t* body,
                                           uint32_t len, const mdpsr_ctx* ctx) {
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    if (msg->cmd == MDPSR_CMD_INIT) return MDPSR_OK;          /* 点火 */
    if (msg->cmd == MDPSR_CMD_FAIL) return MDPSR_OK;          /* 死信回执, 礼貌收下 */
    if (msg->cmd != WINMSG_CMD_EVENT) return MDPSR_ERR_UNKNOWN_CMD;

    /* 短消息防御: 长度不够就当没收过 */
    winmsg_event ev;
    if (!mdpsr_read(body, len, &ev, sizeof(ev))) {
        plog(H, 1, "[paint] 收到的事件载荷太短, 丢掉");
        return MDPSR_ERR_BAD_MESSAGE;
    }
    if (ev.window != mdpsr_hash64(DEMO_PAINT_WINDOW)) return MDPSR_OK;   /* 不是我的窗口 */

    /* 方向键 -> 挪窗口。这条路径把三样东西串起来了:
     *   Win32 按键 -> broker 翻译成事件 -> emit -> 这里填 win.x/y 并 win_seq++
     *   -> broker 下一帧 SetWindowPos -> 窗口真的动了 */
    if (ev.kind == WINMSG_EV_KEY_DOWN) {
        winmsg_rect rc;
        if (winmsg_get_rect(H, mdpsr_hash64(DEMO_PAINT_WINDOW), &rc) == MDPSR_OK) {
            int dx = 0, dy = 0;
            switch (ev.code) {
            case 0x25: dx = -16; break;             /* VK_LEFT */
            case 0x27: dx =  16; break;             /* VK_RIGHT */
            case 0x26: dy = -16; break;             /* VK_UP */
            case 0x28: dy =  16; break;             /* VK_DOWN */
            default: break;
            }
            if (dx || dy) {
                /* ★参数 1: 窗口的绝对 RECT (x/y/w/h) */
                winmsg_move(H, mdpsr_hash64(DEMO_PAINT_WINDOW), rc.x + dx, rc.y + dy);
                plog(H, 0, "[paint] 方向键 -> 请求把窗口 RECT 挪到 (" +
                               std::to_string(rc.x + dx) + "," + std::to_string(rc.y + dy) + ")");
            }
        }
    }

    const uint64_t skey = winmsg_my_surface_key(H);
    winmsg_surface* s = nullptr;
    if (winmsg_lock_ex(H, skey, &s) != MDPSR_OK) return MDPSR_OK;   /* 忙/没了都不算错 */
    paint_stat* st = stats_of(s);
    uint32_t running_now = st->running;
    ++st->events_rx;
    switch (ev.kind) {
    case WINMSG_EV_READY:      ++st->ev_ready;  break;
    case WINMSG_EV_CLOSE:      ++st->ev_close;  break;
    case WINMSG_EV_DESTROYED:  ++st->ev_destroyed; break;
    case WINMSG_EV_KEY_DOWN:
        ++st->ev_key;
        if (ev.code == 0x20) {                 /* 空格 = 暂停/继续刷新 */
            st->running = st->running ? 0u : 1u;
            running_now = st->running;
        }
        if (ev.code == 0x25 || ev.code == 0x26 || ev.code == 0x27 || ev.code == 0x28) {
            ++st->moves;
        }
        break;
    case WINMSG_EV_KEY_UP:     ++st->ev_keyup;  break;
    case WINMSG_EV_CHAR:       ++st->ev_char;   break;
    case WINMSG_EV_MOUSE_DOWN: ++st->ev_mouse;  break;
    case WINMSG_EV_WHEEL:      ++st->ev_wheel;  break;
    case WINMSG_EV_FOCUS_IN:
    case WINMSG_EV_FOCUS_OUT:  ++st->ev_focus;  break;
    default: break;
    }
    winmsg_unlock(H, skey, s, 0);                             /* 只记数, 不触发重绘 */

    if (ev.kind == WINMSG_EV_CLOSE) plog(H, 0, "[paint] 用户要关窗");
    if (ev.kind == WINMSG_EV_KEY_DOWN && ev.code == 0x20) {
        plog(H, 0, std::string("[paint] 空格 -> ") + (running_now ? "继续刷新" : "暂停刷新"));
    }
    return MDPSR_OK;
}

/* --------------------------------------------------------------------------
 *  Handle: paint.Handle —— 自己的刷新节拍
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_handle_paint_Handle(const mdpsr_msg* msg, const uint8_t* body,
                                           uint32_t len, const mdpsr_ctx* ctx) {
    (void)body; (void)len;
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    if (msg->cmd == MDPSR_CMD_FAIL) return MDPSR_OK;

    const uint64_t wname = mdpsr_hash64(DEMO_PAINT_WINDOW);

    /* ---- cmd=0: 申请窗口 + 订阅事件 + 起自转 ---- */
    if (msg->cmd == MDPSR_CMD_INIT) {
        uint64_t skey = 0;
        const int rc = paint_claim(H, &skey);
        if (rc != MDPSR_OK) {
            plog(H, 2, "[paint] 申请窗口失败 rc=" + std::to_string(rc) +
                           " (忘了声明 State 'paint.Surface' 或 winmsg 没装?)");
            return rc;
        }
        {
            winmsg_surface* s = nullptr;
            if (winmsg_lock_ex(H, skey, &s) == MDPSR_OK) {
                stats_of(s)->running = 1;
                winmsg_unlock(H, skey, s, 1);      /* 第一版画面: 让 broker 贴一次 */
            }
        }
        plog(H, 0, "[paint] cmd=0: 已申请窗口 '" DEMO_PAINT_WINDOW "' (rc=" +
                       std::to_string(rc) + "), 等待 broker 建出来");

        uint32_t zero = 0;
        return mdpsr_emit(H, mdpsr_hash64(DEMO_PAINT_TICK), PAINT_CMD_TICK, &zero, sizeof(zero));
    }

    if (msg->cmd != PAINT_CMD_TICK) return MDPSR_ERR_UNKNOWN_CMD;

    /* ---- 每一拍 ---- */
    const uint64_t skey  = winmsg_my_surface_key(H);
    if (!skey) return MDPSR_OK;

    /* 窗口还在吗? 没了就**重新要一个** (名字就是句柄, 不用改名)。
     * 这正是"窗口被关掉 / 客户端被热插拔之后 GUI 自己回来"的那条路。 */
    const uint32_t st_state = winmsg_state(H, wname);
    if (st_state == 0) {
        winmsg_surface* s = nullptr;
        if (winmsg_lock_ex(H, skey, &s) == MDPSR_OK) {
            stats_of(s)->running = 0;
            winmsg_unlock(H, skey, s, 0);
        }
        uint64_t dummy = 0;
        if (paint_claim(H, &dummy) == MDPSR_OK) {
            if (winmsg_lock_ex(H, skey, &s) == MDPSR_OK) {
                stats_of(s)->running = 1;
                ++stats_of(s)->reopens;
                winmsg_unlock(H, skey, s, 1);
            }
            plog(H, 0, "[paint] 窗口已经不在了 -> 重新申请了一个 (订阅一起重新声明)");
        }
        uint32_t zero = 0;
        return mdpsr_emit(H, mdpsr_hash64(DEMO_PAINT_TICK), PAINT_CMD_TICK, &zero, sizeof(zero));
    }
    if (st_state == WINMSG_S_FAILED) {
        plog(H, 2, "[paint] broker 报告建窗失败");
        return MDPSR_OK;
    }

    /* 高频输入: 鼠标在哪 / 哪个键按着 —— 走状态快照, 不产生消息 */
    winmsg_input in;
    std::memset(&in, 0, sizeof(in));
    winmsg_read_input(H, wname, &in);

    winmsg_surface* s = nullptr;
    const int lrc = winmsg_lock_ex(H, skey, &s);
    if (lrc == MDPSR_ERR_BUSY) {          /* 忙 = 下轮再来, 千万别把自转链掐断 */
        uint32_t zero = 0;
        return mdpsr_emit(H, mdpsr_hash64(DEMO_PAINT_TICK), PAINT_CMD_TICK, &zero, sizeof(zero));
    }
    if (lrc != MDPSR_OK) return MDPSR_OK;
    paint_stat* pst = stats_of(s);

    if (pst->running && st_state == WINMSG_S_READY) {
        const uint32_t f = pst->ticks;
        const uint32_t mx = (uint32_t)(in.mx < 0 ? 0 : in.mx);

        /* 1) ★"内存图片改变": 圆是纯色, 颜色跟着帧号 + 鼠标位置变 */
        const uint32_t argb = 0xFF000000u
                            | ((uint32_t)((f * 3u) & 0xFFu) << 0)     /* B */
                            | ((uint32_t)((mx & 0x7Fu) << 1) << 8)    /* G */
                            | ((uint32_t)(in.buttons ? 0xC0u : 0x40u) << 16);  /* R */
        winmsg_draw_circle(s, argb);

        /* 2) ★参数 2 (POINT + 缩放): 先缩放, 再偏置, 再交给系统裁。
         *    原图左上角在窗口客户区里来回飘, 圆外直接是桌面。 */
        const int wx = PAINT_WIN_W, wy = PAINT_WIN_H;
        const int spanx = wx - (int)DEMO_PAINT_IMG_W;
        const int spany = wy - (int)DEMO_PAINT_IMG_H;
        const int32_t ix = spanx > 0 ? (int32_t)(f % (uint32_t)spanx) : 0;
        const int32_t iy = spany > 0 ? (int32_t)((f * 2u) % (uint32_t)spany) : 0;
        if (s->img_pos.x != ix || s->img_pos.y != iy) ++pst->img_moves;
        s->img_pos.x = ix;
        s->img_pos.y = iy;

        ++pst->ticks;
        ++pst->did_repaint;
        pst->last_mx = mx;
        pst->last_input_seq = in.input_seq;

        if (pst->ticks % 60 == 1) {
            plog(H, 0, "[paint] 第 " + std::to_string(pst->ticks) + " 帧 (原图左上角 " +
                           std::to_string(ix) + "," + std::to_string(iy) +
                           " / 鼠标 x=" + std::to_string(mx) +
                           " / 事件 " + std::to_string(pst->events_rx) + " 条)");
        }
    }
    winmsg_unlock(H, skey, s, 1);       /* changed=1 -> content_seq++ 发布新一版 */

    /* 下一拍 */
    uint32_t zero = 0;
    return mdpsr_emit(H, mdpsr_hash64(DEMO_PAINT_TICK), PAINT_CMD_TICK, &zero, sizeof(zero));
}

/* --------------------------------------------------------------------------
 *  模块导出
 * ------------------------------------------------------------------------*/
MDPSR_DECL_ABI_VERSION()

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    plog(ctx->host, 0, "[paint] 已挂载 (GUI 客户端; 一行 Windows 代码都没有)");
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(void) {
}
