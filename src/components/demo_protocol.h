/* ============================================================================
 *  demo_protocol.h —— 演示插件之间的协议 (宿主自测也用它)
 *
 *  这不是框架的一部分, 是"这套骨架该怎么用"的示例协议:
 *  宿主 / 插件之间只靠"条目名 + 命令号 + 一段字节"说话, 谁都不用 include
 *  谁的头文件。把这份头文件删掉, 框架一样跑。
 * ==========================================================================*/
#ifndef MDPSR_DEMO_PROTOCOL_H
#define MDPSR_DEMO_PROTOCOL_H

#include "mdpsr/abi.h"

/* ---- 条目名 ---- */
#define DEMO_SYSMGR_HANDLE  "sysmgr.Handle"
#define DEMO_SYSMGR_STATE   "sysmgr.Status"
#define DEMO_SYSMGR_OBJECT  "sysmgr.Mgr"

#define DEMO_ALPHA_HANDLE   "alpha.Handle"
#define DEMO_ALPHA_STATE    "alpha.Stats"
#define DEMO_ALPHA_OBJECT   "alpha.Keeper"

#define DEMO_BETA_HANDLE    "beta.Handle"
#define DEMO_BETA_STATE     "beta.Counter"

#define DEMO_GAMMA_WORK     "gamma.Handle.Work"
#define DEMO_GAMMA_REPORT   "gamma.Handle.Report"
#define DEMO_GAMMA_STATE    "gamma.Ledger"
#define DEMO_GAMMA_OBJECT   "gamma.Machine"

#define DEMO_ALPHA_QUEUE    "Queue_alpha"
#define DEMO_GAMMA_QUEUE    "Queue_gamma"
#define DEMO_GAMMA_AUX      "Queue_gamma_aux"   /* gamma 运行时自己建的队列 */

/* ---- GUI 演示客户端 paint (它用 winmsg 提供的窗口) ---- */
#define DEMO_PAINT_SURFACE  "paint.Surface"     /* 客户端自己的画面 State */
#define DEMO_PAINT_TICK     "paint.Handle"      /* 自己的刷新 handle */
#define DEMO_PAINT_EVENTS   "paint.Events"      /* 收窗口事件的 handle */
#define DEMO_PAINT_WINDOW   "paint.Window"      /* 窗口名 (哈希用) */
#define DEMO_PAINT_QUEUE    "Queue_paint"       /* 自己的队列; pace_ms 就是动画时钟 */
#define DEMO_PAINT_IMG_W    96u            /* 图比窗口小 -> 窗口里没图的地方直接透出桌面 */
#define DEMO_PAINT_IMG_H    96u

#define PAINT_CMD_TICK      (MDPSR_CMD_USER_BASE + 10)   /* 自己给自己的一拍 */

/* 客户端自己的统计, 存在画面载荷的扩展区里 (winmsg_surface_extra 取到) */
typedef struct paint_stat {
    uint32_t ticks;          /* 刷了多少帧 */
    uint32_t events_rx;      /* 收到多少条窗口事件 */
    uint32_t ev_ready;
    uint32_t ev_close;
    uint32_t ev_destroyed;
    uint32_t ev_key;
    uint32_t ev_keyup;
    uint32_t ev_mouse;
    uint32_t ev_wheel;
    uint32_t ev_char;
    uint32_t ev_focus;
    uint32_t last_mx;        /* 最后一次从状态里读到的鼠标 x */
    uint32_t last_input_seq;
    uint32_t did_repaint;    /* 发布过多少版画面 (含缩放/偏置参数变化) */
    uint32_t moves;          /* 请求过多少次窗口移动 (参数 1) */
    uint32_t img_moves;      /* 改过多少次图像渲染位置 (参数 2) */
    uint32_t reopens;        /* 窗口被回收后又重新要了几次 */
    uint32_t running;
} paint_stat;

/* ---- sysmgr 的命令 ---- */
#define SYS_CMD_INSTALL   (MDPSR_CMD_USER_BASE + 0)
#define SYS_CMD_UNINSTALL (MDPSR_CMD_USER_BASE + 1)
#define SYS_CMD_RELOAD    (MDPSR_CMD_USER_BASE + 2)
#define SYS_CMD_LIST      (MDPSR_CMD_USER_BASE + 3)
#define SYS_CMD_CYCLE     (MDPSR_CMD_USER_BASE + 4)   /* 装-卸循环 (热插拔压力) */
#define SYS_CMD_QUERY     (MDPSR_CMD_USER_BASE + 5)

/* 安装 / 卸载 / 重载 / 循环 的载荷。
 * ★ 注意: 判命令只看 msg->cmd, 这张结构体是"参数在长度够的时候才读"的那个参数。
 *   所以 _cmd 只是一个方便人肉看内存的副本。 */
typedef struct sys_req {
    int32_t  _cmd;
    int32_t  rounds;      /* CYCLE: 循环多少轮 */
    uint64_t plugin;      /* 目标插件键 */
} sys_req;

/* sysmgr 把自己的账本放在 sysmgr.Status 里, 谁都能借来看 */
#define SYS_STATUS_MAGIC 0x53595331u   /* "SYS1" */
typedef struct sys_status {
    uint32_t magic;
    uint32_t status;         /* 1 = 在跑 */
    uint32_t installs;
    uint32_t uninstalls;
    uint32_t reloads;
    uint32_t fails;
    uint32_t cycles_done;
    uint32_t last_error;
    uint64_t last_plugin;
} sys_status;

/* ---- alpha 的命令 ---- */
#define ALPHA_CMD_TICK (MDPSR_CMD_USER_BASE + 0)
#define ALPHA_CMD_STOP (MDPSR_CMD_USER_BASE + 1)
#define ALPHA_CMD_PING (MDPSR_CMD_USER_BASE + 2)

#define ALPHA_STATS_MAGIC 0x414c5031u   /* "ALP1" */
typedef struct alpha_stats {
    uint32_t magic;
    uint32_t ticks;
    uint32_t rounds;         /* 一共跑过几轮自转 */
    uint32_t reloads;
    uint64_t sum;
    uint64_t last_value;
} alpha_stats;

/* ---- beta 的命令 ---- */
#define BETA_CMD_BUMP (MDPSR_CMD_USER_BASE + 0)
#define BETA_CMD_READ (MDPSR_CMD_USER_BASE + 1)

#define BETA_COUNTER_MAGIC 0x42455431u   /* "BET1" */
typedef struct beta_counter {
    uint32_t magic;
    uint32_t bumps;
    uint64_t total;
} beta_counter;

/* ---- gamma 的命令 ---- */
#define GAMMA_CMD_WORK   (MDPSR_CMD_USER_BASE + 0)
#define GAMMA_CMD_REPORT (MDPSR_CMD_USER_BASE + 1)

#define GAMMA_LEDGER_MAGIC 0x47414d31u   /* "GAM1" */
typedef struct gamma_ledger {
    uint32_t magic;
    uint32_t jobs;
    uint64_t total;
    uint64_t max_seen;
} gamma_ledger;

#endif /* MDPSR_DEMO_PROTOCOL_H */
