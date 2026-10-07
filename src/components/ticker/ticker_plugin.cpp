/* ============================================================================
 *  ticker_plugin.cpp —— 示例插件的导出层
 *
 *  符号名 = mdpsr_<种类>_<清单条目名>:
 *      State  "Ticker.Ticks"       mdpsr_state_Ticker.Ticks
 *      Object "Ticker.Keeper"      mdpsr_object_Ticker.Keeper (+ _destroy)
 *      Handle "Ticker.Handle.Tick" mdpsr_handle_Ticker.Handle.Tick
 *      Queue  "Queue_ticker"       mdpsr_queue_Queue_ticker
 *
 *  条目名里带点号没有任何问题: 它只是一个字符串, 哈希之后就是 uint64。
 * ==========================================================================*/
#include "ticker.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

using namespace mdpsr;
using namespace mdpsr::ticker;

static void tlog(const mdpsr_host* H, int level, const std::string& s) {
    if (H && H->log) H->log(H->host, level, s.c_str());
}

/* ==========================================================================
 *  State: Ticker.Ticks —— 纯数值对象, 从 Pool_state 分配
 * ==========================================================================*/
MDPSR_EXPORT void* mdpsr_state_Ticker_Ticks(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = ctx->pool->allocate(sizeof(TickStats), alignof(TickStats));
    if (!mem) return nullptr;
    auto* st = new (mem) TickStats{};
    st->magic = TICKER_MAGIC;
    tlog(ctx->host, 0, "[ticker] State 'Ticker.Ticks' 已挂载");
    return st;
}

/* ==========================================================================
 *  Object: Ticker.Keeper
 * ==========================================================================*/
MDPSR_EXPORT void* mdpsr_object_Ticker_Keeper(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = ctx->pool->allocate(sizeof(Keeper), alignof(Keeper));
    if (!mem) return nullptr;
    try {
        auto* o = new (mem) Keeper(ctx->host, ctx->pool);
        tlog(ctx->host, 0, "[ticker] Object 'Ticker.Keeper' 已构造");
        return o;
    } catch (...) {
        ctx->pool->deallocate(mem, sizeof(Keeper), alignof(Keeper));
        tlog(ctx->host, 2, "[ticker] Object 构造异常");
        return nullptr;
    }
}

MDPSR_EXPORT void mdpsr_object_Ticker_Keeper_destroy(void* instance) {
    auto* o = static_cast<Keeper*>(instance);
    if (!o) return;
    std::pmr::memory_resource* pool = o->pool();
    o->~Keeper();
    if (pool) pool->deallocate(o, sizeof(Keeper), alignof(Keeper));
}

/* ==========================================================================
 *  Queue: Queue_ticker
 *
 *  队列工厂只回答"这条队列要什么参数"。返回非 OK 宿主就跳过这条队列
 *  (插件的 handle 于是回落到 Queue_default)。
 * ==========================================================================*/
MDPSR_EXPORT int mdpsr_queue_Queue_ticker(const mdpsr_factory_ctx* ctx,
                                          mdpsr_queue_desc* out) {
    if (!ctx || !out) return MDPSR_ERR_NULLPTR;
    out->struct_size = sizeof(mdpsr_queue_desc);
    out->capacity    = 4096;
    /* ★ 限速: 两拍之间至少隔 20ms。这就是"防止有人拿队列当 while 用"的地方:
     *   插件不需要自己 sleep, 队列线程会按这个节奏放行。 */
    out->pace_ms     = 20;
    out->reserved    = 0;
    return MDPSR_OK;
}

/* ==========================================================================
 *  Handle: Ticker.Handle.Tick —— 无状态入口
 * ==========================================================================*/
MDPSR_EXPORT int mdpsr_handle_Ticker_Handle_Tick(const uint8_t* msgData, size_t msgLen,
                                                 const mdpsr_context* ctx) {
    if (!ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    /* ★ 防御写法 (规范第 5 条): 同一条命令可能只有 _cmd、也可能带更多字段
     * (宿主点火用的是 16 字节定长消息)。所以:
     *      · 先只读 4 字节的 _cmd —— 判分只靠它;
     *      · 参数在长度够的时候才读, 不够就用默认值。
     * 反过来写 (先要求整条 TickCmd) 会让组件在收到短消息时直接变砖。 */
    int32_t cmd = MDPSR_CMD_INIT;
    if (!mdpsr_read_pod(msgData, msgLen, &cmd)) {
        tlog(H, 1, "[ticker] 载荷连 _cmd 都没有, 拒绝");
        return MDPSR_ERR_BAD_MESSAGE;
    }
    uint64_t value = 0;
    if (msgLen >= sizeof(TickCmd)) {
        TickCmd full{};
        if (mdpsr_read_pod(msgData, msgLen, &full)) value = full.value;
    }

    mdpsr_ptr* op = ctx->object ? H->map_find(ctx->object, mdpsr_hash64("Ticker.Keeper")) : nullptr;
    if (!op) {
        tlog(H, 2, "[ticker] 找不到 object 'Ticker.Keeper'");
        return MDPSR_ERR_BAD_STATE;
    }
    auto* keeper = static_cast<Keeper*>(H->map_ptr_of(H->host, op));
    if (!keeper) return MDPSR_ERR_BAD_STATE;

    switch (cmd) {
    case TICKER_CMD_INIT: return keeper->on_init(ctx);
    case TICKER_CMD_TICK: return keeper->on_tick(ctx, value ? value : 1);
    case TICKER_CMD_POST: return keeper->on_post(ctx, value);
    case TICKER_CMD_STOP:
        tlog(H, 0, "[ticker] 收到 cmd=3 停, 循环不再继续");
        return MDPSR_OK;
    default:
        tlog(H, 1, "[ticker] 未知命令 " + std::to_string(cmd));
        return MDPSR_ERR_UNKNOWN_CMD;
    }
}

/* ==========================================================================
 *  模块导出
 * ==========================================================================*/
MDPSR_EXPORT uint32_t mdpsr_abi_version(void) { return MDPSR_ABI_VERSION; }

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_host* host, uint64_t plugin) {
    if (!host) return MDPSR_ERR_NULLPTR;
    if (host->abi_version != MDPSR_ABI_VERSION) return MDPSR_ERR_PLUGIN_ABI;
    if (host->struct_size < sizeof(mdpsr_host)) return MDPSR_ERR_PLUGIN_ABI;
    tlog(host, 0, "[ticker] 已挂载 (plugin=" + std::to_string(plugin) + ")");
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(uint64_t plugin) {
    (void)plugin;
}
