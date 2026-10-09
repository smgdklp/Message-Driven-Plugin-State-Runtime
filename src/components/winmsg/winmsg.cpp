/* ============================================================================
 *  winmsg.cpp —— GUI broker 插件
 *
 *  它是整个框架里唯一碰 Win32 的组件。别的插件只认"窗口名 + 画面 State + 事件消息"。
 *
 *  ┌ 资源 ────────────────────────────────────────────────────────────────┐
 *  │ State  winmsg.Screen   目录: 窗口名 -> 目录项 (客户端读写, 走 acquire) │
 *  │ Object winmsg.Shelf    HWND 表 + GUI 线程 (私有, 只有 GUI 线程碰)     │
 *  │ Handle winmsg.Control  控制入口: cmd=0 启动 GUI 线程                  │
 *  └──────────────────────────────────────────────────────────────────────┘
 *
 *  ★ 渲染管线 (doc/GUI.md 第三节是权威说明):
 *      每窗口一张和客户区一样大的 32bpp 预乘 BGRA 画布 (初始全 0 = 全透明),
 *      客户端给的图先按 scale 缩放、再按 img_pos 偏置, 最近邻采样写进画布,
 *      然后 UpdateLayeredWindow(ULW_ALPHA) 整张交给系统合成。
 *      写出画布的部分不写 (= 透明), 窗口之外由系统裁掉 —— "出界即透明"是
 *      这两条的自然结果, 不需要任何特判。
 *
 *  ★ 主循环在 GUI 线程里, 五步一轮:
 *      泵窗口消息 -> 扫目录 (建/销窗, 挪窗, 注入, 采样) -> 有变化就组合画布 +
 *      UpdateLayeredWindow -> 睡到下一帧 (MsgWaitForMultipleObjects, 空闲 0% CPU)
 *
 *  ★ 锁纪律 (写死在实现里, 别改):
 *    · Shelf 只有 GUI 线程碰  -> 完全不用锁
 *    · Screen / Surface 都走 acquire/release
 *    · Screen 与 Surface 永不嵌套持有: 拿 Screen 只做快照/更新, 放掉之后才碰 Surface
 *    · 事件路径只 emit, 绝不在这里调客户端代码 (详见 event_send_by_name)
 *
 *  ★ 两条通道:
 *    画面 -> 状态 + content_seq      (丢一两帧无所谓, 合并重绘是好事)
 *    事件 -> emit 到客户端的 handle  (每条都有意义, 不能丢)
 *    鼠标移动 -> 只写输入快照        (高频, 客户端自己轮询)
 * ==========================================================================*/
#include "mdpsr/abi.h"
#include "winmsg_protocol.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

/* ==========================================================================
 *  Object: winmsg.Shelf —— HWND 架 + GUI 线程
 * ==========================================================================*/
struct Shelf {
    const mdpsr_host* host = nullptr;
    mdpsr_pool*       pool = nullptr;
    uint64_t          plugin = 0;

    std::thread       th;
    std::atomic<int>  stop{ 0 };
    std::atomic<int>  running{ 0 };
    std::atomic<int>  started{ 0 };

    /* 只有 GUI 线程碰 */
    HWND     hwnds[WINMSG_MAX_WINDOWS] = {};
    uint64_t names[WINMSG_MAX_WINDOWS] = {};
    /* ★ 每扇窗各自的"显示过没有"。用全局一个标志是错的: 那样只有**第一扇**窗
     *   会被 ShowWindow, 之后重建出来的窗永远停在隐藏状态 (自测实测抓到过)。 */
    bool     shown[WINMSG_MAX_WINDOWS] = {};
    /* ★ 命中测试用: 这扇窗客户端点名要整窗可拖吗 (scan 里从目录镜像过来,
     *   wndproc 只读内存, 不用拿锁) */
    bool     drag_ok[WINMSG_MAX_WINDOWS] = {};

    /* 每个窗口一块画布 DIB (和窗口一样大, 逐像素 alpha) + 内存 DC。
     * 画布初始全透明 (alpha=0), 只有被贴上去的图像像素才可见 ——
     * 这就是"窗口默认纯透明"的来源。都是 GUI 线程独占的资源。 */
    struct Gdi {
        HBITMAP dib = nullptr;      /* 画布 DIB, 32bpp 预乘 BGRA, 自上而下 */
        HDC     memdc = nullptr;
        void*   bits = nullptr;
        uint32_t dw = 0, dh = 0;    /* 画布尺寸 = 窗口客户区尺寸 */
    } gdi[WINMSG_MAX_WINDOWS];

    bool     class_ready = false;
    HMODULE  self_mod = nullptr;

    uint32_t frame_ms = 16;
    uint64_t last_title_ms = 0;
    /* 统计 (GUI 线程写, 每帧发布到 Screen) */
    uint32_t c_creates = 0, c_destroys = 0, c_paints = 0, c_invalidates = 0;
};

static thread_local Shelf* t_shelf = nullptr;

/* ---- 小工具 ---------------------------------------------------------- */
static inline uint64_t k_screen()  { return mdpsr_hash64(WINMSG_SCREEN_NAME); }
static inline uint64_t k_shelf()   { return mdpsr_hash64(WINMSG_SHELF_NAME); }
static inline uint64_t k_control() { return mdpsr_hash64(WINMSG_CONTROL_NAME); }

static int shelf_find(Shelf* sh, HWND hw) {
    for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
        if (sh->hwnds[i] == hw) return (int)i;
    }
    return -1;
}

/* 拿一把目录锁; 失败返回 null (卸载中/还没建好) */
static winmsg_screen* screen_lock_any(const mdpsr_host* H) {
    void* p = nullptr;
    uint32_t g = 0;
    if (mdpsr_acquire(H, k_screen(), 0, &p, &g) != MDPSR_OK) return nullptr;
    winmsg_screen* sc = (winmsg_screen*)p;
    if (!sc || sc->magic != WINMSG_MAGIC) {
        mdpsr_release(H, k_screen());
        return nullptr;
    }
    return sc;
}
static winmsg_screen* screen_lock(Shelf* sh) { return screen_lock_any(sh->host); }
static void screen_unlock(Shelf* sh) { mdpsr_release(sh->host, k_screen()); }

/* ==========================================================================
 *  事件: 翻译 + 投递
 *
 *  WndProc 里绝不直接调用客户端的处理函数 —— 那条路会:
 *    · 绕过分发路径的 CallScope/handle 锁, 卸载可能正好"卸在你调到一半";
 *    · 让慢客户端把 GUI 线程卡死。
 *  所以一律翻译成 POD + emit (异步, 进客户端自己的队列)。客户端自己的线程慢慢处理。
 * ==========================================================================*/
static void event_send_by_name(Shelf* sh, uint64_t name, int kind, int code,
                               int mx, int my, uint32_t mods) {
    if (!name) return;
    uint64_t handle = 0;
    uint32_t want = 0;
    {
        winmsg_screen* sc = screen_lock(sh);
        if (!sc) return;
        for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
            winmsg_slot& s = sc->slots[i];
            if ((s.flags & WINMSG_F_ALIVE) && s.name == name) {
                handle = s.handle;
                want = s.events;
                break;
            }
        }
        screen_unlock(sh);
    }
    if (!handle || !want) return;
    if (kind < WINMSG_EV__COUNT && !(want & WINMSG_EV_BIT(kind))) return;

    winmsg_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev._cmd = WINMSG_CMD_EVENT;
    ev.kind = kind;
    ev.code = code;
    ev.x = mx;
    ev.y = my;
    ev.mods = mods;
    ev.window = name;

    const int rc = mdpsr_emit_from(sh->host, handle, 0, WINMSG_CMD_EVENT, &ev, sizeof(ev));

    /* 记一笔 (成功/丢包都记, 客户端和自测都能读到) */
    winmsg_screen* sc = screen_lock(sh);
    if (sc) {
        for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
            winmsg_slot& s = sc->slots[i];
            if ((s.flags & WINMSG_F_ALIVE) && s.name == name) {
                if (rc == MDPSR_OK) ++s.events_sent; else ++s.events_dropped;
                break;
            }
        }
        if (rc == MDPSR_OK) ++sc->events_sent_total; else ++sc->events_dropped_total;
        screen_unlock(sh);
    }
}

static void event_send(Shelf* sh, HWND hwnd, int kind, int code, int mx, int my, uint32_t mods) {
    const int idx = shelf_find(sh, hwnd);
    if (idx < 0) return;
    event_send_by_name(sh, sh->names[idx], kind, code, mx, my, mods);
}

/* 高频输入: 只写状态快照, 一条消息都不发 */
struct InputPatch {
    int      set_pos = 0;
    int      mx = 0, my = 0;
    int      set_buttons = 0;
    uint32_t buttons = 0;
};
static void input_note(Shelf* sh, HWND hwnd, const InputPatch& p) {
    const int idx = shelf_find(sh, hwnd);
    if (idx < 0) return;
    const uint64_t name = sh->names[idx];
    if (!name) return;
    winmsg_screen* sc = screen_lock(sh);
    if (!sc) return;
    for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
        winmsg_slot& s = sc->slots[i];
        if (!((s.flags & WINMSG_F_ALIVE) && s.name == name)) continue;
        int changed = 0;
        if (p.set_pos && (s.mx != p.mx || s.my != p.my)) {
            s.mx = p.mx; s.my = p.my; changed = 1;
        }
        if (p.set_buttons && s.buttons != p.buttons) {
            s.buttons = p.buttons; changed = 1;
        }
        if (changed) ++s.input_seq;
        break;
    }
    screen_unlock(sh);
}

/* ==========================================================================
 *  WndProc —— 窗口消息进来的地方 (GUI 线程)
 * ==========================================================================*/
static void winmsg_paint_one(Shelf* sh, HWND hwnd);   /* 见下面 */

static uint32_t mods_now() {
    uint32_t m = 0;
    if (::GetKeyState(VK_SHIFT)   & 0x8000) m |= WINMSG_MOD_SHIFT;
    if (::GetKeyState(VK_CONTROL) & 0x8000) m |= WINMSG_MOD_CTRL;
    if (::GetKeyState(VK_MENU)    & 0x8000) m |= WINMSG_MOD_ALT;
    return m;
}
static uint32_t buttons_from_wparam(WPARAM wp) {
    uint32_t b = 0;
    if (wp & MK_LBUTTON) b |= WINMSG_BTN_LEFT;
    if (wp & MK_RBUTTON) b |= WINMSG_BTN_RIGHT;
    if (wp & MK_MBUTTON) b |= WINMSG_BTN_MIDDLE;
    return b;
}

/* 窗口位置/尺寸变了 -> 把系统认的矩形写回目录 (slot.cur)。
 * 只更新 cur, **不动** win/win_done: 否则会和在途的 win_seq 请求打架, 把
 * "刚提交的新 RECT" 误标成"已经执行过"。 */
static void sync_cur_from_hwnd(Shelf* sh, HWND hwnd, int idx) {
    if (idx < 0 || !sh->names[idx]) return;
    RECT wr{};
    ::GetWindowRect(hwnd, &wr);
    const uint64_t name = sh->names[idx];
    winmsg_screen* sc = screen_lock(sh);
    if (sc) {
        winmsg_slot& s = sc->slots[idx];
        if (s.name == name && (s.flags & WINMSG_F_ALIVE)) {
            s.cur.x = wr.left; s.cur.y = wr.top;
            s.cur.w = wr.right - wr.left; s.cur.h = wr.bottom - wr.top;
        }
        screen_unlock(sh);
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Shelf* sh = t_shelf;
    if (!sh) return ::DefWindowProcW(hwnd, msg, wp, lp);

    const int mx = (int)(short)LOWORD(lp);
    const int my = (int)(short)HIWORD(lp);

    switch (msg) {
    case WM_NCHITTEST: {
        /* ★ 默认是普通客户区 (HTCLIENT): 点击/抬起/滚轮/按键全都真的到客户端。
         *   只有客户端在 slot.flags 里点名 WINMSG_F_DRAGGABLE 时, 这扇窗才整窗
         *   当标题栏 —— 代价是那扇窗收不到真实的鼠标按下/抬起 (系统拿去跑拖拽了)。
         *   上一版是无条件 HTCAPTION, 于是"能不能收到真实点击"是坏的, 而自测用
         *   PostMessage 注入 (绕过命中测试) 看不出来。 */
        const int idx = shelf_find(sh, hwnd);
        if (idx >= 0 && sh->drag_ok[idx]) return HTCAPTION;
        break;
    }

    case WM_ERASEBKGND:
        return 1;                    /* 整屏都会被贴满, 别擦背景 (免得闪) */

    case WM_PAINT: {
        /* 分层窗口的内容全部由 UpdateLayeredWindow 交上去, WM_PAINT 只是系统
         * 要求"你该画了"; 这里顺手重贴一次 (幂等), 免得万一有别的路径让它脏了。 */
        PAINTSTRUCT ps;
        ::BeginPaint(hwnd, &ps);
        winmsg_paint_one(sh, hwnd);
        ::EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOVE:
    case WM_SIZE:
        /* 用户拖了 (可拖窗口) 或者我们自己 SetWindowPos 了 -> 目录里的"实际矩形"
         * 跟上。不做这一步, 客户端读到的是旧值, 下一次 winmsg_move 会把窗口弹回去。 */
        sync_cur_from_hwnd(sh, hwnd, shelf_find(sh, hwnd));
        return 0;

    case WM_CLOSE:
        /* 不交给 DefWindowProc: 统一由 scan 去销毁 + 回收格子, 免得两条路各拆一半 */
        event_send(sh, hwnd, WINMSG_EV_CLOSE, 0, mx, my, 0);
        return 0;

    case WM_DESTROY: {
        /* ★ 先把名字取出来 (event_send_by_name 要读目录里的 handle/events),
         *   而目录里的格子要等 kill_window 里的 screen_clear 才清 ——
         *   那条路径在 DestroyWindow 返回之后, 所以这里读得到。 */
        const int idx = shelf_find(sh, hwnd);
        const uint64_t nm = (idx >= 0) ? sh->names[idx] : 0;
        if (idx >= 0) { sh->hwnds[idx] = nullptr; sh->names[idx] = 0; sh->drag_ok[idx] = false; }
        event_send_by_name(sh, nm, WINMSG_EV_DESTROYED, 0, 0, 0, 0);
        return 0;
    }

    case WM_MOUSEMOVE: {
        InputPatch p;
        p.set_pos = 1; p.mx = mx; p.my = my;
        p.set_buttons = 1; p.buttons = buttons_from_wparam(wp);
        input_note(sh, hwnd, p);
        event_send(sh, hwnd, WINMSG_EV_MOUSE_MOVE, 0, mx, my, mods_now());  /* 默认没订阅就不发 */
        return 0;
    }

#define BTN_CASE(m, kind, bit)                                                     \
    case m: {                                                                      \
        InputPatch p;                                                              \
        p.set_pos = 1; p.mx = mx; p.my = my;                                       \
        p.set_buttons = 1;                                                         \
        p.buttons = (kind == WINMSG_EV_MOUSE_DOWN)                                 \
                        ? (buttons_from_wparam(wp) | (bit))                        \
                        : (buttons_from_wparam(wp) & ~(bit));                      \
        input_note(sh, hwnd, p);                                                   \
        event_send(sh, hwnd, kind, (int)(bit), mx, my, mods_now());                \
        return 0;                                                                  \
    }
    BTN_CASE(WM_LBUTTONDOWN, WINMSG_EV_MOUSE_DOWN, WINMSG_BTN_LEFT)
    BTN_CASE(WM_LBUTTONUP,   WINMSG_EV_MOUSE_UP,   WINMSG_BTN_LEFT)
    BTN_CASE(WM_RBUTTONDOWN, WINMSG_EV_MOUSE_DOWN, WINMSG_BTN_RIGHT)
    BTN_CASE(WM_RBUTTONUP,   WINMSG_EV_MOUSE_UP,   WINMSG_BTN_RIGHT)
    BTN_CASE(WM_MBUTTONDOWN, WINMSG_EV_MOUSE_DOWN, WINMSG_BTN_MIDDLE)
    BTN_CASE(WM_MBUTTONUP,   WINMSG_EV_MOUSE_UP,   WINMSG_BTN_MIDDLE)
#undef BTN_CASE

    case WM_MOUSEWHEEL: {
        /* 滚轮的坐标是屏幕坐标, 换算成客户区再报给客户端 */
        POINT pt;
        pt.x = (LONG)(short)LOWORD(lp);
        pt.y = (LONG)(short)HIWORD(lp);
        ::ScreenToClient(hwnd, &pt);
        event_send(sh, hwnd, WINMSG_EV_WHEEL, (int)(short)HIWORD(wp), (int)pt.x, (int)pt.y, mods_now());
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        event_send(sh, hwnd, WINMSG_EV_KEY_DOWN, (int)wp, mx, my, mods_now());
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        event_send(sh, hwnd, WINMSG_EV_KEY_UP, (int)wp, mx, my, mods_now());
        return 0;

    case WM_CHAR:
        event_send(sh, hwnd, WINMSG_EV_CHAR, (int)wp, mx, my, mods_now());
        return 0;

    case WM_SETFOCUS:  event_send(sh, hwnd, WINMSG_EV_FOCUS_IN,  0, 0, 0, 0); return 0;
    case WM_KILLFOCUS: event_send(sh, hwnd, WINMSG_EV_FOCUS_OUT, 0, 0, 0, 0); return 0;

    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==========================================================================
 *  贴图 = 缩放 -> 偏置 -> 写画布 -> 交给系统裁剪
 *
 *  ★ 管线 (顺序不能换):
 *      ① 读窗口 RECT  —— 决定画布多大、窗口在屏幕哪个位置;
 *      ② 按 scale 缩放客户端的图 (最近邻), 以**原图左上角**为锚点;
 *      ③ 把锚点摆到 img_pos (客户区坐标, 左上角原点);
 *      ④ 写进画布 (画布和窗口一样大, 初始全 0);
 *      ⑤ UpdateLayeredWindow(ULW_ALPHA) 整张交系统合成 —— 画布之外不写,
 *         窗口之外由系统裁掉, 所以"出界"天然就是"透明"。
 * ==========================================================================*/

/* 按需建/重建本窗口的画布 DIB (尺寸跟着窗口走) */
static bool ensure_canvas(Shelf* sh, int idx, uint32_t w, uint32_t h) {
    Shelf::Gdi& g = sh->gdi[idx];
    if (g.dib && g.dw == w && g.dh == h) return true;

    if (g.memdc) { ::DeleteDC(g.memdc); g.memdc = nullptr; }
    if (g.dib)   { ::DeleteObject(g.dib); g.dib = nullptr; }
    g.bits = nullptr; g.dw = 0; g.dh = 0;
    if (w == 0 || h == 0) return false;

    HDC screen = ::GetDC(nullptr);
    g.memdc = ::CreateCompatibleDC(screen);
    ::ReleaseDC(nullptr, screen);
    if (!g.memdc) return false;

    BITMAPINFO bmi;
    std::memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)w;
    bmi.bmiHeader.biHeight = -(LONG)h;    /* 负 = 自上而下, row 0 是窗口顶行 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g.dib = ::CreateDIBSection(g.memdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g.dib || !bits) {
        ::DeleteDC(g.memdc); g.memdc = nullptr;
        if (g.dib) { ::DeleteObject(g.dib); g.dib = nullptr; }
        return false;
    }
    ::SelectObject(g.memdc, g.dib);
    g.bits = bits;
    g.dw = w;
    g.dh = h;
    return true;
}

static void release_gdi(Shelf* sh, int idx) {
    Shelf::Gdi& g = sh->gdi[idx];
    if (g.memdc) { ::DeleteDC(g.memdc); g.memdc = nullptr; }
    if (g.dib)   { ::DeleteObject(g.dib); g.dib = nullptr; }
    g.bits = nullptr; g.dw = g.dh = 0;
    sh->shown[idx] = false;
}

/* 把一块"预乘好的像素"写进画布, 越界自动丢弃 */
static inline void canvas_put(Shelf::Gdi& g, int x, int y, uint32_t b, uint32_t gr,
                              uint32_t r, uint32_t a) {
    if (x < 0 || y < 0 || (uint32_t)x >= g.dw || (uint32_t)y >= g.dh) return;
    uint8_t* row = (uint8_t*)g.bits + (size_t)y * g.dw * 4u;
    row[(size_t)x * 4u + 0] = (uint8_t)b;
    row[(size_t)x * 4u + 1] = (uint8_t)gr;
    row[(size_t)x * 4u + 2] = (uint8_t)r;
    row[(size_t)x * 4u + 3] = (uint8_t)a;
}

static void winmsg_paint_one(Shelf* sh, HWND hwnd) {
    const int idx = shelf_find(sh, hwnd);
    if (idx < 0) return;
    const uint64_t name = sh->names[idx];
    if (!name) return;

    /* ① 读窗口 RECT (先于读图像; 它也给出画布尺寸和屏幕位置) */
    RECT wr{};
    ::GetWindowRect(hwnd, &wr);
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;
    if (ww <= 0 || wh <= 0) return;

    /* 短拿目录锁, 只读到 surface_key */
    uint64_t skey = 0;
    {
        winmsg_screen* sc = screen_lock(sh);
        if (!sc) return;
        for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
            const winmsg_slot& s = sc->slots[i];
            if ((s.flags & WINMSG_F_ALIVE) && s.name == name && s.state == WINMSG_S_READY) {
                skey = s.surface_key;
                break;
            }
        }
        screen_unlock(sh);
    }
    if (!skey) return;

    /* ② 借客户端的画面 (同时把它钉住不让它卸载) */
    void* sp = nullptr;
    uint32_t sg = 0;
    if (mdpsr_acquire(sh->host, skey, 0, &sp, &sg) != MDPSR_OK) return;
    winmsg_surface* su = (winmsg_surface*)sp;
    if (su && su->magic == WINMSG_SURFACE_MAGIC && su->pixels && su->img_w && su->img_h &&
        ensure_canvas(sh, idx, (uint32_t)ww, (uint32_t)wh)) {

        /* ③ 画布初始全透明 */
        Shelf::Gdi& g = sh->gdi[idx];
        std::memset(g.bits, 0, (size_t)ww * (size_t)wh * 4u);

        /* ④ 缩放 + 偏置。dst = img_pos + src * scale  (src 是原图坐标, 左上角原点)
         *    scale 传 0 = 1.0; 负值 = 镜像。 */
        const float sx = su->scale_x != 0.0f ? su->scale_x : 1.0f;
        const float sy = su->scale_y != 0.0f ? su->scale_y : 1.0f;
        const int   ox = su->img_pos.x;
        const int   oy = su->img_pos.y;

        /* 缩放后图像在画布坐标里的包围盒 (可能整个在画布外) */
        double bx0 = (double)ox, by0 = (double)oy;
        double bx1 = (double)ox + (double)su->img_w * (double)sx;
        double by1 = (double)oy + (double)su->img_h * (double)sy;
        if (bx1 < bx0) { const double t = bx0; bx0 = bx1; bx1 = t; }
        if (by1 < by0) { const double t = by0; by0 = by1; by1 = t; }
        int x0 = (int)std::floor(bx0), x1 = (int)std::ceil(bx1);
        int y0 = (int)std::floor(by0), y1 = (int)std::ceil(by1);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > ww) x1 = ww;
        if (y1 > wh) y1 = wh;

        const bool any = (x1 > x0) && (y1 > y0);
        if (any) {
            const uint8_t* src = (const uint8_t*)su->pixels;
            const size_t src_stride = su->stride ? su->stride : (size_t)su->img_w * 4u;
            const float inv_x = 1.0f / sx, inv_y = 1.0f / sy;
            for (int y = y0; y < y1; ++y) {
                const int isy = (int)std::floor(((float)y - (float)oy) * inv_y);
                if (isy < 0 || isy >= (int)su->img_h) continue;
                const uint8_t* srow = src + (size_t)isy * src_stride;
                float fx = ((float)x0 - (float)ox) * inv_x;
                for (int x = x0; x < x1; ++x, fx += inv_x) {
                    const int isx = (int)std::floor(fx);
                    if (isx < 0 || isx >= (int)su->img_w) continue;
                    const uint8_t* p = srow + (size_t)isx * 4u;
                    const uint32_t a = p[3];
                    if (a == 0) continue;                       /* 全透明 -> 不写 (就是透明) */
                    const uint32_t b = p[0], gr = p[1], r = p[2];
                    if (a == 255) canvas_put(g, x, y, b, gr, r, 255);
                    else canvas_put(g, x, y, b * a / 255u, gr * a / 255u, r * a / 255u, a);  /* 预乘 */
                }
            }
        }

        /* ⑤ 整张画布交给系统合成 (ULW_ALPHA: alpha=0 的像素透出桌面) */
        POINT ptSrc{ 0, 0 };
        POINT ptPos{ wr.left, wr.top };
        SIZE  sz{ ww, wh };
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        const BOOL ulw = ::UpdateLayeredWindow(hwnd, nullptr, &ptPos, &sz, g.memdc, &ptSrc, 0, &bf, ULW_ALPHA);
        /* 窗口创建时没带 WS_VISIBLE (避免首帧闪黑), 而且 UpdateLayeredWindow
         * **不会**把窗口显示出来 —— 所以每扇窗第一次贴完图都必须自己 ShowWindow。
         * (用"全局只显示一次"的标志是错的: 重建出来的窗会永远看不见。) */
        if (ulw && !sh->shown[idx]) {
            sh->shown[idx] = true;
            ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }

        /* ⑥ 记账 (无论画没画上都记, 免得 content_seq 追不上 painted_seq 空转) */
        su->painted_seq = su->content_seq;
        ++su->paints;
        su->last_img_x = ox;
        su->last_img_y = oy;
        su->last_scale_x = sx;
        su->last_scale_y = sy;
        su->last_dst_x = any ? x0 : 0;
        su->last_dst_y = any ? y0 : 0;
        su->last_dst_w = any ? (x1 - x0) : 0;
        su->last_dst_h = any ? (y1 - y0) : 0;
        ++sh->c_paints;

        const uint64_t now = ::GetTickCount64();
        if (now - sh->last_title_ms > 500) {
            sh->last_title_ms = now;
            wchar_t t[64];
            ::swprintf_s(t, L"winmsg: paints=%u", (unsigned)sh->c_paints);
            ::SetWindowTextW(hwnd, t);
        }
    }
    mdpsr_release(sh->host, skey);
}
/* ==========================================================================
 *  建窗 / 销窗 (必须在 GUI 线程)
 * ==========================================================================*/
static const wchar_t* kClassName = L"mdpsr_winmsg_class";

static bool ensure_class(Shelf* sh) {
    if (sh->class_ready) return true;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)(const void*)&wndproc, &sh->self_mod);
    WNDCLASSEXW wc;
    std::memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = sh->self_mod;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) {
        if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        /* 同名类已经存在: 必须是"我们这个模块 + 我们这个 wndproc"才算数。
         * 否则(别人注册的 / 上一份已经被 FreeLibrary 的 winmsg 注册的)窗口消息
         * 会被派发到一段已经不在内存里的代码上 —— 那是直接崩。 */
        WNDCLASSEXW have;
        std::memset(&have, 0, sizeof(have));
        have.cbSize = sizeof(have);
        if (!::GetClassInfoExW(sh->self_mod, kClassName, &have)) return false;
        if (have.lpfnWndProc != wc.lpfnWndProc || have.hInstance != sh->self_mod) return false;
    }
    sh->class_ready = true;
    return true;
}

static HWND make_window(Shelf* sh, int x, int y, int w, int h) {
    if (!ensure_class(sh)) return nullptr;
    /* WS_EX_LAYERED + WS_POPUP, **不要** WS_VISIBLE: 分层窗口第一次
     * UpdateLayeredWindow 之前显示出来会闪一下黑底。所以先建隐藏的, 贴完第一帧
     * 再 ShowWindow (winmsg_paint_one 里做, 每扇窗各一次)。
     * WS_EX_LAYERED 让整扇窗口都是逐像素透明画布 —— alpha=0 的地方直接看到桌面。 */
    return ::CreateWindowExW(WS_EX_LAYERED, kClassName, L"winmsg", WS_POPUP,
                             x, y, w, h, nullptr, nullptr, sh->self_mod, nullptr);
}

/* 更新目录里的状态 (拿锁 -> 校验名字没被复用 -> 改 -> 放) */
static void screen_mark(Shelf* sh, int idx, uint64_t name, bool ready, uint32_t err) {
    winmsg_screen* sc = screen_lock(sh);
    if (!sc) return;
    winmsg_slot& s = sc->slots[idx];
    if (s.name == name && (s.flags & WINMSG_F_ALIVE)) {
        s.flags &= ~(WINMSG_F_WANT_CREATE | WINMSG_F_WANT_CLOSE);
        s.state = ready ? WINMSG_S_READY : WINMSG_S_FAILED;
        s.last_error = err;
    }
    screen_unlock(sh);
}

static void screen_clear(Shelf* sh, int idx, uint64_t name) {
    winmsg_screen* sc = screen_lock(sh);
    if (!sc) return;
    winmsg_slot& s = sc->slots[idx];
    if (s.name == name) std::memset(&s, 0, sizeof(s));
    screen_unlock(sh);
}

/* 拿锁 -> 校验这一格没被复用 -> 让 fn 改 -> 放锁 */
template <typename F>
static void screen_update(Shelf* sh, int idx, uint64_t name, F&& fn) {
    winmsg_screen* sc = screen_lock(sh);
    if (!sc) return;
    winmsg_slot& s = sc->slots[idx];
    if (s.name == name && (s.flags & WINMSG_F_ALIVE)) fn(s);
    screen_unlock(sh);
}

static void kill_window(Shelf* sh, int idx, bool destroyed_already) {
    HWND hw = sh->hwnds[idx];
    const uint64_t name = sh->names[idx];
    sh->hwnds[idx] = nullptr;
    sh->names[idx] = 0;
    sh->drag_ok[idx] = false;
    if (hw && !destroyed_already) {
        ::DestroyWindow(hw);            /* WM_DESTROY 会再确认一次数组 */
        ++sh->c_destroys;
    }
    release_gdi(sh, idx);               /* 画布 DIB / 内存 DC / shown 标志一起收掉 */
    if (name) screen_clear(sh, idx, name);
}

/* ==========================================================================
 *  扫目录: 建窗 / 销窗 / 挪窗 / 注入 / 采样 / 重绘 (只给自测用的两项标了 ★)
 * ==========================================================================*/

/* ★ 只给自测用: 在 Win32 边界上灌一批合成输入。
 *   下游的翻译/emit/客户端解析全都是真的 (但它是 PostMessage, 绕过命中测试)。 */
static void inject_burst(HWND hw) {
    RECT rc{};
    ::GetClientRect(hw, &rc);
    const int cx = (rc.right > 4) ? rc.right / 2 : 1;
    const int cy = (rc.bottom > 4) ? rc.bottom / 2 : 1;
    const LPARAM pos = MAKELPARAM(cx, cy);
    ::PostMessageW(hw, WM_MOUSEMOVE,   0,                pos);
    ::PostMessageW(hw, WM_LBUTTONDOWN, MK_LBUTTON,       pos);
    ::PostMessageW(hw, WM_LBUTTONUP,   0,                pos);
    ::PostMessageW(hw, WM_KEYDOWN,     VK_SPACE,         0);
    ::PostMessageW(hw, WM_KEYUP,       VK_SPACE,         0);
    ::PostMessageW(hw, WM_CHAR,        (WPARAM)' ',      0);
    /* 滚轮的 lParam 按 Win32 约定是**屏幕**坐标 (处理函数会 ScreenToClient) */
    POINT wpt{ cx, cy };
    ::ClientToScreen(hw, &wpt);
    ::PostMessageW(hw, WM_MOUSEWHEEL,  MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(wpt.x, wpt.y));
    ::PostMessageW(hw, WM_SETFOCUS,    0,                0);
    /* 方向键: 演示客户端拿它来挪窗口 —— 这条能端到端验证
     * "Win32 输入 -> broker 翻译 -> emit -> 客户端改参数 -> broker SetWindowPos" */
    ::PostMessageW(hw, WM_KEYDOWN,     VK_RIGHT,         0);
    ::PostMessageW(hw, WM_KEYUP,       VK_RIGHT,         0);
}

/* ★ 只给自测用: 在客户区中心来一次真实的左键按下+抬起
 *   (命中测试 / 焦点 / 按钮状态 / emit 全都走真路径) */
static void inject_click(HWND hw) {
    RECT rc{};
    ::GetClientRect(hw, &rc);
    const int cx = (rc.right > 4) ? rc.right / 2 : 1;
    const int cy = (rc.bottom > 4) ? rc.bottom / 2 : 1;
    const LPARAM pos = MAKELPARAM(cx, cy);
    ::PostMessageW(hw, WM_MOUSEMOVE,   0,          pos);
    ::PostMessageW(hw, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    ::PostMessageW(hw, WM_LBUTTONUP,   0,          pos);
}

static bool scan(Shelf* sh) {
    const mdpsr_host* H = sh->host;

    struct Snap { int idx; winmsg_slot s; };
    std::vector<Snap> snap;
    {
        winmsg_screen* sc = screen_lock(sh);
        if (!sc) return false;
        ++sc->scans;
        sc->frame_ms          = sh->frame_ms;
        sc->creates_total     = sh->c_creates;
        sc->destroys_total    = sh->c_destroys;
        sc->paints_total      = sh->c_paints;
        sc->invalidates_total = sh->c_invalidates;
        /* slots_used / windows_open / visible / 可拖标志 由这里统一算出来
         * (不由别处加减维护, 免得对不上) */
        uint32_t used = 0, open = 0;
        for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
            winmsg_slot& slot = sc->slots[i];
            sh->drag_ok[i] = (slot.flags & WINMSG_F_DRAGGABLE) != 0;
            /* ★ visible 必须问系统, 不能靠我们自己记账 —— 这一条就是
             *   "重建了但没显示"那个 bug 现在跑不掉的原因。 */
            slot.visible = (sh->hwnds[i] && ::IsWindowVisible(sh->hwnds[i])) ? 1u : 0u;
            if (!(slot.flags & WINMSG_F_ALIVE)) continue;
            ++used;
            if (sh->hwnds[i]) ++open;
            Snap sn;
            sn.idx = (int)i;
            sn.s = slot;
            snap.push_back(sn);
        }
        sc->slots_used   = used;
        sc->windows_open = open;
        screen_unlock(sh);
    }

    bool did = false;

    for (size_t k = 0; k < snap.size(); ++k) {
        const int idx = snap[k].idx;
        const winmsg_slot s = snap[k].s;      /* 用快照判断, 需要改再拿锁 */

        /* a. 请求方还在吗? 换代了吗? */
        bool owner_gone = false;
        {
            mdpsr_plugin_info pi;
            std::memset(&pi, 0, sizeof(pi));
            pi.struct_size = sizeof(pi);
            if (H->plugin_info(H->self, s.owner, &pi) != MDPSR_OK) owner_gone = true;
            else if (pi.status != MDPSR_PLUGIN_READY) owner_gone = true;
            else if (s.owner_gen != 0 && pi.gen != s.owner_gen) owner_gone = true;
        }
        const uint32_t sgen = mdpsr_gen_of(H, s.surface_key);
        const bool surface_gone = (sgen == 0);

        /* b. 该拆的拆 (客户端要求关 / 请求方没了 / 画面没了)
         *    DESTROYED 事件由 WM_DESTROY 里那条路径发 (那时目录格子还在) */
        if (sh->hwnds[idx] &&
            ((s.flags & WINMSG_F_WANT_CLOSE) || owner_gone || surface_gone)) {
            kill_window(sh, idx, false);
            did = true;
            continue;
        }
        /* 请求方已经没了但窗还没建出来 -> 直接回收格子, 不发事件 */
        if (!sh->hwnds[idx] && (owner_gone || surface_gone)) {
            screen_clear(sh, idx, s.name);
            did = true;
            continue;
        }

        /* c. 该建的建 (用客户端填的 window RECT) */
        if (!sh->hwnds[idx] && (s.flags & WINMSG_F_WANT_CREATE) &&
            s.win.w > 0 && s.win.h > 0) {
            HWND hw = make_window(sh, s.win.x, s.win.y, s.win.w, s.win.h);
            if (hw) {
                sh->hwnds[idx] = hw;
                sh->names[idx] = s.name;
                sh->shown[idx] = false;      /* 这扇新窗还没显示过 */
                ++sh->c_creates;
                screen_mark(sh, idx, s.name, true, 0);
                /* 把系统认的窗口矩形记进目录 (客户端可以查 winmsg_get_rect) */
                RECT wr{};
                ::GetWindowRect(hw, &wr);
                screen_update(sh, idx, s.name, [&](winmsg_slot& t) {
                    t.win_done = t.win_seq;
                    t.cur.x = wr.left; t.cur.y = wr.top;
                    t.cur.w = wr.right - wr.left; t.cur.h = wr.bottom - wr.top;
                });
                event_send_by_name(sh, s.name, WINMSG_EV_READY, 0, 0, 0, 0);
            } else {
                screen_mark(sh, idx, s.name, false, (uint32_t)::GetLastError());
            }
            did = true;
            continue;
        }

        /* d. ★参数 1: 窗口 RECT 变了? (win_seq != win_done) -> 在 GUI 线程上 SetWindowPos */
        if (sh->hwnds[idx] && s.win_seq != s.win_done) {
            const bool keep_pos  = (s.win.x == WINMSG_KEEP || s.win.y == WINMSG_KEEP);
            const bool keep_size = (s.win.w == WINMSG_KEEP || s.win.h == WINMSG_KEEP);
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            if (keep_pos)  flags |= SWP_NOMOVE;
            if (keep_size) flags |= SWP_NOSIZE;
            ::SetWindowPos(sh->hwnds[idx], nullptr,
                           keep_pos  ? 0 : s.win.x, keep_pos  ? 0 : s.win.y,
                           keep_size ? 0 : s.win.w, keep_size ? 0 : s.win.h,
                           flags);
            ::InvalidateRect(sh->hwnds[idx], nullptr, FALSE);
            ++sh->c_invalidates;
            /* 把"实际"矩形读回来写进目录 —— 这样客户端和自测看到的是系统认的几何,
             * 而不是"我们请求了什么" (证明真的挪动了)。 */
            RECT wr{};
            ::GetWindowRect(sh->hwnds[idx], &wr);
            screen_update(sh, idx, s.name, [&](winmsg_slot& t) {
                t.win_done = s.win_seq;
                t.cur.x = wr.left; t.cur.y = wr.top;
                t.cur.w = wr.right - wr.left; t.cur.h = wr.bottom - wr.top;
                ++t.win_moves;
            });
            did = true;
        }

        /* e. ★只给自测用: 注入合成输入 (一批 / 一次真点击 / 一条 WM_CLOSE) */
        if (sh->hwnds[idx] && s.inject_req) {
            if (s.inject_req == WINMSG_INJECT_CLOSE) {
                ::PostMessageW(sh->hwnds[idx], WM_CLOSE, 0, 0);
            } else if (s.inject_req == WINMSG_INJECT_CLICK) {
                inject_click(sh->hwnds[idx]);
            } else {
                inject_burst(sh->hwnds[idx]);
            }
            screen_update(sh, idx, s.name, [](winmsg_slot& t) { t.inject_req = 0; });
            did = true;
        }

        /* e2. ★只给自测用: 读画布在窗口相对坐标 (x,y) 的像素 (0xAARRGGBB, 预乘)。
         *   ★ 为什么读画布而不是读屏幕: 分层窗口是 DWM 合成的, GetPixel(GetDC(NULL))
         *     读不到它 —— 读屏幕会得到"桌面透过来"的假象 (圆也看不见)。
         *     画布就是 UpdateLayeredWindow 交上去的那张图, alpha=0 即"透出桌面",
         *     alpha=255 即不透明, 这正是我们要验的语义。 */
        if (sh->hwnds[idx] && s.probe_seq != s.probe_done) {
            uint32_t color = 0xFFFFFFFFu;   /* 越界 / 没画布 */
            Shelf::Gdi& g = sh->gdi[idx];
            if (g.bits && s.probe_x >= 0 && s.probe_y >= 0 &&
                (uint32_t)s.probe_x < g.dw && (uint32_t)s.probe_y < g.dh) {
                const uint32_t* row = (const uint32_t*)g.bits + (size_t)s.probe_y * g.dw;
                color = row[s.probe_x];
            }
            screen_update(sh, idx, s.name, [&](winmsg_slot& t) {
                t.probe_color = color;
                t.probe_done = s.probe_seq;
            });
            did = true;
        }

        /* f. ★ 画面变了 (content_seq != painted_seq) -> 立刻重绘。
         *    分层窗口的内容是 UpdateLayeredWindow 交上去的, 所以这里直接组合画布。 */
        if (sh->hwnds[idx]) {
            bool dirty = false;
            void* sp = nullptr;
            uint32_t sg = 0;
            if (mdpsr_acquire(H, s.surface_key, 0, &sp, &sg) == MDPSR_OK) {
                winmsg_surface* su = (winmsg_surface*)sp;
                if (su && su->magic == WINMSG_SURFACE_MAGIC &&
                    su->content_seq != su->painted_seq) {
                    ++su->invalidates;
                    ++sh->c_invalidates;
                    dirty = true;
                }
                mdpsr_release(H, s.surface_key);
            }
            if (dirty && sh->hwnds[idx]) {
                winmsg_paint_one(sh, sh->hwnds[idx]);   /* 缩放+偏置+组合+交系统 */
                did = true;
            }
        }
    }
    return did;
}

/* ==========================================================================
 *  GUI 线程
 *
 *  一轮五步:
 *    1) 泵窗口消息 (必须有! 不然窗口不刷新/不响应/5 秒后被判"未响应")
 *    2) 扫目录 (短拿锁): 建窗/销窗/挪窗/注入/采样
 *    3) 建窗销窗都在本线程做 (CreateWindowEx/DestroyWindow 有线程亲和)
 *    4) 真正贴图: 组合画布 + UpdateLayeredWindow (也在本线程)
 *    5) 有活就立刻回去泵, 没活就睡到下一帧 —— MsgWaitForMultipleObjects 带超时,
 *       有消息立刻醒, 空闲时 CPU 0%
 * ==========================================================================*/
static void gui_thread(Shelf* sh) {
    t_shelf = sh;
    ::SetThreadDescription(::GetCurrentThread(), L"winmsg-gui");
    sh->running.store(1, std::memory_order_release);

    uint64_t next_frame = ::GetTickCount64();
    while (!sh->stop.load(std::memory_order_acquire)) {
        /* 1) 泵窗口消息 */
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { sh->stop.store(1); break; }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (sh->stop.load(std::memory_order_acquire)) break;

        /* 2~4) 扫一遍 (建/销/挪/注入/采样/贴图) */
        const bool did = scan(sh);

        /* 5) 节流 */
        const uint64_t now = ::GetTickCount64();
        DWORD wait = 0;
        if (!did) {
            if (now < next_frame) wait = (DWORD)(next_frame - now);
            next_frame = now + sh->frame_ms;
        }
        if (wait > 0) {
            ::MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT);
        } else {
            ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
        }
    }

    /* 退出前必须在自己线程上把所有窗口和 GDI 资源收干净 */
    for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
        if (sh->hwnds[i]) {
            HWND hw = sh->hwnds[i];
            sh->hwnds[i] = nullptr;
            sh->names[i] = 0;
            sh->drag_ok[i] = false;
            ::DestroyWindow(hw);
        }
        release_gdi(sh, i);
    }
    if (sh->class_ready && sh->self_mod) {
        ::UnregisterClassW(kClassName, sh->self_mod);
        sh->class_ready = false;
    }
    t_shelf = nullptr;
    sh->running.store(0, std::memory_order_release);
}

/* ==========================================================================
 *  导出: State winmsg.Screen
 * ==========================================================================*/
MDPSR_EXPORT void* mdpsr_state_winmsg_Screen(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(winmsg_screen), alignof(winmsg_screen));
    if (!mem) return nullptr;
    winmsg_screen* sc = (winmsg_screen*)mem;
    std::memset(sc, 0, sizeof(*sc));
    sc->magic = WINMSG_MAGIC;
    sc->version = WINMSG_VERSION;
    sc->frame_ms = 16;
    mdpsr_log(ctx->host, 0, (std::string("[winmsg] 目录已挂载 (winmsg.Screen, ") +
                             std::to_string(WINMSG_MAX_WINDOWS) + " 个窗口位)").c_str());
    return sc;
}

/* ==========================================================================
 *  导出: Object winmsg.Shelf
 * ==========================================================================*/
MDPSR_EXPORT void* mdpsr_object_winmsg_Shelf(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(Shelf), alignof(Shelf));
    if (!mem) return nullptr;
    Shelf* sh = new (mem) Shelf();
    sh->host = ctx->host;
    sh->pool = ctx->pool;
    sh->plugin = ctx->plugin;
    mdpsr_log(ctx->host, 0, "[winmsg] GUI 架已构造 (winmsg.Shelf)");
    return sh;
}

MDPSR_EXPORT void mdpsr_object_winmsg_Shelf_destroy(void* inst, const mdpsr_factory_ctx* ctx) {
    (void)ctx;
    Shelf* sh = (Shelf*)inst;
    if (!sh) return;
    /* ★ 卸载前必须停线程 + join: 框架只 join 它自己建的队列线程, 不会管我们
     *   自己开的这条。忘了 join = DLL 卸掉时线程还在跑 = 崩。 */
    sh->stop.store(1, std::memory_order_release);
    if (sh->th.joinable()) sh->th.join();
    if (sh->host) {
        mdpsr_log(sh->host, 0, (std::string("[winmsg] GUI 线程已停 (建窗 ") +
                                std::to_string(sh->c_creates) + " / 贴图 " +
                                std::to_string(sh->c_paints) + " / 重绘请求 " +
                                std::to_string(sh->c_invalidates) + ")").c_str());
    }
    sh->~Shelf();
}

/* ==========================================================================
 *  导出: Handle winmsg.Control
 *
 *  它不是 GUI 线程上跑的 —— 它只是"控制台": 最重要的职责是 cmd=0 时启动
 *  GUI 线程 (那是整个装载流程的最后一步, 目录和架子都已经就位)。
 * ==========================================================================*/
MDPSR_EXPORT int mdpsr_handle_winmsg_Control(const mdpsr_msg* msg, const uint8_t* body,
                                             uint32_t len, const mdpsr_ctx* ctx) {
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    /* ---- cmd=0: 只借"架子"(Object), 启动 GUI 线程 ---- */
    if (msg->cmd == MDPSR_CMD_INIT) {
        void* sp = nullptr;
        uint32_t sg = 0;
        if (mdpsr_acquire(H, k_shelf(), 0, &sp, &sg) != MDPSR_OK) return MDPSR_ERR_NOT_READY;
        Shelf* sh = (Shelf*)sp;
        int rc = MDPSR_OK;
        if (!sh) {
            rc = MDPSR_ERR_BAD_STATE;
        } else if (sh->started.exchange(1, std::memory_order_acq_rel) == 0) {
            /* exchange 保证"只启动一次": 检查-再启动要是分成两步, 第二发会去赋值
             * 一个 joinable 的 std::thread —— 那是 std::terminate。 */
            sh->stop.store(0, std::memory_order_release);
            sh->th = std::thread([sh] { gui_thread(sh); });
            mdpsr_log(H, 0, (std::string("[winmsg] GUI 线程已启动 (帧间隔 ") +
                             std::to_string(sh->frame_ms) + "ms)").c_str());
        }
        mdpsr_release(H, k_shelf());
        return rc;
    }

    /* ---- 其余命令只借"目录"(State)。
     * ★ 千万不要在同一个调用里既借 Shelf 又借 Screen: 两个键的哈希大小是任意的,
     *   嵌套借就会违反"按键升序"规则 -> LOCK_ORDER。这是自测抓出来的真问题。 ---- */
    winmsg_screen* sc = screen_lock_any(H);
    if (!sc) return MDPSR_ERR_NOT_READY;

    int rc = MDPSR_OK;
    switch (msg->cmd) {
    case WINMSG_CMD_STATS:
        if (msg->src) mdpsr_send_reply(H, msg, MDPSR_CMD_REPLY, sc, sizeof(winmsg_screen));
        break;

    case WINMSG_CMD_CLOSE_ALL:
        for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
            if (sc->slots[i].flags & WINMSG_F_ALIVE) sc->slots[i].flags |= WINMSG_F_WANT_CLOSE;
        }
        break;

    case WINMSG_CMD_INJECT: {
        /* ★ 只给自测用: 让 GUI 线程往某个窗口注入输入。
         *   body = { uint64_t name; uint32_t kind; uint32_t pad; }
         *   只给 8 字节 (老写法) 也收: 默认当"一批合成输入"。 */
        struct InjectReq { uint64_t name; uint32_t kind; uint32_t pad; } req;
        std::memset(&req, 0, sizeof(req));
        if (body && len >= 8) {
            std::memcpy(&req, body, len < sizeof(req) ? len : sizeof(req));
        } else {
            rc = MDPSR_ERR_BAD_MESSAGE;
            break;
        }
        if (req.kind == 0) req.kind = WINMSG_INJECT_BURST;
        bool hit = false;
        for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
            if ((sc->slots[i].flags & WINMSG_F_ALIVE) && sc->slots[i].name == req.name) {
                sc->slots[i].inject_req = req.kind;
                hit = true;
                break;
            }
        }
        if (!hit) rc = MDPSR_ERR_NOT_FOUND;
        break;
    }

    case WINMSG_CMD_PROBE: {
        /* ★ 只给自测用: 读窗口某个像素。载荷 = {窗口名, x, y} */
        struct ProbeReq { uint64_t name; int32_t x, y; int32_t pad; } req;
        if (!mdpsr_read(body, len, &req, sizeof(req))) { rc = MDPSR_ERR_BAD_MESSAGE; break; }
        bool hit = false;
        for (uint32_t i = 0; i < WINMSG_MAX_WINDOWS; ++i) {
            if ((sc->slots[i].flags & WINMSG_F_ALIVE) && sc->slots[i].name == req.name) {
                sc->slots[i].probe_x = req.x;
                sc->slots[i].probe_y = req.y;
                sc->slots[i].probe_seq++;
                sc->slots[i].probe_color = 0xFFFFFFFFu;
                hit = true;
                break;
            }
        }
        if (!hit) rc = MDPSR_ERR_NOT_FOUND;
        break;
    }

    default:
        rc = MDPSR_ERR_UNKNOWN_CMD;
        break;
    }
    mdpsr_release(H, k_screen());
    return rc;
}

/* ==========================================================================
 *  模块导出
 * ==========================================================================*/
MDPSR_DECL_ABI_VERSION()

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    mdpsr_log(ctx->host, 0, "[winmsg] 已挂载 (GUI broker; 宿主主线程不参与分发)");
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(void) {
    /* 线程的收尾在 Object 的 _destroy 里做 (那时架子还在) */
}
