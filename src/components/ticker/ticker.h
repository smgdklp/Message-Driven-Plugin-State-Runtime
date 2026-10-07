#pragma once
/* ============================================================================
 *  Ticker —— 一个"最小可用"的示例插件
 *
 *  它存在的意义是**把骨架的每条机制都走一遍**, 而且不带任何图形/文件依赖:
 *
 *      State  "Ticker.Ticks"  公用计数 (纯数值对象) —— 演示 State 托管
 *      Object "Ticker.Keeper" 干活的对象     —— 演示统一 Pool_object
 *      Handle "Ticker.Handle.Tick"
 *                             无状态入口 + cmd=1 自转循环 —— 演示消息驱动与限速
 *      Queue  "Queue_ticker"  自己的一条分流队列 —— 演示"组件申请专属线程"
 *
 *  规范的要点在它身上都能看到:
 *      · handle 无状态: 所有跨消息的东西都在 State 里
 *      · 资源从 ctx->pool 分配, 卸载时由宿主统一回收
 *      · 要循环就"把消息回滚给自己", 不 sleep、不 while(1)
 * ==========================================================================*/

#include "mdpsr/abi.h"

namespace mdpsr {
namespace ticker {

/* ==========================================================================
 *  State: Ticker.Ticks —— 纯数值, 不需要析构
 * ==========================================================================*/
#define TICKER_MAGIC 0x5449434Bu   /* 'TICK' */

struct TickStats {
    uint32_t magic;
    uint32_t ticks;        /* 一共跑了几拍 */
    uint64_t last_value;   /* 最近一次从队列里收到的值 */
    uint64_t sum;          /* 收到的所有值之和 */
    uint32_t reloads;      /* 被要求重载自己的次数 */
};

/* ==========================================================================
 *  Handle 的载荷
 *
 *      cmd = 0  初始化: 报一次到, 然后把 cmd=1 回滚给自己, 循环就转起来
 *      cmd = 1  一拍: 累加, 然后把 cmd=1 再回滚给自己
 *      cmd = 2  投一条消息给默认队列 (演示跨队列/跨组件投递)
 *      cmd = 3  停: 不再回滚, 循环自然停下
 * ==========================================================================*/
struct TickCmd {
    int32_t  _cmd;
    uint32_t _pad;
    uint64_t value;
};

#define TICKER_CMD_INIT  0
#define TICKER_CMD_TICK  1
#define TICKER_CMD_POST  2
#define TICKER_CMD_STOP  3

/* ==========================================================================
 *  Object: Ticker.Keeper
 * ==========================================================================*/
class Keeper {
public:
    Keeper(const mdpsr_host* host, mdpsr_resource* pool) : _h(host), _pool(pool) {}
    ~Keeper() = default;

    Keeper(const Keeper&) = delete;
    Keeper& operator=(const Keeper&) = delete;

    /* 拿到自己的 State (从 state 字典按名字取, 拿不到就返回 nullptr)
     * —— 这就是"跨组件/跨插件取公用数据"的标准姿势。 */
    static TickStats* stats_of(const mdpsr_context* ctx, const mdpsr_host* H);

    int on_init(const mdpsr_context* ctx);
    int on_tick(const mdpsr_context* ctx, uint64_t value);
    int on_post(const mdpsr_context* ctx, uint64_t value);

    uint32_t local_ticks() const { return _local_ticks; }
    mdpsr_resource* pool() const { return _pool; }

private:
    const mdpsr_host* _h = nullptr;
    mdpsr_resource*   _pool = nullptr;
    uint32_t _local_ticks = 0;   /* 只在本对象内部用的计数器 (不跨插件) */
};

} /* namespace ticker */
} /* namespace mdpsr */
