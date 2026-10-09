# GUI 与 GUI 插件（winmsg）—— ABI v3 / winmsg 协议 v2

> GUI 的唯一权威文档。README / 架构.md / 插件规范.md 只留指路。
>
> 两个角色：
> * **GUI 插件 `winmsg`** —— 框架里**唯一**碰 Win32 的组件。它自己开一条 GUI 线程，
>   建窗口、泵消息、贴图。
> * **GUI 客户端** —— 任何想显示东西的普通插件。**它一行 Windows 代码都不用写。**

---

## 一、示例GUI
窗口是"透明分层窗口"，画面是一块四通道内存图

```
┌─ winmsg (GUI 插件, C++) ────────────────────────────────────────────────┐
│  State  winmsg.Screen   目录: 窗口名 -> 目录项 (两端都读写, 走 acquire)   │
│  Object winmsg.Shelf    HWND 表 + GUI 线程 + 每窗口一张透明画布 DIB        │
│  Handle winmsg.Control  控制入口 (跑在 Queue_default 上)                 │
│                                                                          │
│  GUI 线程一轮: 泵窗口消息 -> 扫目录 -> 建窗/销窗/挪窗 ->                  │
│                每个脏窗口: 缩放 + 偏置 + 组合画布 + UpdateLayeredWindow     │
└──────────────────────────────────────────────────────────────────────────┘
        ▲ 登记窗口 / 改参数 / 收事件            │ 读参数 + 组合画布
        │                                       ▼
┌─ GUI 客户端 (纯 C, 不含 windows.h) ─────────────────────────────────────┐
│  State  <插件名>.Surface   画面: 图像指针 + 缩放 + 偏置 + 序号             │
│  Handle <自己的>            自己的节奏 (可选: 拿队列 pace_ms 当动画钟)     │
└──────────────────────────────────────────────────────────────────────────┘
```

**窗口是 `WS_EX_LAYERED` 分层窗口，默认整扇都是纯透明的**：

* 画布是一张和窗口一样大的 32bpp 预乘 BGRA DIB，**初始全 0（alpha=0）**；
* 每次重绘：客户端给的图先**缩放**、再**偏置**、然后最近邻采样写进画布，
  最后 `UpdateLayeredWindow(ULW_ALPHA)` 整张交给系统合成；
* **没被图写到的画布像素 alpha=0 → 直接透出窗口后面的桌面/别的窗口**；
* 图超出窗口（画布）的部分根本不写 → 同样是透明；窗口超出屏幕的部分由系统裁掉。

**宿主主线程完全没参与**：它不建窗口、不泵 Win32、也不拥有任何队列。GUI 线程是
`winmsg` 用 `std::thread` 自己开的，所以"卸载前 join"是它自己的责任（放在 Object 的
`_destroy` 里，自测每次收工都走一遍）。

---

## 二、渲染管线（这一段是全部参数的语义来源）

```
① 缩放    dst = (src - 0) * scale        src 是原图坐标, 锚点是**原图左上角**
② 偏置    dst = dst + img_pos            img_pos 是"原图左上角"在客户区里的位置
③ 写画布  画布和客户区一样大, 越界的点直接不写 (= 透明)
④ 系统裁剪 窗口在屏幕外的部分由系统裁掉
```

坐标原点**统一是左上角**（和 Win32 一致）：客户区原点在窗口左上角，x 向右、y 向下。

由此得到的几条性质（自测逐条验过）：

| 现象 | 为什么 |
| --- | --- |
| 图画在哪儿不会错位 | 缩放锚点就是"原图左上角"，偏置也作用在它上面 |
| 图比窗口大 → 只看见一部分 | ③ 只写画布内的点 |
| 偏置是负的 → 图的左上角跑到窗口外 | ③ 同样只写画布内的点，剩下的部分照常画 |
| 窗口里没画的地方是桌面 | 画布初始全 0，`ULW_ALPHA` 直接把 alpha=0 的像素透出去 |
| 放大就是像素块 | 采样是最近邻（不插值）—— 要平滑请客户端自己先缩放好 |

---

## 三、资源地图

| 资源 | 类型 | 谁写 | 装什么 |
| --- | --- | --- | --- |
| `winmsg.Screen` | State（公共） | 两端各写各的字段 | 16 个窗口位：名字、谁要的、**窗口 RECT**、状态、事件发到哪、高频输入快照、计数、探测结果 |
| `winmsg.Shelf` | Object（**私有**） | 只有 GUI 线程 | `HWND` 表、窗口类、线程、**每窗口一张透明画布 DIB + 内存 DC** |
| `winmsg.Control` | Handle | 宿主/内核 | `cmd=0` 启动 GUI 线程；统计查询；关全部窗；**自测钩子（注入 / 采画布像素）** |
| `<插件名>.Surface` | State（公共） | 客户端写参数，broker 写计数 | 图像指针、尺寸、**缩放**、**偏置**、`content_seq`、已贴序号、实际绘制区域 |

两条硬性质：

* **`HWND` 出不了 `winmsg`**：Shelf 是 Object，别的插件 `acquire` 直接吃 `NO_PERM`（11016）。
* **每个窗口一把自己的锁**：画面在**客户端自己的 State** 里，broker 贴图借的是那一格
  自己的锁，不是全局大锁 —— 客户端之间不互相阻塞。

---

## 四、可调参数

### 4.1 两个几何参数一览

| | 参数 1 | 参数 2 |
| --- | --- | --- |
| 类型 | **RECT**（`winmsg_rect {x,y,w,h}`） | **POINT**（`winmsg_point {x,y}`）+ **缩放**（`scale_x/scale_y`） |
| 在哪 | `winmsg.Screen` 的槽 `slot.win` | `<客户端>.Surface` 的 `surf.img_pos` / `surf.scale_*` |
| 管什么 | **窗口在屏幕上的绝对位置与尺寸** | **图在窗口里怎么摆放**：先按 scale 缩放，再把原图左上角放到 img_pos |
| 基准 | 屏幕坐标（左上角原点） | 窗口**客户区**坐标（左上角原点，x 向右 y 向下） |
| 怎么改 | `winmsg_set_rect` / `winmsg_move` / `winmsg_resize` | `winmsg_set_offset`（= `winmsg_place_image`）/ `winmsg_set_scale`，或在 `winmsg_lock_ex` 里直接改 |
| 触发 | `slot.win_seq++` | `surf.content_seq++`（`winmsg_unlock(...,1)`） |
| 谁执行 | GUI 线程（`SetWindowPos` 必须在创建窗口的线程上） | GUI 线程（组合画布 + `UpdateLayeredWindow`） |
| 生效 | 下一帧（≤ `frame_ms`，默认 16ms） | 下一帧 |
| 回读 | `winmsg_get_rect` → `slot.cur`（**系统认的**实际矩形） | `surf.last_dst_*` / `last_img_*` / `last_scale_*` |

**参数 1 链路**：`winmsg_move` → 填 `slot.win` + `win_seq++` → GUI 线程 `SetWindowPos` →
`GetWindowRect` 写回 `slot.cur` → 下次组合画布按新窗口大小。
用户自己拖动窗口（只对开了 `WINMSG_F_DRAGGABLE` 的窗口）时，`WM_MOVE` 也会把
系统认的矩形写回 `slot.cur`，所以客户端按 `cur` 算出来的下一次移动不会"弹回去"。

**参数 2 链路**：改 `scale`/`img_pos` + `content_seq++` → GUI 线程发现
`content_seq != painted_seq` → 缩放 + 偏置 + 组合画布 → `UpdateLayeredWindow` →
屏幕上的圆挪到新位置，旧位置透明。

**为什么用序号（`win_seq` / `content_seq`）而不是布尔标志**：客户端在 broker 处理请求的
瞬间又改了一次，布尔标志会被连同旧值一起清掉 → 新值丢了。序号只记"我执行到哪一版"，
永不丢。

### 4.2 内存图像的格式约定（4 通道）

| 项 | 约定 |
| --- | --- |
| 通道数 | **4**（每像素 4 字节） |
| 内存字节序 | `B`, `G`, `R`, `A`（当 `uint32_t` 读 = `0xAARRGGBB`） |
| A 的含义 | 0 = 完全透明，255 = 完全不透明，中间值半透明 |
| 行序 | **自上而下**（第 0 行是图像的顶行） |
| 行距 | `stride`，0 = `img_w * 4` |
| 预乘 alpha | **不用**，客户端给直通值，预乘由 broker 贴图时做 |
| `format` | 目前只有 `0 = BGRA8888` |
| 缩放 | `scale_x/scale_y`，0 表示 1.0，负值 = 镜像；采样是**最近邻** |

**"透明底 + 纯色圆"就是这么画的**：圆内 `alpha=255`、圆外 `alpha=0`，把图贴到透明画布上
就得到"窗口里只有个圆，圆外是桌面"。`winmsg_draw_circle(s, 0xFFRRGGBB)` 就是这个演示画法。

### 4.3 画面参数（`<插件名>.Surface`，全部在 `winmsg_lock_ex` / `winmsg_unlock` 之间改）

| 我想…… | 改哪个字段 | 说明 |
| --- | --- | --- |
| 换一张图（换内存指针） | `pixels` | 指向自己池里的内存；**换指针/释放旧图都必须在锁内做** |
| 换图尺寸 | `img_w` / `img_h` | broker 用它解释 `pixels` 那块内存有多大 |
| 换行距 / 格式 | `stride`（0 = `img_w*4`）/ `format`（0 = BGRA8888） | |
| **缩放** | `scale_x` / `scale_y` | 锚点是原图左上角；0 = 1.0；负值 = 镜像 |
| **图像在窗口里的位置** | `img_pos`（POINT） | **原图左上角**落在客户区的这个点上 |
| **触发一次重绘** | `winmsg_unlock(..., changed=1)` → `content_seq++` | broker 每帧比较 `content_seq != painted_seq` |
| 只改统计、不想重绘 | `winmsg_unlock(..., changed=0)` | 免得白贴一帧 |
| 读 broker 实际画到哪 | `last_dst_x/y/w/h`、`last_img_x/y`、`last_scale_x/y` | "我改的参数到底生效了吗"看这里 |
| 画一张纯色图 / 一个圆 | `winmsg_fill_solid` / `winmsg_draw_circle` | 演示与自测用的最省事画法 |

### 4.4 透明与出界的语义

* **窗口默认纯透明**：画布初始全 0，`alpha=0` 的像素 `ULW_ALPHA` 之后直接透出桌面。
* **出界不用特判**：图超出画布的点不写、窗口超出屏幕的部分系统裁掉 —— 两层"裁剪"
  都不需要客户端或 broker 算交集。
* **整块出界**：`last_dst_w/h == 0`（broker 记的绘制区为空），这一版照样记成"已贴过"
  （`painted_seq = content_seq`），所以不会因为"什么都没画"而空转重绘。

### 4.5 窗口几何（参数 1）注意点

| 场景 | 怎么办 |
| --- | --- |
| 只想挪位置 | `winmsg_move(H, 名, x, y)` |
| 只想改大小 | `winmsg_resize(H, 名, w, h)` |
| 一起改 / 只改一维 | `winmsg_set_rect(H, 名, x, y, w, h)`，不想动的填 `WINMSG_KEEP`（不能填 0，0 是合法坐标） |
| 查现在到底在哪、多大 | `winmsg_get_rect(H, 名, &rc)` → 系统认的实际矩形 |
| 窗口还没建好（`state != READY`） | 改几何无效，直接用 `winmsg_open` 的位置尺寸 |

### 4.6 事件订阅与"整窗可拖"

| 我想…… | 调什么 |
| --- | --- |
| 窗口事件发到某个 handle | `winmsg_subscribe(H, 名, 收事件的handle键, 掩码)` |
| 只要某几类 | 掩码里点名 `WINMSG_EV_BIT(kind)` |
| 一条消息都不要（纯显示面板） | 不调 `winmsg_subscribe`（默认 `events = 0`） |
| 连鼠标移动的消息也要 | 掩码加 `WINMSG_EV_BIT(WINMSG_EV_MOUSE_MOVE)` |
| **这扇窗整窗可拖**（用户按住任意位置就能搬窗口） | `winmsg_set_draggable(H, 名, 1)`（= `slot.flags |= WINMSG_F_DRAGGABLE`） |

> **可拖的代价**：`WINMSG_F_DRAGGABLE` 的窗口会把命中测试报成"标题栏"，于是系统用
> 鼠标拖拽把窗口搬走，**那扇窗真实的按下/抬起事件就到不了客户端了**（点击被当拖拽吃掉）。
> 默认**不开**：默认是普通客户区，点击/抬起/滚轮/按键全都真的到客户端。
> 这是自测抓出来的一个真问题 —— 上一版无条件 `HTCAPTION`，于是"能不能收到真实点击"
> 是坏的，而自测当时用 `PostMessage` 注入（绕过命中测试）看不出来。

### 4.7 状态、可见性与关窗

| 我想…… | 调什么 | 返回 |
| --- | --- | --- |
| 窗口建好了吗 | `winmsg_state(H, 名)` | `0`=没有 / `PENDING` / `READY` / `FAILED` |
| **这扇窗真的在屏幕上吗** | `winmsg_is_visible(H, 名)` | broker 从系统读回来的 `IsWindowVisible` |
| 请求关窗 | `winmsg_close(H, 名)` | broker 下一帧拆窗 + 回收格子 |
| 读高频输入 | `winmsg_read_input(H, 名, &in)` | `mx/my/buttons/input_seq` + 事件计数 |
| 事件有没有丢 | `in.events_dropped` | 客户端队列满时丢的条数 |

`state == READY` 只说明"目录上登记好了"，**`visible` 才是"用户看得见"**。自测会两条都查
（`winmsg_is_visible` 就是那个"重建了但没显示"的回归测试）。

**"窗口被关掉了"不需要消息也能知道**：`winmsg_state()` 返回 `0` 就是。
这时**重新 `winmsg_open`（名字不用改）** 就能再要一个窗口 —— 热插拔之后客户端就是靠
这条自己把 GUI 要回来的。

> ⚠ **重新登记时订阅要一起重新声明**（事件发到哪个 handle / 要哪些 / 可否拖动）。
> 格子跟着窗口一起回收，所以这些字段也没了。为免踩坑，一律用
> **`winmsg_open_ex(H, 名, x,y,w,h, 事件handle, 掩码, 可否拖动, &surface_key)`**：
> 首次点火和自愈走同一段代码。（这个坑自测抓到过：点击关窗自愈之后 `WM_CLOSE` 收不到了。）

---

## 五、消息队列：窗口事件怎么走到插件

### 5.1 流程

```
GUI 线程: WndProc(hwnd, WM_KEYDOWN, VK_RIGHT, ...)
  ├─ 由 HWND 反查窗口名 (Shelf 那张表只有本线程碰, 不用锁)
  ├─ 短拿 winmsg.Screen 读 {handle, events} 快照 -> 放锁
  ├─ events 里点名了吗? handle 非 0 吗? 没 -> 什么都不发 (默认档)
  ├─ 翻译成纯 C 的 POD: winmsg_event { _cmd=16, kind, code, x, y, mods, window }
  └─ host->emit_from(handle, src=0, WINMSG_CMD_EVENT, &ev, sizeof(ev))
         │  (框架队列, 异步)
         ▼
客户端自己的队列线程: handle(收到 cmd=16) -> mdpsr_read 长度校验 -> switch (ev.kind)
```

**异步投递**：`emit` 只是塞进客户端 handle 所在的那条队列（FIFO），处理在客户端自己的
队列线程上。**慢客户端卡不住界面**；队列满时 `emit` 返回 `QUEUE_FULL`，broker 记一笔
`slot.events_dropped` 继续。

### 5.2 为什么不在 WndProc 里直接调客户端

| 如果同步调用 | 后果 |
| --- | --- |
| 不在分发路径上（没有 `CallScope`/handle 锁） | 卸载可能正好"卸在你调到一半" |
| 客户端慢 | GUI 线程被卡住 → 掉帧 / 5 秒后"未响应" |
| 跨插件同步调用 | 违反"插件之间只通过 emit 投消息"（FreeLibrary 安全的前提） |

### 5.3 事件体 + 种类

```c
typedef struct winmsg_event {
    int32_t  _cmd;      /* = WINMSG_CMD_EVENT (16) */
    int32_t  kind;      /* WINMSG_EV_* */
    int32_t  code;      /* VK 码 / 字符 / 按钮位 / 滚轮增量 */
    int32_t  x, y;      /* 客户区坐标 */
    uint32_t mods;      /* WINMSG_MOD_* */
    uint32_t pad;
    uint64_t window;    /* 窗口名哈希 */
} winmsg_event;
```

| `kind` | 何时来 | `code` / `x,y` |
| --- | --- | --- |
| `READY` | 窗口建好（发一次） | — |
| `CLOSE` | 用户关（Alt+F4 / 点的关闭） | — |
| `DESTROYED` | 窗口已销毁（最后一次） | — |
| `KEY_DOWN` / `KEY_UP` | 键盘 | `code` = VK |
| `CHAR` | 字符 | `code` = 字符码 |
| `MOUSE_DOWN` / `MOUSE_UP` | 鼠标键 | `code` = 按钮位；`x,y` = 客户区坐标 |
| `WHEEL` | 滚轮 | `code` = 有符号增量（一格 = 120） |
| `FOCUS_IN` / `FOCUS_OUT` | 焦点 | — |
| `MOUSE_MOVE` | **默认不发**，点名才发 | `x,y` |

### 5.4 高频输入走状态快照

鼠标移动一秒上百条，走消息会把客户端队列灌满。所以 `WM_MOUSEMOVE`/按键状态只写
`slot.mx/my/buttons` 并 `input_seq++`，客户端在自己的节拍里轮询一次。

> 一条判据：**每条都要处理、顺序有意义 → 消息；只要最新一版 → 状态。**

---

## 六、客户端最小骨架（透明底 + 纯色圆）

清单（`Init` 指向自己的 Tick handle）：

```jsonc
{
  "name": "myplugin",
  "dll_path": "mdpsr_myplugin.dll",
  "State":  ["myplugin.Surface"],
  "Handle": ["myplugin.Tick", "myplugin.Events"],
  "Queue":  ["Queue_myplugin"],            // 两个 handle 同一条队列 -> 天然串行
  "Init":   "myplugin.Tick"
}
```

```c
#include "mdpsr/abi.h"
#include "winmsg_client.h"

#define MY_WIN  "myplugin.Window"
#define MY_TICK (MDPSR_CMD_USER_BASE + 10)
#define MY_MASK (WINMSG_EV_BIT(WINMSG_EV_READY) | WINMSG_EV_BIT(WINMSG_EV_CLOSE) | \
                 WINMSG_EV_BIT(WINMSG_EV_KEY_DOWN))

MDPSR_EXPORT void* mdpsr_state_myplugin_Surface(const mdpsr_factory_ctx* ctx) {
    void* mem = winmsg_surface_make(ctx, 96, 96);     /* 图像内存从自己的池里出 */
    if (mem) winmsg_draw_circle((winmsg_surface*)mem, 0xFF8040C0u);   /* 透明底 + 纯色圆 */
    return mem;
}
MDPSR_EXPORT int mdpsr_queue_Queue_myplugin(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out) {
    out->struct_size = sizeof(*out); out->capacity = 8192; out->pace_ms = 16; out->flags = 0;
    return MDPSR_OK;                                   /* pace_ms 就是动画钟 */
}

MDPSR_EXPORT int mdpsr_handle_myplugin_Tick(const mdpsr_msg* msg, const uint8_t* body,
                                            uint32_t len, const mdpsr_ctx* ctx) {
    const mdpsr_host* H = ctx->host;
    const uint64_t win  = mdpsr_hash64(MY_WIN);
    const uint64_t surf = winmsg_my_surface_key(H);
    uint32_t zero = 0;

    if (msg->cmd == MDPSR_CMD_INIT) {
        /* ★ 一步到位: 要窗口 + 订阅 + (可选)可拖 —— 自愈路径也用它 */
        if (winmsg_open_ex(H, win, 160, 160, 220, 160,
                           mdpsr_hash64("myplugin.Events"), MY_MASK, 0, NULL) != MDPSR_OK) return MDPSR_OK;
        return mdpsr_emit(H, mdpsr_hash64("myplugin.Tick"), MY_TICK, &zero, sizeof(zero));
    }
    if (msg->cmd != MY_TICK) return MDPSR_ERR_UNKNOWN_CMD;   /* 判命令只读 msg->cmd */

    if (winmsg_state(H, win) == 0) {                    /* 窗口没了 -> 自己再要一个 */
        winmsg_open_ex(H, win, 160, 160, 220, 160,
                       mdpsr_hash64("myplugin.Events"), MY_MASK, 0, NULL);
        return mdpsr_emit(H, mdpsr_hash64("myplugin.Tick"), MY_TICK, &zero, sizeof(zero));
    }

    static uint32_t f = 0;
    winmsg_surface* s = NULL;
    if (winmsg_lock_ex(H, surf, &s) == MDPSR_OK) {       /* 忙/没了都不算错 */
        winmsg_draw_circle(s, 0xFF000000u | ((f * 3u) & 0xFFu));   /* 内存图片: 换色动画 */
        s->scale_x = 1.0f; s->scale_y = 1.0f;                      /* 参数 2: 缩放 */
        s->img_pos.x = (int32_t)(f % 100);                          /* 参数 2: 偏置 */
        s->img_pos.y = 20;
        ++f;
        winmsg_unlock(H, surf, s, 1);                    /* changed=1 -> 发布新一版 */
    }
    return mdpsr_emit(H, mdpsr_hash64("myplugin.Tick"), MY_TICK, &zero, sizeof(zero));
}
```

（真实例子：`src/components/paint/paint.cpp`（演示客户端），以及
`test/circ_a|circ_b|circ_c`（自测夹具，逻辑共用 `test/gui_client_common.h`）。）

### 6.1 客户端助手一览（`winmsg_client.h`，纯 C）

| 函数 | 作用 |
| --- | --- |
| `winmsg_surface_make[_ex](ctx, w, h[, extra])` | State 工厂里分配"载荷 + 像素" |
| `winmsg_surface_extra(s)` | 取像素区后面的扩展数据 |
| `winmsg_open(H, 名, x, y, w, h, &surf_key)` | 登记窗口 |
| **`winmsg_open_ex(H, 名, x,y,w,h, 事件handle, 掩码, 可否拖动, &surf_key)`** | **登记 + 订阅 + 可拖，一步到位（推荐）** |
| `winmsg_subscribe` / `winmsg_set_draggable` | 单独改订阅 / 单独改可拖 |
| `winmsg_state` / `winmsg_is_visible` | 轮询状态 / 轮询"真的在屏幕上吗" |
| `winmsg_close(H, 名)` | 请求关窗（完成后 `state` 归 0，可以再 `winmsg_open`） |
| `winmsg_lock_ex(H, key, &s)` / `winmsg_lock` / `winmsg_unlock(H, key, s, changed)` | 借/还画面（`lock_ex` 会告诉你 BUSY 还是没了） |
| `winmsg_set_scale(H, key, sx, sy)` | 参数 2：缩放 |
| `winmsg_set_offset`（= `winmsg_place_image`） | 参数 2：偏置 |
| `winmsg_set_rect` / `winmsg_move` / `winmsg_resize` / `winmsg_get_rect` | 参数 1 |
| `winmsg_last_dst(H, key, &rect, &offset, &scale)` | 读 broker 实际画到哪、用了什么参数 |
| `winmsg_read_input(H, 名, &in)` | 高频输入快照 |
| `winmsg_fill_solid(s, b, g, r)` / `winmsg_draw_circle(s, argb)` | 刷纯色 / 画透明底纯色圆 |

---

## 七、硬规则（违反会崩、不刷新、或者收不到事件）

1. **`pixels` 只在 `winmsg_lock_ex` 和 `winmsg_unlock` 之间有效。** 那把锁同时是"我正在用它"
   的声明：broker 贴图也借它，客户端卸载会被推迟（抢不到锁 → `BUSY` → 下轮再来）。
   **绕过锁读 `pixels` = 拿悬垂指针**。
2. 换图像指针 / 释放旧图，都必须在锁内做。
3. 客户端内部顺序：先读状态（`winmsg_state`/`winmsg_read_input`/`winmsg_get_rect`）→ 再 `winmsg_lock_ex`。
4. **别在一个调用里嵌套借两个资源**（键必须严格递增，两个名字的哈希大小是任意的）。
5. 想省事就把"收事件"和"刷新"的 handle 声明到同一条队列上，天然串行。
6. `winmsg_open` 之后等 `state == READY` 再改画面。
7. **`SetWindowPos`/`DestroyWindow` 只能由 GUI 线程做**；客户端拿不到 `HWND`。
8. **窗口被回收之后重新登记，订阅也要一起重新声明** —— 用 `winmsg_open_ex`。
9. `winmsg_lock_ex` 返回 `BUSY` 是**正常回退**（资源正被 broker/别人借走），不是错误：
   下轮再来就行。别像"资源没了"那样把自己的自转链掐断。

---

## 八、踩过的坑（都是真发生过的，改代码前请先看这一节）

| # | 现象 | 根因 | 修法 |
| --- | --- | --- | --- |
| 1 | `LOCK_ORDER` + 注入事件全丢 | `winmsg.Control` 嵌套借 `Shelf`+`Screen` | 一个调用只借一个资源 |
| 2 | 窗口销毁失败 | `DestroyWindow` 必须由创建线程调 | `_destroy` 只 signal + join，GUI 线程自己拆 |
| 3 | 卸载时崩 | 框架不管插件自开的线程 | join 放 Object 的 `_destroy` |
| 4 | 偶尔不刷新 | 布尔脏标志会丢 | 单调递增序号 `content_seq`/`win_seq` |
| 5 | 空闲烧 CPU / 输入一顿一顿 | `Sleep(16)` 节流 | `MsgWaitForMultipleObjects` 带超时 |
| 6 | 窗口一直看不见 | 分层窗口创建时没带 `WS_VISIBLE`，`UpdateLayeredWindow` 也不置位 | 第一次贴图后 `ShowWindow(SW_SHOWNOACTIVATE)` |
| 7 | **重建出来的窗口一直看不见** | 上面那个"显示一次"用了**全局**标志，于是只有第一扇窗被显示过 | 标志改成**每扇窗各自一个**（`shown[idx]`）；并且 `slot.visible` 让自测能验"真的可见" |
| 8 | 屏幕探针读不到圆 | 分层窗口是 DWM 合成的，`GetPixel(GetDC(NULL))` 读不到它 | 自测改成**读画布**（`UpdateLayeredWindow` 交上去的那张图） |
| 9 | **真实鼠标点击收不到** | `WM_NCHITTEST` 无条件返回 `HTCAPTION`（整窗当标题栏拖拽），点击被系统吃掉 | 默认 `HTCLIENT`；可拖改成客户端点名的 `WINMSG_F_DRAGGABLE` |
| 10 | 用户拖动之后窗口"弹回去" | 拖动改了系统里的位置，但目录里的 `slot.cur` 没跟上 | `WM_MOVE`/`WM_SIZE` 把系统认的矩形写回 `cur` |
| 11 | 自愈之后 `WM_CLOSE` 收不到 | 窗口被拆时目录格子清零，订阅跟着没了；自愈只 `winmsg_open` 没重新订阅 | 统一走 `winmsg_open_ex`（登记 + 订阅 + 可拖） |
| 12 | 注入的滚轮坐标是错的 | 注入时用了客户区坐标，而 `WM_MOUSEWHEEL` 的 `lParam` 按约定是屏幕坐标 | 注入前 `ClientToScreen` |

---

## 九、自测验什么

### `--guitest`（几秒钟，改 GUI 的时候用）

只跑 GUI 阶段，四扇窗（`paint` + `circ_a/b/c`）都在。**122 条断言**，全过退出码 0：

| 测试 | 怎么验 |
| --- | --- |
| 窗口真的出来了 | 每扇窗 `state == READY` **且** `slot.visible == 1`（后者是 broker 从系统读的 `IsWindowVisible`） |
| 动画 | 同一客户端隔 400ms 采两次：帧号在涨、颜色在变、缩放或偏置在动、broker 贴图次数在涨 |
| 缩放 / 偏置 / 透明 | 自测把客户端**暂停**，自己驱动 surface 到确定的 (颜色, 缩放, 偏置)，等 broker 贴完，再逐点采**画布**像素，和 `test/gui_test_protocol.h` 里的参考光栅器 `gtc_expect()` 比对（避开圆边缘 3 像素内的点，免得考取整规则） |
| 出界 | 偏置设成负数 / 设成把整块图挪出窗口：窗口里剩下的部分必须对，没被图盖到的画布必须 `alpha=0`，整块出界时 `last_dst_w/h == 0` |
| 参数 1 | 写 `slot.win` 挪窗/改尺寸 → 等 `slot.cur`（系统认的）跟到请求值 → 画布跟着变大且新区域透明 |
| 参数 1 端到端 | 注入方向键 → broker 翻译 → `emit` → 客户端改 RECT → 窗口真的挪（`cur.x` 变了） |
| 输入分流 | 注入一批合成输入 → 逐类到达客户端（按键/按下/滚轮/字符/焦点）；鼠标位置走状态快照（`input_seq` 变、不产生消息） |
| 模拟点击关闭 | 客户区中心一次**真点击**（`PostMessage` 的鼠标消息，命中测试/焦点/按钮状态全走真路径）→ 客户端收到 `MOUSE_DOWN` → 它调用 `winmsg_close` → broker 拆窗 → 客户端发现窗口没了**自己再要一个**（自愈），并核对"新建窗口数 +1、自愈计数 +1" |
| WM_CLOSE | 收到 `CLOSE` 事件；**只有客户端请求关窗才真拆**（broker 不替客户端决定） |
| 热插拔 | 三个测试插件逐个"卸载 → 窗口消失 → 装回 → 窗口又**可见** → 重载 N 轮"，每轮核对条目/队列回到基线 |

### `--selftest --cycles N`

先跑热插拔压力测试（4 条捣乱线程 + 真装卸 N 轮，见 README 第八节），最后接上面的 GUI 阶段。
`--cycles 1000` 大约 30 秒，退出码 0/3。

手动看：

```powershell
build\mdpsr.exe --duration 8000      # 屏幕上出现 4 扇透明窗口, 里面各有一个纯色圆在动
Get-Process mdpsr | Select MainWindowTitle   # 4 扇窗的标题都是 "winmsg: paints=N"
```

窗口是透明的：**圆外直接看到桌面**；`circ_b` 那扇开了整窗可拖（拖得动）、`paint` 支持方向键挪窗、
空格暂停刷新、`Alt+F4` 关闭。`circ_a`/`circ_c` 会在收到点击时**自己关窗再自己开回来**。

---

## 十、边界（诚实清单）

| 项 | 现状 |
| --- | --- |
| 画面是"最新值"语义 | 来不及画的那几版被自然合并（`content_seq` 只保证"贴过这一版"，不保证每版都上过屏） |
| 缩放是最近邻 | 放大就是像素块；要平滑/要 3D 请在客户端自己把图画成目标尺寸 |
| 每帧成本 ≈ O(窗口像素) × 脏窗口数 | 没有脏矩形，重绘就是整张画布 `memset` + 组合 + `UpdateLayeredWindow` |
| 一个 GUI 线程 | 所有窗口共用 `winmsg` 那条 GUI 线程；窗口特别多 / 要 GPU 时再写一个新 GUI 插件（协议不变） |
| 目录是一把锁 | `winmsg.Screen` 是 16 个窗口共用的一条 State（控制面串行）；数据面（每窗口的 `Surface`）各自一把锁，互不阻塞 |
| 窗口事件会丢（有计数） | 客户端队列满时丢掉并记 `slot.events_dropped`，不反压 GUI 线程（慢客户端不该卡住界面） |
| 无边框、默认全透明 | 没有标题栏/边框/系统菜单；可拖是客户端点名的；要控件得自己画在画面里 |
| 拖动时收不到鼠标按下/抬起 | 只对 `WINMSG_F_DRAGGABLE` 的窗口成立（见 4.6）；不开可拖就是普通客户区 |
| `WINMSG_CMD_INJECT` / `WINMSG_CMD_PROBE` | 只给自测用（`slot.inject_req` / `probe_*` 也是），不是给普通客户端的功能 |
