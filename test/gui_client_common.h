/* ============================================================================
 *  gui_client_common.h —— 三个"纯色圆 GUI 客户端"测试插件的公共实现
 *
 *  三个插件 (test/circ_a, test/circ_b, test/circ_c) 的 .cpp 都只有十几行:
 *  把 GtcProfile 传进来, 导出各自的符号。所有逻辑在这里一份。
 *
 *  它演示/验证的东西:
 *      · 画面 State 从自己的池里出 (winmsg_surface_make_ex), 卸载随池回收
 *      · 每一拍: 画纯色圆 -> 改缩放/偏置 -> winmsg_unlock(changed=1) 发布新一版
 *      · 队列 pace_ms 当动画钟 (不同插件不同节奏)
 *      · 窗口事件走 emit (READY/CLOSE/DESTROYED/按键/鼠标/滚轮/焦点), 高频输入走
 *        winmsg_read_input 轮询
 *      · 窗口没了 (state == 0) 自己重新要一个 —— 热插拔之后 GUI 能回来的关键
 *      · GTC_CMD_PAUSE: 自测用它把画面冻住, 然后自己驱动 surface 做确定性像素校验
 * ==========================================================================*/
#ifndef MDPSR_GUI_CLIENT_COMMON_H
#define MDPSR_GUI_CLIENT_COMMON_H

#include "mdpsr/abi.h"
#include "winmsg_client.h"
#include "gui_test_protocol.h"

#include <cstring>
#include <string>

static void gtc_log(const mdpsr_host* H, int lv, const std::string& s) {
    if (H && H->log) H->log(H->self, lv, s.c_str());
}
static GtcStat* gtc_stat_of(winmsg_surface* s) {
    return (GtcStat*)winmsg_surface_extra(s);
}

/* --------------------------------------------------------------------------
 *  State 工厂: <插件>.Surface
 * ------------------------------------------------------------------------*/
static void* gtc_state_factory(const mdpsr_factory_ctx* ctx, const GtcProfile& p) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = winmsg_surface_make_ex(ctx, p.img_w, p.img_h, (uint32_t)sizeof(GtcStat));
    if (!mem) return nullptr;
    winmsg_surface* s = (winmsg_surface*)mem;
    GtcStat* st = gtc_stat_of(s);
    std::memset(st, 0, sizeof(*st));
    st->magic = GTC_STAT_MAGIC;
    s->scale_x = p.scale_x;
    s->scale_y = p.scale_y;
    s->img_pos.x = p.off_x0;
    s->img_pos.y = p.off_y0;
    winmsg_draw_circle(s, p.base_argb);
    st->color = 0xFF000000u | (p.base_argb & 0x00FFFFFFu);
    st->img_pos_x = (uint32_t)p.off_x0;
    st->img_pos_y = (uint32_t)p.off_y0;
    st->scale_x = s->scale_x;
    st->scale_y = s->scale_y;
    gtc_log(ctx->host, 0, std::string("[") + p.plugin + "] 画面 State 已挂载 (" +
                              std::to_string(p.img_w) + "x" + std::to_string(p.img_h) + " BGRA)");
    return mem;
}

/* --------------------------------------------------------------------------
 *  队列工厂: Queue_<插件> —— pace_ms 就是动画时钟
 * ------------------------------------------------------------------------*/
static int gtc_queue_factory(const mdpsr_factory_ctx* ctx, const GtcProfile& p,
                             mdpsr_queue_desc* out) {
    if (!ctx || !out) return MDPSR_ERR_NULLPTR;
    out->struct_size = sizeof(mdpsr_queue_desc);
    out->capacity    = 8192;
    out->pace_ms     = p.pace_ms;
    out->flags       = 0;
    return MDPSR_OK;
}

/* --------------------------------------------------------------------------
 *  登记窗口: 要窗口 + 订阅事件 + (可选)整窗可拖 —— 一段代码两条路都用
 *  (首次点火 / 窗口被回收之后的自愈)。见 winmsg_open_ex 的说明: 格子会跟着
 *  窗口一起回收, 所以自愈时必须把订阅一起重新声明, 否则事件会静静地消失。
 * ------------------------------------------------------------------------*/
static uint32_t gtc_event_mask() {
    return WINMSG_EV_BIT(WINMSG_EV_READY) |
           WINMSG_EV_BIT(WINMSG_EV_CLOSE) |
           WINMSG_EV_BIT(WINMSG_EV_DESTROYED) |
           WINMSG_EV_BIT(WINMSG_EV_KEY_DOWN) |
           WINMSG_EV_BIT(WINMSG_EV_MOUSE_DOWN) |
           WINMSG_EV_BIT(WINMSG_EV_WHEEL) |
           WINMSG_EV_BIT(WINMSG_EV_CHAR) |
           WINMSG_EV_BIT(WINMSG_EV_FOCUS_IN);
}
static int gtc_register(const mdpsr_host* H, const GtcProfile& p, uint64_t* out_skey) {
    const uint64_t wname = mdpsr_hash64(p.window);
    const std::string ev = std::string(p.plugin) + ".Events";
    return winmsg_open_ex(H, wname, p.win_x, p.win_y, p.win_w, p.win_h,
                          mdpsr_hash64(ev.c_str()), gtc_event_mask(),
                          (int)p.draggable, out_skey);
}

/* --------------------------------------------------------------------------
 *  一拍: 画圆 + 改缩放/偏置 + 发布
 * ------------------------------------------------------------------------*/
static void gtc_step(const mdpsr_host* H, const GtcProfile& p) {
    const uint64_t wname = mdpsr_hash64(p.window);
    const uint64_t skey  = winmsg_my_surface_key(H);
    if (!skey) return;

    /* 窗口没了? 重新要一个 (名字就是句柄, 不用改名; 订阅一起重新声明) */
    const uint32_t st = winmsg_state(H, wname);
    if (st == 0) {
        uint64_t dummy = 0;
        if (gtc_register(H, p, &dummy) == MDPSR_OK) {
            winmsg_surface* s0 = nullptr;
            if (winmsg_lock_ex(H, skey, &s0) == MDPSR_OK) {
                ++gtc_stat_of(s0)->reopens;
                winmsg_unlock(H, skey, s0, 1);
            }
            gtc_log(H, 0, std::string("[") + p.plugin + "] 窗口没了 -> 重新申请了一个 (含订阅)");
        }
        return;
    }
    if (st == WINMSG_S_FAILED) return;

    winmsg_input in;
    std::memset(&in, 0, sizeof(in));
    winmsg_read_input(H, wname, &in);

    winmsg_surface* s = nullptr;
    const int rc = winmsg_lock_ex(H, skey, &s);
    if (rc != MDPSR_OK) return;              /* 忙就下轮再来, 自转链不能断 */
    GtcStat* gs = gtc_stat_of(s);
    gs->seen_visible = in.visible;
    gs->last_mx = (uint32_t)(in.mx < 0 ? 0 : in.mx);
    gs->last_input_seq = in.input_seq;

    if (gs->paused || st != WINMSG_S_READY || s->magic != WINMSG_SURFACE_MAGIC) {
        winmsg_unlock(H, skey, s, 0);
        return;
    }

    const uint32_t f = gs->ticks;
    /* 颜色: 基础色 + 帧号旋转 (每一帧都不一样, 但完全可预测) */
    const uint32_t rgb = ((p.base_argb & 0x00FFFFFFu) + f * 0x00010307u) & 0x00FFFFFFu;
    const uint32_t argb = 0xFF000000u | rgb;
    winmsg_draw_circle(s, argb);

    if (p.mode == GTC_MODE_SCALE) {
        /* 缩放来回摆: 0.6 ~ 1.5 */
        const float t = (float)(f % 64u) / 63.0f;
        const float k = 0.6f + 0.9f * (t < 0.5f ? (t * 2.0f) : (2.0f - t * 2.0f));
        s->scale_x = k;
        s->scale_y = k;
    } else {
        s->scale_x = p.scale_x;
        s->scale_y = p.scale_y;
    }

    /* 偏置: 在 [x0, x1] / [y0, y1] 里来回摆 */
    {
        const int32_t spanx = p.off_x1 - p.off_x0;
        const int32_t spany = p.off_y1 - p.off_y0;
        const uint32_t px = spanx > 0 ? (f % (uint32_t)(spanx * 2)) : 0u;
        const uint32_t py = spany > 0 ? ((f * 2u) % (uint32_t)(spany * 2)) : 0u;
        s->img_pos.x = p.off_x0 + (int32_t)(px <= (uint32_t)spanx ? px : (uint32_t)(spanx * 2) - px);
        s->img_pos.y = p.off_y0 + (int32_t)(py <= (uint32_t)spany ? py : (uint32_t)(spany * 2) - py);
    }

    ++gs->ticks;
    gs->color = argb;
    gs->img_pos_x = (uint32_t)s->img_pos.x;
    gs->img_pos_y = (uint32_t)s->img_pos.y;
    gs->scale_x = s->scale_x;
    gs->scale_y = s->scale_y;
    winmsg_unlock(H, skey, s, 1);            /* changed=1 -> 发布新一版 */
}

/* 把自己再叫醒一次 (自转链) */
static int gtc_next_tick(const mdpsr_host* H, const GtcProfile& p) {
    const std::string tick = std::string(p.plugin) + ".Tick";
    uint32_t zero = 0;
    return mdpsr_emit(H, mdpsr_hash64(tick.c_str()), GTC_CMD_TICK, &zero, sizeof(zero));
}

/* --------------------------------------------------------------------------
 *  Handle: <插件>.Tick —— 点火 / 每一拍 / 自测开关
 * ------------------------------------------------------------------------*/
static int gtc_tick_handle(const GtcProfile& p, const mdpsr_msg* msg,
                           const uint8_t* body, uint32_t len, const mdpsr_ctx* ctx) {
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;
    if (msg->cmd == MDPSR_CMD_FAIL) return MDPSR_OK;

    if (msg->cmd == MDPSR_CMD_INIT) {
        uint64_t skey = 0;
        const int rc = gtc_register(H, p, &skey);
        if (rc != MDPSR_OK) {
            gtc_log(H, 2, std::string("[") + p.plugin + "] 登记窗口失败 rc=" + std::to_string(rc));
            return rc;
        }
        gtc_log(H, 0, std::string("[") + p.plugin + "] cmd=0: 窗口 '" + p.window +
                          "' 已登记 (" + std::to_string(p.win_w) + "x" + std::to_string(p.win_h) +
                          "), 等 broker 建出来");
        return gtc_next_tick(H, p);
    }

    if (msg->cmd == GTC_CMD_PAUSE) {
        uint32_t on = 1;
        mdpsr_read(body, len, &on, sizeof(on));
        const uint64_t skey = winmsg_my_surface_key(H);
        winmsg_surface* s = nullptr;
        if (winmsg_lock_ex(H, skey, &s) == MDPSR_OK) {
            gtc_stat_of(s)->paused = on ? 1u : 0u;
            winmsg_unlock(H, skey, s, 0);
        }
        return MDPSR_OK;
    }

    if (msg->cmd == GTC_CMD_REOPEN) {
        /* 主动关窗: 走客户端自己的 winmsg_close, 下一步 tick 里会重新要一个 */
        winmsg_close(H, mdpsr_hash64(p.window));
        return MDPSR_OK;
    }

    if (msg->cmd != GTC_CMD_TICK) return MDPSR_ERR_UNKNOWN_CMD;

    gtc_step(H, p);
    return gtc_next_tick(H, p);
}

/* --------------------------------------------------------------------------
 *  Handle: <插件>.Events —— 窗口事件 (broker emit 过来的 cmd=16)
 * ------------------------------------------------------------------------*/
static int gtc_events_handle(const GtcProfile& p, const mdpsr_msg* msg,
                             const uint8_t* body, uint32_t len, const mdpsr_ctx* ctx) {
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;
    if (msg->cmd == MDPSR_CMD_INIT) return MDPSR_OK;
    if (msg->cmd == MDPSR_CMD_FAIL) return MDPSR_OK;
    if (msg->cmd != WINMSG_CMD_EVENT) return MDPSR_ERR_UNKNOWN_CMD;

    winmsg_event ev;
    if (!mdpsr_read(body, len, &ev, sizeof(ev))) return MDPSR_ERR_BAD_MESSAGE;
    if (ev.window != mdpsr_hash64(p.window)) return MDPSR_OK;

    /* ★ 只有"这条事件真的来自我的窗口"才动手: 名字已经对过了, 下面按 kind 处理 */
    int want_close = 0;
    int click_close = 0;
    if (ev.kind == WINMSG_EV_MOUSE_DOWN && p.close_on_click) {
        want_close = 1;
        click_close = 1;
    } else if (ev.kind == WINMSG_EV_CLOSE && p.close_on_click) {
        want_close = 1;
    }

    const uint64_t skey = winmsg_my_surface_key(H);
    winmsg_surface* s = nullptr;
    if (winmsg_lock_ex(H, skey, &s) == MDPSR_OK) {
        GtcStat* gs = gtc_stat_of(s);
        ++gs->events_rx;
        switch (ev.kind) {
        case WINMSG_EV_READY:     ++gs->ev_ready;  break;
        case WINMSG_EV_CLOSE:     ++gs->ev_close;  break;
        case WINMSG_EV_DESTROYED: ++gs->ev_destroyed; break;
        case WINMSG_EV_KEY_DOWN:  ++gs->ev_key;    break;
        case WINMSG_EV_MOUSE_DOWN:++gs->ev_mouse;  break;
        case WINMSG_EV_WHEEL:     ++gs->ev_wheel;  break;
        case WINMSG_EV_CHAR:      ++gs->ev_char;   break;
        case WINMSG_EV_FOCUS_IN:
        case WINMSG_EV_FOCUS_OUT: ++gs->ev_focus;  break;
        default: break;
        }
        if (click_close) ++gs->closed_by_click;
        if (want_close && !click_close) ++gs->closed_by_close;
        winmsg_unlock(H, skey, s, 0);
    }

    if (want_close) {
        gtc_log(H, 0, std::string("[") + p.plugin + "] 收到" +
                          (click_close ? "点击" : " CLOSE 事件") + " -> 请求关窗");
        winmsg_close(H, mdpsr_hash64(p.window));
    }
    return MDPSR_OK;
}

#endif /* MDPSR_GUI_CLIENT_COMMON_H */
