/* ============================================================================
 *  gui_selftest.cpp —— GUI 阶段自测 (winmsg broker + 4 个 GUI 客户端)
 *
 *  验什么 (每一条都能打印 [ok]/[FAIL], 失败计数就是返回值):
 *
 *    1. 窗口真的建出来了, 而且**真的可见** (slot.visible 是 broker 从系统读回来的
 *       IsWindowVisible —— 光看目录 READY 是不够的, 上一版就是那样漏掉了
 *       "重建之后窗口看不见"的 bug)
 *    2. 动画: 客户端在换颜色 (画面上真的变了), 缩放/偏置也在动
 *    3. 缩放 + 偏置 + 透明: 自测把客户端暂停, 自己驱动 surface 到确定的
 *       (颜色, 缩放, 偏置), 等 broker 贴完, 然后用 winmsg.Control 的采样钩子
 *       读**画布**像素, 和参考光栅器 (test/gui_test_protocol.h 的 gtc_expect)
 *       逐点比对 —— 覆盖: 圆外透明、没被图盖到的画布透明、缩放正确、偏置正确、
 *       偏置跑到出界 (含负值) 时窗口里剩下的部分正确
 *    4. 参数 1 (RECT): 改位置 -> 系统认的 cur 跟上; 改尺寸 -> 窗口和画布一起变大
 *    5. 参数 1 端到端: 注入方向键 -> broker 翻译成事件 -> 客户端改 RECT -> 窗口真挪
 *    6. 输入: 注入的一批合成输入按类别到达客户端; 高频鼠标走状态快照 (不产生消息)
 *    7. 模拟点击关闭: 客户区中心一次真点击 -> 客户端收到 MOUSE_DOWN -> 它请求关窗
 *       -> broker 拆窗 -> 客户端发现窗口没了自己重新要一个 (自愈)
 *    8. WM_CLOSE 路径: 收到 CLOSE 事件; 只有客户端请求关窗才会真拆
 *    9. 热插拔: 三个测试插件逐个"卸载 / 装回 / 重载 N 轮", 每轮都核对
 *       窗口消失了/又可见了, 条目与队列精确回到基线
 * ==========================================================================*/
#include "gui_selftest.h"

#include "runtime/runtime.h"

#include "demo_protocol.h"
#include "winmsg_protocol.h"
#include "winmsg_client.h"
#include "gui_test_protocol.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace mdpsr;

namespace {

/* --------------------------------------------------------------------------
 *  小工具
 * ------------------------------------------------------------------------*/
struct Ctx {
    Runtime* rt = nullptr;
    int      fails = 0;
    int      checks = 0;
    std::string name;
};

void ok(Ctx& c, const std::string& m) {
    c.rt->log(0, "  [ ok ] " + m);
}
void bad(Ctx& c, const std::string& m) {
    ++c.fails;
    c.rt->log(2, "  [FAIL] " + m);
}
void note(Ctx& c, const std::string& m) {
    c.rt->log(0, "  [note] " + m);
}
void good_note(Ctx& c, const std::string& m) { ok(c, m); }
/* 一次断言: 过就静默 +1, 不过就记 FAIL */
void expect(Ctx& c, bool cond, const std::string& what) {
    ++c.checks;
    if (!cond) bad(c, what);
}
uint64_t now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void nap(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static inline uint64_t k_screen()  { return mdpsr_hash64(WINMSG_SCREEN_NAME); }
static inline uint64_t k_control() { return mdpsr_hash64(WINMSG_CONTROL_NAME); }

/* ---- 目录快照 / 改动 ---- */
bool screen_snap(Ctx& c, winmsg_screen* out) {
    void* p = nullptr;
    uint32_t g = 0;
    if (c.rt->reg().acquire(k_screen(), 0, &p, &g) != MDPSR_OK) return false;
    bool good = false;
    winmsg_screen* sc = (winmsg_screen*)p;
    if (sc && sc->magic == WINMSG_MAGIC) { std::memcpy(out, sc, sizeof(*out)); good = true; }
    c.rt->reg().release(k_screen());
    return good;
}
bool slot_of(Ctx& c, uint64_t wname, winmsg_slot* out) {
    winmsg_screen sc;
    if (!screen_snap(c, &sc)) return false;
    for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
        if ((sc.slots[i].flags & WINMSG_F_ALIVE) && sc.slots[i].name == wname) {
            if (out) *out = sc.slots[i];
            return true;
        }
    }
    return false;
}
/* 把窗口 RECT 请求写进目录 (等于客户端调 winmsg_set_rect); 只改想改的维 */
bool set_slot_rect(Ctx& c, uint64_t wname, int32_t x, int32_t y, int32_t w, int32_t h) {
    void* p = nullptr;
    uint32_t g = 0;
    if (c.rt->reg().acquire(k_screen(), 0, &p, &g) != MDPSR_OK) return false;
    bool done = false;
    winmsg_screen* sc = (winmsg_screen*)p;
    if (sc && sc->magic == WINMSG_MAGIC) {
        for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
            winmsg_slot& s = sc->slots[i];
            if (!((s.flags & WINMSG_F_ALIVE) && s.name == wname)) continue;
            if (x != WINMSG_KEEP) s.win.x = x;
            if (y != WINMSG_KEEP) s.win.y = y;
            if (w != WINMSG_KEEP) s.win.w = w;
            if (h != WINMSG_KEEP) s.win.h = h;
            ++s.win_seq;
            done = true;
            break;
        }
    }
    c.rt->reg().release(k_screen());
    return done;
}
bool wait_rect(Ctx& c, uint64_t wname, int32_t x, int32_t y, int32_t w, int32_t h, int ms) {
    for (int i = 0; i < ms / 5; ++i) {
        winmsg_slot s;
        if (slot_of(c, wname, &s) && s.cur.x == x && s.cur.y == y && s.cur.w == w && s.cur.h == h) {
            return true;
        }
        nap(5);
    }
    return false;
}
bool wait_state(Ctx& c, uint64_t wname, uint32_t want, int ms) {
    for (int i = 0; i < ms / 5; ++i) {
        winmsg_slot s;
        if (slot_of(c, wname, &s) && s.state == want) return true;
        nap(5);
    }
    return false;
}
/* 等窗口从目录里彻底消失 (被拆掉 + 格子回收) */
bool wait_gone(Ctx& c, uint64_t wname, int ms) {
    for (int i = 0; i < ms / 5; ++i) {
        if (!slot_of(c, wname, nullptr)) return true;
        nap(5);
    }
    return false;
}
bool window_ready_visible(Ctx& c, uint64_t wname, int ms, winmsg_slot* last) {
    for (int i = 0; i < ms / 5; ++i) {
        winmsg_slot s;
        if (slot_of(c, wname, &s)) {
            if (last) *last = s;
            if (s.state == WINMSG_S_READY && s.visible == 1) return true;
        }
        nap(5);
    }
    return false;
}

/* ---- 注入 (只给自测用的钩子) ---- */
bool inject(Ctx& c, uint64_t wname, uint32_t kind) {
    struct Req { uint64_t name; uint32_t kind; uint32_t pad; } req;
    req.name = wname; req.kind = kind; req.pad = 0;
    return c.rt->emit(k_control(), 0, WINMSG_CMD_INJECT, &req, sizeof(req)) == MDPSR_OK;
}
/* 读画布某个客户区像素 (0xAARRGGBB; 0xFFFFFFFF = 越界/没画布) */
bool probe(Ctx& c, uint64_t wname, int32_t x, int32_t y, uint32_t* out, int ms) {
    uint32_t seq0 = 0, done0 = 0;
    {
        winmsg_slot s;
        if (!slot_of(c, wname, &s)) return false;
        seq0 = s.probe_seq; done0 = s.probe_done;
        (void)done0;
    }
    struct Req { uint64_t name; int32_t x, y, pad; } req;
    req.name = wname; req.x = x; req.y = y; req.pad = 0;
    if (c.rt->emit(k_control(), 0, WINMSG_CMD_PROBE, &req, sizeof(req)) != MDPSR_OK) return false;
    for (int i = 0; i < ms / 5; ++i) {
        winmsg_slot s;
        if (slot_of(c, wname, &s) && s.probe_seq != seq0 && s.probe_done == s.probe_seq) {
            *out = s.probe_color;
            return true;
        }
        nap(5);
    }
    return false;
}

/* ---- 客户端画面: 读参数 / 读统计 / 自测驱动 ---- */
struct SurfSnap {
    uint32_t content_seq = 0, painted_seq = 0;
    float    sx = 1, sy = 1;
    int32_t  ox = 0, oy = 0;
    int32_t  last_dst_w = 0, last_dst_h = 0;
    uint32_t paints = 0;
    GtcStat  st{};
    bool     has_stat = false;
};

/* 借一次画面, 把参数和统计一起拷出来 (一次加锁, 保证两者是同一版) */
bool surf_snap(Ctx& c, uint64_t skey, SurfSnap* out) {
    void* p = nullptr;
    uint32_t g = 0;
    if (c.rt->reg().acquire(skey, 0, &p, &g) != MDPSR_OK) return false;
    bool good = false;
    winmsg_surface* s = (winmsg_surface*)p;
    if (s && s->magic == WINMSG_SURFACE_MAGIC) {
        out->content_seq = s->content_seq;
        out->painted_seq = s->painted_seq;
        out->sx = s->scale_x != 0.0f ? s->scale_x : 1.0f;
        out->sy = s->scale_y != 0.0f ? s->scale_y : 1.0f;
        out->ox = s->img_pos.x;
        out->oy = s->img_pos.y;
        out->last_dst_w = s->last_dst_w;
        out->last_dst_h = s->last_dst_h;
        out->paints = s->paints;
        GtcStat* st = (GtcStat*)winmsg_surface_extra(s);
        if (st && st->magic == GTC_STAT_MAGIC) { out->st = *st; out->has_stat = true; }
        good = true;
    }
    c.rt->reg().release(skey);
    return good;
}
/* 自测把客户端画面改成确定的一版: 纯色圆 + 指定缩放/偏置, 然后 content_seq++ */
bool drive_surface(Ctx& c, const GtcProfile& p, uint64_t skey, uint32_t color,
                   float sx, float sy, int32_t ox, int32_t oy, uint32_t* out_seq) {
    void* q = nullptr;
    uint32_t g = 0;
    if (c.rt->reg().acquire(skey, 0, &q, &g) != MDPSR_OK) return false;
    bool done = false;
    winmsg_surface* s = (winmsg_surface*)q;
    if (s && s->magic == WINMSG_SURFACE_MAGIC && s->pixels) {
        winmsg_draw_circle(s, color);
        s->scale_x = sx;
        s->scale_y = sy;
        s->img_pos.x = ox;
        s->img_pos.y = oy;
        ++s->content_seq;
        if (out_seq) *out_seq = s->content_seq;
        done = true;
    }
    c.rt->reg().release(skey);
    (void)p;
    return done;
}
bool wait_painted(Ctx& c, uint64_t skey, uint32_t seq, int ms) {
    for (int i = 0; i < ms / 5; ++i) {
        void* q = nullptr;
        uint32_t g = 0;
        if (c.rt->reg().acquire(skey, 0, &q, &g) == MDPSR_OK) {
            winmsg_surface* s = (winmsg_surface*)q;
            const bool done = s && s->magic == WINMSG_SURFACE_MAGIC && s->painted_seq >= seq;
            c.rt->reg().release(skey);
            if (done) return true;
        }
        nap(5);
    }
    return false;
}
bool put_pause(Ctx& c, const GtcProfile& p, uint32_t on) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s.Tick", p.plugin);
    return c.rt->emit(mdpsr_hash64(buf), 0, GTC_CMD_PAUSE, &on, sizeof(on)) == MDPSR_OK;
}
/* "<插件名>.Surface" 的键 —— 客户端画面那条 State 的名字是约定死的 */
uint64_t surf_key_of_name(const char* plugin) {
    char buf[MDPSR_NAME_MAX + 16];
    std::snprintf(buf, sizeof(buf), "%s.Surface", plugin);
    return mdpsr_hash64(buf);
}
uint64_t surface_key_of(const GtcProfile& p) { return surf_key_of_name(p.plugin); }

/* 等"暂停"真的生效: 连续两次采样 ticks 不再变化
 * (暂停是投给自己的队列消息, 客户端手上可能还有 1~2 拍在路上) */
bool wait_paused(Ctx& c, uint64_t skey, int ms) {
    uint32_t last = 0;
    bool have = false;
    for (int i = 0; i < ms / 100; ++i) {
        SurfSnap s;
        if (!surf_snap(c, skey, &s)) return false;
        if (have && s.st.ticks == last) return true;
        last = s.st.ticks;
        have = true;
        nap(100);
    }
    return false;
}

/* --------------------------------------------------------------------------
 *  逐点比对: 把"参考光栅器说的"和"画布上真有的"对一次
 *
 *  只在**远离圆边缘** (>3 像素) 且落在画布内的点上比 —— 边缘像素的取整规则
 *  不该由测试来规定。
 * ------------------------------------------------------------------------*/
void check_point(Ctx& c, const GtcProfile& p, uint64_t wname, uint32_t color,
                 float sx, float sy, int32_t ox, int32_t oy,
                 int x, int y, const char* what) {
    winmsg_slot sl;
    if (!slot_of(c, wname, &sl)) { bad(c, std::string(what) + ": 窗口不见了"); return; }
    const int ww = sl.cur.w, wh = sl.cur.h;
    if (x < 0 || y < 0 || x >= ww || y >= wh) return;      /* 窗口外的点没法采样 */
    const GtcExpect e = gtc_expect(&p, color, sx, sy, ox, oy, x, y);
    if (e.in_image && e.margin < 3.0f && e.margin > -3.0f) return;   /* 贴着圆边, 跳过 */
    uint32_t got = 0;
    if (!probe(c, wname, x, y, &got, 1500)) { bad(c, std::string(what) + ": 采样超时"); return; }
    ++c.checks;
    if (got != e.argb) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%s: 画布 (%d,%d)=0x%08X, 期望 0x%08X (原图坐标 %d,%d, 圆内=%d)",
                      what, x, y, (unsigned)got, (unsigned)e.argb, e.isx, e.isy, e.solid);
        bad(c, buf);
    }
}

/* --------------------------------------------------------------------------
 *  一个测试插件的完整检查
 * ------------------------------------------------------------------------*/
void check_client(Ctx& c, const GtcProfile& p, int cycles, bool quick) {
    Runtime& rt = *c.rt;
    const uint64_t wname = mdpsr_hash64(p.window);
    const uint64_t skey  = surface_key_of(p);
    const uint32_t color = 0xFF000000u | (p.base_argb & 0x00FFFFFFu);
    c.name = p.plugin;
    rt.log(0, "--- " + std::string(p.plugin) + " (" + std::to_string(p.img_w) + "x" +
              std::to_string(p.img_h) + " 图 / " + std::to_string(p.win_w) + "x" +
              std::to_string(p.win_h) + " 窗) ---");

    /* 1. 窗口在, 而且**真的可见** */
    winmsg_slot sl;
    const bool rv = window_ready_visible(c, wname, 4000, &sl);
    expect(c, rv, std::string(p.plugin) + ": 窗口没能在 4 秒内 READY 且可见");
    if (!rv) return;
    if (sl.visible != 1) note(c, std::string(p.plugin) + ": slot.visible=" + std::to_string(sl.visible));

    /* 2. 动画: 画面真的在变 (换色; 缩放/偏置按画像的模式) */
    {
        SurfSnap a, b;
        if (!surf_snap(c, skey, &a)) { bad(c, std::string(p.plugin) + ": 借不到画面 State"); return; }
        nap(400);
        if (!surf_snap(c, skey, &b)) { bad(c, std::string(p.plugin) + ": 借不到画面 State"); return; }
        expect(c, b.st.ticks > a.st.ticks, std::string(p.plugin) + ": 客户端没有再发布新画面");
        expect(c, b.st.color != a.st.color, std::string(p.plugin) + ": 颜色没有变化 (动画没跑)");
        if (p.mode == GTC_MODE_SCALE) {
            expect(c, b.sx != a.sx, std::string(p.plugin) + ": 缩放没有变化");
        } else {
            expect(c, b.ox != a.ox || b.oy != a.oy, std::string(p.plugin) + ": 偏置没有变化");
        }
        expect(c, b.paints > a.paints, std::string(p.plugin) + ": broker 没有重绘");
        char t[32];
        std::snprintf(t, sizeof(t), "%06X", (unsigned)(b.st.color & 0x00FFFFFFu));
        char m[256];
        std::snprintf(m, sizeof(m), "%s: 帧 %u -> %u, 颜色 0x%s, broker 贴图 %u 次",
                      p.plugin, (unsigned)a.st.ticks, (unsigned)b.st.ticks, t, (unsigned)b.paints);
        note(c, m);
    }

    /* 3. 确定性像素校验: 暂停客户端, 自测驱动 surface, 逐点比对参考光栅器 */
    put_pause(c, p, 1);
    expect(c, wait_paused(c, skey, 2000),
           std::string(p.plugin) + ": 暂停请求没生效 (画面还在被客户端改)");

    struct Cfg { float sx, sy; int32_t ox, oy; const char* what; };
    const Cfg cfgs[4] = {
        { 1.0f, 1.0f,  10,  10, "缩放1x/偏置(10,10)" },
        { 1.0f, 1.0f, -16, -16, "偏置为负(出界)"     },
        { 2.0f, 2.0f,  20,  16, "放大2倍"            },
        { 1.0f, 1.0f, 1000, 1000, "整块挪出窗口"      },
    };
    for (const Cfg& cfg : cfgs) {
        uint32_t seq = 0;
        if (!drive_surface(c, p, skey, color, cfg.sx, cfg.sy, cfg.ox, cfg.oy, &seq)) {
            bad(c, std::string(p.plugin) + ": 驱动画面失败");
            break;
        }
        if (!wait_painted(c, skey, seq, 3000)) {
            bad(c, std::string(p.plugin) + ": 驱动的那一版没被贴出来 (" + cfg.what + ")");
            continue;
        }
        winmsg_slot cur;
        if (!slot_of(c, wname, &cur)) { bad(c, std::string(p.plugin) + ": 窗口不见了"); break; }
        const int ww = cur.cur.w, wh = cur.cur.h;

        /* 图中心 (缩放后): 一定在圆里 -> 不透明纯色 */
        const int cx = (int)((float)cfg.ox + (float)p.img_w * 0.5f * cfg.sx);
        const int cy = (int)((float)cfg.oy + (float)p.img_h * 0.5f * cfg.sy);
        check_point(c, p, wname, color, cfg.sx, cfg.sy, cfg.ox, cfg.oy, cx, cy, cfg.what);
        /* 原图左上角落点: 在圆外 -> 透明 */
        check_point(c, p, wname, color, cfg.sx, cfg.sy, cfg.ox, cfg.oy, cfg.ox, cfg.oy, cfg.what);
        /* 没被图盖到的画布: 透明 (挑窗口右下角, 那个点肯定在图外) */
        check_point(c, p, wname, color, cfg.sx, cfg.sy, cfg.ox, cfg.oy, ww - 2, wh - 2, cfg.what);
        /* 一个"offset 真的生效了"的判据: 图左边 12 像素处必须是透明 */
        check_point(c, p, wname, color, cfg.sx, cfg.sy, cfg.ox, cfg.oy, cfg.ox - 12, cfg.oy + 4, cfg.what);

        SurfSnap s2;
        if (surf_snap(c, skey, &s2)) {
            if (std::strcmp(cfg.what, "整块挪出窗口") == 0) {
                expect(c, s2.last_dst_w == 0 && s2.last_dst_h == 0,
                       std::string(p.plugin) + ": 整块出界时 broker 记的绘制区应该为 0");
            } else {
                expect(c, s2.last_dst_w > 0 && s2.last_dst_h > 0,
                       std::string(p.plugin) + ": broker 记的绘制区不该是空的");
            }
        }
    }
    /* 画面参数恢复成画像本来的样子; 客户端**先别叫醒** —— 下面量 RECT 的时候
     * 自测自己驱动画面, 客户端一起来就会把它覆盖掉, 那就不是确定性测试了。 */
    drive_surface(c, p, skey, color, p.scale_x, p.scale_y, p.off_x0, p.off_y0, nullptr);

    /* 4. ★参数 1: RECT 移动 + 缩放 (系统认的 cur 必须跟上) */
    {
        winmsg_slot before;
        if (!slot_of(c, wname, &before)) { bad(c, std::string(p.plugin) + ": 窗口不见了"); return; }
        const int32_t nx = before.cur.x + 50, ny = before.cur.y + 30;
        if (!set_slot_rect(c, wname, nx, ny, WINMSG_KEEP, WINMSG_KEEP)) {
            bad(c, std::string(p.plugin) + ": 写窗口 RECT 失败");
        } else {
            const bool moved = wait_rect(c, wname, nx, ny, before.cur.w, before.cur.h, 2000);
            expect(c, moved, std::string(p.plugin) + ": 请求挪窗口后 cur 没跟上 (系统没真的挪)");
            if (moved) note(c, std::string(p.plugin) + ": 窗口挪到 (" + std::to_string(nx) + "," +
                                   std::to_string(ny) + "), 系统确认");
        }
        const int32_t nw = before.cur.w + 40, nh = before.cur.h + 30;
        if (set_slot_rect(c, wname, nx, ny, nw, nh)) {
            const bool sized = wait_rect(c, wname, nx, ny, nw, nh, 2000);
            expect(c, sized, std::string(p.plugin) + ": 请求改窗口尺寸后 cur 没跟上");
            if (sized) {
                /* 画布跟着变大: 新多出来的那块右下角必须是透明 (那里没图) */
                uint32_t seq = 0;
                drive_surface(c, p, skey, color, 1.0f, 1.0f, 0, 0, &seq);
                if (wait_painted(c, skey, seq, 3000)) {
                    const GtcExpect e = gtc_expect(&p, color, 1.0f, 1.0f, 0, 0, nw - 2, nh - 2);
                    if (!e.in_image) {
                        uint32_t got = 0;
                        if (probe(c, wname, nw - 2, nh - 2, &got, 1500)) {
                            expect(c, got == 0u,
                                   std::string(p.plugin) + ": 窗口变大后多出来的区域不是透明的");
                        }
                    }
                }
                note(c, std::string(p.plugin) + ": 窗口缩放到 " + std::to_string(nw) + "x" +
                         std::to_string(nh) + ", 系统确认");
            }
        }
        /* 摆回原位, 后面的检查好算 */
        set_slot_rect(c, wname, p.win_x, p.win_y, p.win_w, p.win_h);
        wait_rect(c, wname, p.win_x, p.win_y, p.win_w, p.win_h, 2000);
    }

    /* 画面参数 / RECT 都量完了, 现在把客户端叫醒 (它会自己继续画动画) */
    put_pause(c, p, 0);

    /* 5. 点击 -> 关窗 -> 自愈 (只有 close_on_click 的画像才关) */
    {
        SurfSnap s0;
        if (!surf_snap(c, skey, &s0)) return;
        winmsg_screen sc0;
        screen_snap(c, &sc0);
        inject(c, wname, WINMSG_INJECT_CLICK);
        bool got_click = false;
        for (int i = 0; i < 800 && !got_click; ++i) {
            SurfSnap s1;
            if (surf_snap(c, skey, &s1) && s1.st.ev_mouse > s0.st.ev_mouse) got_click = true;
            else nap(5);
        }
        expect(c, got_click, std::string(p.plugin) + ": 真实点击没有到达客户端 (MOUSE_DOWN)");
        if (p.close_on_click) {
            /* ★ 别去"抓窗口消失的那一瞬": 客户端一发现窗口没了就立刻重新要一个
             *   (节奏 8~16ms), 抓不到是正常的。看的是"关过 + 又建出来了"。 */
            SurfSnap s2;
            bool recovered = false;
            for (int i = 0; i < 1000 && !recovered; ++i) {
                if (surf_snap(c, skey, &s2) && s2.st.closed_by_click > s0.st.closed_by_click &&
                    s2.st.reopens > s0.st.reopens) {
                    recovered = true;
                } else {
                    nap(5);
                }
            }
            expect(c, recovered, std::string(p.plugin) + ": 点击关闭 + 自愈这条链没走通");
            expect(c, window_ready_visible(c, wname, 4000, nullptr),
                   std::string(p.plugin) + ": 自愈之后窗口不在/不可见");
            winmsg_screen sc1;
            if (screen_snap(c, &sc1)) {
                expect(c, sc1.creates_total > sc0.creates_total,
                       std::string(p.plugin) + ": 点击关窗后没有新建窗口");
            }
            if (recovered) {
                good_note(c, std::string(p.plugin) + ": 点击 -> 客户端请求关窗 -> broker 拆窗 -> "
                                                      "客户端自己又要了一个 (自愈)");
            }
        } else {
            winmsg_slot s1;
            expect(c, slot_of(c, wname, &s1) && s1.state == WINMSG_S_READY,
                   std::string(p.plugin) + ": 不该关窗的画像把窗口关掉了");
        }
    }

    /* 6. WM_CLOSE: 事件必须到; 只有客户端请求关窗才真拆 */
    {
        SurfSnap s0;
        if (!surf_snap(c, skey, &s0)) return;
        inject(c, wname, WINMSG_INJECT_CLOSE);
        bool got_close = false;
        for (int i = 0; i < 600 && !got_close; ++i) {
            SurfSnap s1;
            if (surf_snap(c, skey, &s1) && s1.st.ev_close > s0.st.ev_close) got_close = true;
            else nap(5);
        }
        expect(c, got_close, std::string(p.plugin) + ": WM_CLOSE 没有变成 CLOSE 事件");
        if (p.close_on_click) {
            expect(c, window_ready_visible(c, wname, 4000, nullptr),
                   std::string(p.plugin) + ": CLOSE 之后窗口没能恢复");
            SurfSnap s2;
            if (surf_snap(c, skey, &s2)) {
                expect(c, s2.st.closed_by_close > 0, std::string(p.plugin) + ": 没记到 CLOSE 导致的关系");
            }
        } else {
            winmsg_slot s1;
            expect(c, slot_of(c, wname, &s1) && s1.state == WINMSG_S_READY,
                   std::string(p.plugin) + ": 没请求关窗却被拆了 (broker 不该替客户端决定)");
        }
    }

    /* 7. 热插拔: 卸载 -> 窗口消失; 装回 -> 窗口又可见; 再重载 N 轮 */
    {
        const size_t base_entries = rt.reg().size();
        uint32_t base_queues = 0;
        rt.queue_list(nullptr, 0, &base_queues);
        const uint64_t pkey = mdpsr_plugin_key(p.plugin);
        char manifest[128];
        std::snprintf(manifest, sizeof(manifest), "plugins/%s/plugin.json", p.plugin);

        int rc = rt.plugin_uninstall(pkey, 0);
        expect(c, rc == MDPSR_OK, std::string(p.plugin) + ": 卸载失败 rc=" + std::to_string(rc));
        if (rc == MDPSR_OK) {
            expect(c, wait_gone(c, wname, 3000), std::string(p.plugin) + ": 卸载后窗口还在");
            expect(c, rt.reg().size() == base_entries - 4,
                   std::string(p.plugin) + ": 卸载后条目没回到基线 (" +
                       std::to_string(rt.reg().size()) + " vs " + std::to_string(base_entries) + ")");
        }
        rc = rt.plugin_install(manifest, nullptr, nullptr);
        expect(c, rc == MDPSR_OK, std::string(p.plugin) + ": 重装失败 rc=" + std::to_string(rc));
        if (rc == MDPSR_OK) {
            expect(c, window_ready_visible(c, wname, 4000, nullptr),
                   std::string(p.plugin) + ": 重装后窗口不在/不可见 (★ 这一条就是那个 bug 的回归测试)");
            expect(c, rt.reg().size() == base_entries,
                   std::string(p.plugin) + ": 重装后条目没回到基线");
        }

        const int rounds = quick ? 1 : cycles;
        bool all_ok = true;
        for (int i = 0; i < rounds && all_ok; ++i) {
            const int rr = rt.plugin_reload(pkey, nullptr);
            if (rr != MDPSR_OK) {
                bad(c, std::string(p.plugin) + ": 第 " + std::to_string(i) +
                       " 轮重载失败 rc=" + std::to_string(rr));
                all_ok = false;
                break;
            }
            winmsg_slot s;
            if (!window_ready_visible(c, wname, 4000, &s)) {
                bad(c, std::string(p.plugin) + ": 第 " + std::to_string(i) +
                       " 轮重载后窗口不在/不可见");
                all_ok = false;
            }
        }
        if (all_ok) {
            good_note(c, std::string(p.plugin) + ": 卸载/装回/重载 " + std::to_string(rounds) +
                          " 轮, 每轮窗口都回来了而且是可见的");
        }
        uint32_t end_queues = 0;
        rt.queue_list(nullptr, 0, &end_queues);
        expect(c, end_queues == base_queues,
               std::string(p.plugin) + ": 队列数没回到基线 (" + std::to_string(end_queues) +
                   " vs " + std::to_string(base_queues) + ")");
        expect(c, rt.reg().size() == base_entries, std::string(p.plugin) + ": 条目数没回到基线");
    }
}

} /* namespace */

/* ============================================================================
 *  入口
 * ==========================================================================*/
int mdpsr_gui_selftest(Runtime& rt, int cycles, bool quick) {
    Ctx c;
    c.rt = &rt;
    if (cycles <= 0) cycles = 1;

    rt.log(0, "== GUI 阶段 (winmsg broker + paint + 3 个测试客户端) ==");

    /* ---- 基线 ---- */
    const size_t base_entries = rt.reg().size();
    uint32_t base_queues = 0;
    rt.queue_list(nullptr, 0, &base_queues);
    winmsg_screen sc0;
    if (!screen_snap(c, &sc0)) {
        bad(c, "拿不到 winmsg.Screen (winmsg 没装?)");
        return c.fails;
    }
    note(c, "基线: 注册表 " + std::to_string(base_entries) + " 个条目, " +
            std::to_string(base_queues) + " 条队列");

    /* ---- 1. 四个窗口 (paint + 3 个测试插件) 都要 READY 且可见 ---- */
    const uint64_t w_paint = mdpsr_hash64(DEMO_PAINT_WINDOW);
    struct Target { const char* plugin; uint64_t win; };
    std::vector<Target> targets;
    targets.push_back({ "paint", w_paint });
    for (uint32_t i = 0; i < GTC_PROFILE_COUNT; ++i) {
        targets.push_back({ kGtcProfiles[i].plugin, mdpsr_hash64(kGtcProfiles[i].window) });
    }
    for (const Target& t : targets) {
        winmsg_slot s;
        const bool rv = window_ready_visible(c, t.win, 5000, &s);
        expect(c, rv, std::string(t.plugin) + ": 窗口没能在 5 秒内 READY 且可见");
        if (rv && s.visible != 1) bad(c, std::string(t.plugin) + ": slot.visible != 1");
    }
    {
        winmsg_screen sc;
        if (screen_snap(c, &sc)) {
            if (sc.paints_total > 0) {
                good_note(c, "broker: 建窗 " + std::to_string(sc.creates_total) +
                             " / 贴图 " + std::to_string(sc.paints_total) +
                             " / 事件 " + std::to_string(sc.events_sent_total) +
                             " / 开着的窗 " + std::to_string(sc.windows_open));
            } else {
                bad(c, "broker 一个像素都没贴上");
            }
            expect(c, sc.windows_open >= 4, "同时开着的窗口数应该 >= 4");
        }
    }

    /* ---- 2. paint 客户端: 帧率 / broker 重绘率 / 注入输入 / 高频输入 ---- */
    {
        const uint64_t skey = surf_key_of_name("paint");
        winmsg_screen a, b;
        uint32_t ticks_a = 0, ticks_b = 0, paints_a = 0, paints_b = 0;
        {
            void* q = nullptr; uint32_t g = 0;
            if (rt.reg().acquire(skey, 0, &q, &g) == MDPSR_OK) {
                paint_stat* st = (paint_stat*)winmsg_surface_extra((winmsg_surface*)q);
                if (st) ticks_a = st->ticks;
                rt.reg().release(skey);
            }
            screen_snap(c, &a);
            paints_a = a.paints_total;
        }
        nap(1000);
        {
            void* q = nullptr; uint32_t g = 0;
            if (rt.reg().acquire(skey, 0, &q, &g) == MDPSR_OK) {
                paint_stat* st = (paint_stat*)winmsg_surface_extra((winmsg_surface*)q);
                if (st) ticks_b = st->ticks;
                rt.reg().release(skey);
            }
            screen_snap(c, &b);
            paints_b = b.paints_total;
        }
        expect(c, ticks_b > ticks_a, "paint 客户端没有在刷新画面");
        expect(c, paints_b > paints_a, "broker 这一秒一次都没重绘");
        if (ticks_b > ticks_a && paints_b > paints_a) {
            good_note(c, "paint: 客户端 " + std::to_string(ticks_b - ticks_a) +
                         " 帧/秒, broker 重绘 " + std::to_string(paints_b - paints_a) + " 次/秒 (变了才贴)");
        }

        /* 注入一批合成输入 -> 逐类到达; 高频鼠标走状态快照 */
        paint_stat st_before{};
        {
            void* q = nullptr; uint32_t g = 0;
            if (rt.reg().acquire(skey, 0, &q, &g) == MDPSR_OK) {
                paint_stat* st = (paint_stat*)winmsg_surface_extra((winmsg_surface*)q);
                if (st) st_before = *st;
                rt.reg().release(skey);
            }
        }
        inject(c, w_paint, WINMSG_INJECT_BURST);
        paint_stat st_after{};
        bool hit = false;
        for (int i = 0; i < 800 && !hit; ++i) {
            void* q = nullptr; uint32_t g = 0;
            if (rt.reg().acquire(skey, 0, &q, &g) == MDPSR_OK) {
                paint_stat* st = (paint_stat*)winmsg_surface_extra((winmsg_surface*)q);
                if (st) {
                    st_after = *st;
                    hit = st->ev_key > st_before.ev_key && st->ev_mouse > st_before.ev_mouse &&
                          st->ev_wheel > st_before.ev_wheel && st->ev_char > st_before.ev_char &&
                          st->ev_focus > st_before.ev_focus;
                }
                rt.reg().release(skey);
            }
            if (!hit) nap(5);
        }
        expect(c, hit, "注入的输入没有全部到达 paint 客户端");
        if (hit) {
            good_note(c, "消息分流: 注入输入全部到达 (按键 " + std::to_string(st_after.ev_key) +
                         " / 鼠标 " + std::to_string(st_after.ev_mouse) +
                         " / 滚轮 " + std::to_string(st_after.ev_wheel) +
                         " / 字符 " + std::to_string(st_after.ev_char) +
                         " / 焦点 " + std::to_string(st_after.ev_focus) + ")");
        }
        expect(c, st_after.last_input_seq != st_before.last_input_seq || st_after.last_mx != st_before.last_mx,
               "高频输入快照 (鼠标位置/按键) 没有更新");

        /* 参数 1 端到端: 方向键 -> 事件 -> 客户端改 RECT -> 窗口真挪 */
        winmsg_slot before;
        if (slot_of(c, w_paint, &before)) {
            inject(c, w_paint, WINMSG_INJECT_BURST);   /* 里面带一次 VK_RIGHT */
            bool moved = false;
            for (int i = 0; i < 800 && !moved; ++i) {
                winmsg_slot s;
                if (slot_of(c, w_paint, &s) && s.cur.x != before.cur.x) moved = true;
                else nap(5);
            }
            expect(c, moved, "方向键没能让窗口真的挪动 (参数 1 端到端断了)");
            if (moved) {
                winmsg_slot s;
                slot_of(c, w_paint, &s);
                good_note(c, "参数 1 端到端: 方向键 -> 事件 -> 客户端改 RECT -> 窗口 x " +
                             std::to_string(before.cur.x) + " -> " + std::to_string(s.cur.x));
            }
        }
    }

    /* ---- 3. 三个测试插件: 像素级校验 + RECT + 点击关闭 + 热插拔 ---- */
    for (uint32_t i = 0; i < GTC_PROFILE_COUNT; ++i) {
        check_client(c, kGtcProfiles[i], cycles, quick);
    }

    /* ---- 4. 收尾核对 ---- */
    expect(c, rt.reg().size() == base_entries,
           "GUI 阶段结束后条目没回到基线 (" + std::to_string(rt.reg().size()) + " vs " +
               std::to_string(base_entries) + ")");
    uint32_t end_queues = 0;
    rt.queue_list(nullptr, 0, &end_queues);
    expect(c, end_queues == base_queues,
           "GUI 阶段结束后队列数没回到基线 (" + std::to_string(end_queues) + " vs " +
               std::to_string(base_queues) + ")");
    winmsg_screen end;
    if (screen_snap(c, &end)) {
        expect(c, end.windows_open >= 4, "GUI 阶段结束时应该还有 4 扇窗开着");
        note(c, "broker 累计: 建窗 " + std::to_string(end.creates_total) + " / 销毁 " +
                std::to_string(end.destroys_total) + " / 贴图 " + std::to_string(end.paints_total) +
                " / 事件 " + std::to_string(end.events_sent_total) + " (丢 " +
                std::to_string(end.events_dropped_total) + ")");
    }
    if (rt.t_leaked.load() != 0) {
        bad(c, "GUI 阶段有插件漏还锁: " + std::to_string(rt.t_leaked.load()));
    }
    rt.log(0, std::string("== GUI 阶段结束: ") +
              (c.fails ? "失败 " + std::to_string(c.fails) + " 项" : "全部通过") +
              " (断言 " + std::to_string(c.checks) + " 条) ==");
    return c.fails;
}
