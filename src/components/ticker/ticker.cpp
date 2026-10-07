/* ============================================================================
 *  ticker.cpp —— 示例插件的实现
 * ==========================================================================*/
#include "ticker.h"

#include <cstdio>
#include <string>

namespace mdpsr {
namespace ticker {

static void tlog(const mdpsr_host* H, int level, const std::string& s) {
    if (H && H->log) H->log(H->host, level, s.c_str());
}

/* 从 state 字典里取公用计数。
 * 注意这里用的是"取指针"的姿势: get 只保证"这一瞬间有效", 但 dispatch
 * 在调用本 handle 期间持有它的锁, 所以期间不会被卸载掉。 */
TickStats* Keeper::stats_of(const mdpsr_context* ctx, const mdpsr_host* H) {
    if (!ctx || !ctx->state || !H) return nullptr;
    mdpsr_ptr* p = H->map_find(ctx->state, mdpsr_hash64("Ticker.Ticks"));
    if (!p) return nullptr;
    if (!H->map_is_valid(H->host, p)) return nullptr;
    auto* st = static_cast<TickStats*>(H->map_ptr_of(H->host, p));
    if (!st || st->magic != TICKER_MAGIC) return nullptr;
    return st;
}

int Keeper::on_init(const mdpsr_context* ctx) {
    TickStats* st = stats_of(ctx, _h);
    if (!st) {
        tlog(_h, 2, "[ticker] cmd=0: 拿不到 State 'Ticker.Ticks'");
        return MDPSR_ERR_BAD_STATE;
    }
    st->reloads++;
    tlog(_h, 0, "[ticker] cmd=0 初始化完成 (第 " + std::to_string(st->reloads) +
                   " 次): state 里记着 ticks=" + std::to_string(st->ticks) +
                   " sum=" + std::to_string(st->sum));

    /* 把工作消息回滚给自己 —— 循环就这么转起来。
     * 限速不在插件里做: 我们声明了 Queue_ticker 的 pace_ms, 由 Queue 保证
     * 两拍之间至少隔那么久, 所以这里【不需要】sleep。 */
    TickCmd m{};
    m._cmd = TICKER_CMD_TICK;
    m.value = 1;
    return _h->emit(_h->host, mdpsr_hash64("Ticker.Handle.Tick"), &m, sizeof(m));
}

int Keeper::on_tick(const mdpsr_context* ctx, uint64_t value) {
    TickStats* st = stats_of(ctx, _h);
    if (!st) return MDPSR_ERR_BAD_STATE;

    ++_local_ticks;
    st->ticks++;
    st->last_value = value;
    st->sum += value;

    /* 每 10 拍报一次, 免得刷屏 */
    if (st->ticks % 10 == 1) {
        tlog(_h, 0, "[ticker] 第 " + std::to_string(st->ticks) + " 拍: last=" +
                       std::to_string(value) + " sum=" + std::to_string(st->sum) +
                       " (本对象内部计 " + std::to_string(_local_ticks) + ")");
    }

    /* 收工条件: 跑够 50 拍就停下, 不再回滚 —— 演示"循环要能自己结束"。
     * 一个停不下来的自转循环是队列限速要防的东西, 但限速只是保险,
     * 组件自己该有终止条件。 */
    if (st->ticks >= 50) {
        tlog(_h, 0, "[ticker] 跑够 50 拍, 循环收工 (sum=" + std::to_string(st->sum) + ")");
        return MDPSR_OK;
    }

    TickCmd m{};
    m._cmd = TICKER_CMD_TICK;
    m.value = value + 1;
    return _h->emit(_h->host, mdpsr_hash64("Ticker.Handle.Tick"), &m, sizeof(m));
}

int Keeper::on_post(const mdpsr_context* ctx, uint64_t value) {
    /* 演示"把消息投到指定队列" —— 这里投给默认队列, 收件人还是自己。
     * 真实场景里这一手是用来把活儿挪到别的线程上的。 */
    TickStats* st = stats_of(ctx, _h);
    if (st) st->sum += value;

    TickCmd m{};
    m._cmd = TICKER_CMD_TICK;
    m.value = value;
    const int r = _h->queue_emit(_h->host, mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME),
                                 mdpsr_hash64("Ticker.Handle.Tick"), &m, sizeof(m));
    tlog(_h, r == MDPSR_OK ? 0 : 2,
         "[ticker] cmd=2 投一条到 " MDPSR_QUEUE_DEFAULT_NAME " (rc=" + std::to_string(r) + ")");
    return r;
}

} /* namespace ticker */
} /* namespace mdpsr */
