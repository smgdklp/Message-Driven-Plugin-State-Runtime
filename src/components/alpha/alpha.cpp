/* ============================================================================
 *  alpha.cpp —— 演示插件: 自转循环 / 队列限速 / 请求-回包
 *
 *  跑的机制:
 *      · State 托管       alpha.Stats 由宿主记生命周期, 卸载时随池一起回收
 *      · 统一对象          alpha.Keeper 干活的对象
 *      · 自己一条线程      清单里声明 Queue_alpha -> handle 不在默认队列上跑
 *      · 队列限速          mdpsr_queue_Queue_alpha 回答 pace_ms = 12
 *      · 消息回滚自转      cmd=0 之后把 cmd=TICK 投给自己, 循环就转起来
 *      · 循环要能停        跑够 MAX_TICKS 自己收工 (限速是保险, 不是许可)
 *      · 请求-回包         cmd=PING 回 PONG 给 msg->src
 *      · 借用协议          用 acquire_many 一次借 State+Object, 用完 release_all
 *
 *  ★ 关于同步初始化: 清单里 Init 指向本 handle, 宿主默认"在装载者线程上同步
 *    跑一次 cmd=0"。所以 cmd=0 里【不要】假设自己在 Queue_alpha 的线程上 ——
 *    需要线程亲和的活请投一条消息给自己, 让它回到自己的队列上做。
 *    这里正好就是这么干的: cmd=0 只记账 + 投出第一条 TICK。
 * ==========================================================================*/
#include "mdpsr/abi.h"
#include "demo_protocol.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#define ALPHA_MAX_TICKS 240   /* 跑够这么多拍就自己收工 */

static void alog(const mdpsr_host* H, int lv, const std::string& s) {
    if (H && H->log) H->log(H->self, lv, s.c_str());
}

/* --------------------------------------------------------------------------
 *  State: alpha.Stats
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT void* mdpsr_state_alpha_Stats(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(alpha_stats), alignof(alpha_stats));
    if (!mem) return nullptr;
    auto* s = new (mem) alpha_stats();
    s->magic = ALPHA_STATS_MAGIC;
    alog(ctx->host, 0, "[alpha] State 'alpha.Stats' 已挂载 (gen " +
                          std::to_string(ctx->plugin_gen) + ")");
    return s;
}

/* --------------------------------------------------------------------------
 *  Object: alpha.Keeper
 * ------------------------------------------------------------------------*/
struct AlphaKeeper {
    const mdpsr_host* host = nullptr;
    mdpsr_pool*       pool = nullptr;
    uint32_t local_ticks = 0;
    uint64_t last_ms = 0;
};

MDPSR_EXPORT void* mdpsr_object_alpha_Keeper(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(AlphaKeeper), alignof(AlphaKeeper));
    if (!mem) return nullptr;
    auto* k = new (mem) AlphaKeeper();
    k->host = ctx->host;
    k->pool = ctx->pool;
    alog(ctx->host, 0, "[alpha] Object 'alpha.Keeper' 已构造");
    return k;
}

MDPSR_EXPORT void mdpsr_object_alpha_Keeper_destroy(void* inst, const mdpsr_factory_ctx* ctx) {
    (void)ctx;
    auto* k = static_cast<AlphaKeeper*>(inst);
    if (!k) return;
    alog(k->host, 0, "[alpha] Object 'alpha.Keeper'._destroy: 本对象内部计过 " +
                        std::to_string(k->local_ticks) + " 拍");
    k->~AlphaKeeper();
}

/* --------------------------------------------------------------------------
 *  队列工厂: Queue_alpha
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_queue_Queue_alpha(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out) {
    if (!ctx || !out) return MDPSR_ERR_NULLPTR;
    out->struct_size = sizeof(mdpsr_queue_desc);
    out->capacity    = 8192;
    /* ★ 限速: 两拍之间至少隔 12ms。
     * 这就是"防止有人拿队列当 while 用"的地方: 插件不需要自己 sleep。
     * 宿主为了这个把系统计时器精度提到了 1ms, 所以 12 就是 12 左右, 不是 15.6。 */
    out->pace_ms     = 12;
    out->flags       = 0;
    return MDPSR_OK;
}

/* --------------------------------------------------------------------------
 *  Handle: alpha.Handle
 * ------------------------------------------------------------------------*/
static int borrow(const mdpsr_host* H, alpha_stats** st, AlphaKeeper** kp) {
    const uint64_t keys[2] = { mdpsr_hash64(DEMO_ALPHA_STATE), mdpsr_hash64(DEMO_ALPHA_OBJECT) };
    void*          ptrs[2] = { nullptr, nullptr };
    uint32_t       gens[2] = { 0, 0 };
    const int rc = mdpsr_acquire_many(H, keys, nullptr, 2, ptrs, gens);
    if (rc != MDPSR_OK) return rc;
    if (!ptrs[0] || !ptrs[1]) { mdpsr_release_all(H); return MDPSR_ERR_BAD_STATE; }
    *st = static_cast<alpha_stats*>(ptrs[0]);
    *kp = static_cast<AlphaKeeper*>(ptrs[1]);
    if ((*st)->magic != ALPHA_STATS_MAGIC) { mdpsr_release_all(H); return MDPSR_ERR_TYPE_MISMATCH; }
    return MDPSR_OK;
}

MDPSR_EXPORT int mdpsr_handle_alpha_Handle(const mdpsr_msg* msg, const uint8_t* body,
                                           uint32_t len, const mdpsr_ctx* ctx) {
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    /* 参数: 长度够才读 (不够就用默认值, 绝不因为短消息变砖) */
    uint32_t value = 1;
    mdpsr_read(body, len, &value, sizeof(value));

    switch (msg->cmd) {

    case MDPSR_CMD_INIT: {
        alpha_stats* st = nullptr; AlphaKeeper* k = nullptr;
        const int rc = borrow(H, &st, &k);
        if (rc != MDPSR_OK) {
            alog(H, 2, "[alpha] cmd=0 借不到自己的 State/Object, rc=" + std::to_string(rc));
            return rc;
        }
        st->reloads++;
        k->local_ticks = 0;
        k->last_ms = mdpsr_millis(H);
        const uint32_t reloads = st->reloads;
        alog(H, 0, "[alpha] cmd=0 初始化 (第 " + std::to_string(reloads) +
                       " 次装上): ticks 归零, 开始自转");
        mdpsr_release_all(H);

        /* 把工作消息投给自己 —— 循环就这么转起来。
         * 限速不在插件里做: 队列的 pace_ms 会保证节奏, 这里【不需要】sleep。 */
        uint32_t v = 1;
        return mdpsr_emit(H, mdpsr_hash64(DEMO_ALPHA_HANDLE), ALPHA_CMD_TICK, &v, sizeof(v));
    }

    case ALPHA_CMD_TICK: {
        alpha_stats* st = nullptr; AlphaKeeper* k = nullptr;
        const int rc = borrow(H, &st, &k);
        if (rc != MDPSR_OK) return rc;

        ++k->local_ticks;
        st->ticks++;
        st->sum += value;
        st->last_value = value;

        /* 顺手量一下限速到底准不准 */
        const uint64_t now = mdpsr_millis(H);
        const uint64_t gap = now - k->last_ms;
        k->last_ms = now;

        if (st->ticks % 20 == 1) {
            alog(H, 0, "[alpha] 第 " + std::to_string(st->ticks) + " 拍: sum=" +
                           std::to_string(static_cast<unsigned long long>(st->sum)) +
                           ", 距上一拍 " + std::to_string(gap) + "ms (声明 12ms)");
        }

        const bool done = st->ticks >= ALPHA_MAX_TICKS;
        if (done) {
            st->rounds++;
            alog(H, 0, "[alpha] 跑够 " + std::to_string(ALPHA_MAX_TICKS) +
                           " 拍, 本轮收工 (总 sum=" +
                           std::to_string(static_cast<unsigned long long>(st->sum)) + ")");
        }
        mdpsr_release_all(H);

        if (done) return MDPSR_OK;
        uint32_t v = value + 1;
        return mdpsr_emit(H, mdpsr_hash64(DEMO_ALPHA_HANDLE), ALPHA_CMD_TICK, &v, sizeof(v));
    }

    /* 框架保留的 PING / STOP 也认 —— 别的插件按标准协议问它, 它就该按标准答。
     * 自己的 ALPHA_CMD_PING 只是一个别名, 一并接受。 */
    case MDPSR_CMD_PING:
    case ALPHA_CMD_PING: {
        mdpsr_reply rep{};
        rep.cmd   = msg->cmd;
        rep.code  = MDPSR_OK;
        rep.src   = ctx->handle;
        rep.seq   = value;
        rep.value = 0;
        return mdpsr_send_reply(H, msg, MDPSR_CMD_PONG, &rep, sizeof(rep));
    }

    case MDPSR_CMD_FAIL: {
        /* 框架的回执: "你有条消息没送到" (通常是目标正在被卸载)。
         * 它不是错误, 更不该回 UNKNOWN_CMD —— 插件对保留命令要有礼貌。 */
        mdpsr_fail f{};
        if (body && len >= sizeof(f)) std::memcpy(&f, body, sizeof(f));
        alog(H, 3, "[alpha] 收到 FAIL 回执: code=" + std::to_string(f.code) +
                       " reason=" + std::to_string(f.reason));
        return MDPSR_OK;
    }

    case MDPSR_CMD_STOP:
    case ALPHA_CMD_STOP: {
        alog(H, 0, "[alpha] 收到 STOP, 本轮循环不再继续");
        return MDPSR_OK;
    }

    default:
        return MDPSR_ERR_UNKNOWN_CMD;
    }
}

/* --------------------------------------------------------------------------
 *  模块导出
 * ------------------------------------------------------------------------*/
MDPSR_DECL_ABI_VERSION()

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    std::string list;
    for (uint32_t i = 0; i < ctx->list_count; ++i) {
        if (i) list += ",";
        list += ctx->list[i] ? ctx->list[i] : "";
    }
    alog(ctx->host, 0, "[alpha] 已挂载 (gen " + std::to_string(ctx->plugin_gen) +
                          ", 清单 list=[" + list + "])");
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(void) {
}
