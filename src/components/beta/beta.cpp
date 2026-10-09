/* ============================================================================
 *  beta.cpp —— 演示插件: 没有自己的队列 + 跨插件请求/回包
 *
 *  它故意【不】声明 Queue:
 *      -> handle 落在 Queue_default 上 (宿主给的默认队列), 跑在默认线程里
 *
 *  cmd=0 的时候它会给 alpha 发一条 PING, 并把 src 写成自己:
 *      -> alpha 处理完回一条 PONG, 落在 beta.Handle 上, 由 beta 处理
 *  这就是"插件之间只靠消息说话"的样子: beta 完全不需要知道 alpha 的 dll、
 *  头文件或者函数指针, 只知道一个名字 "alpha.Handle"。
 * ==========================================================================*/
#include "mdpsr/abi.h"
#include "demo_protocol.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

static void blog(const mdpsr_host* H, int lv, const std::string& s) {
    if (H && H->log) H->log(H->self, lv, s.c_str());
}

/* --------------------------------------------------------------------------
 *  State: beta.Counter
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT void* mdpsr_state_beta_Counter(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(beta_counter), alignof(beta_counter));
    if (!mem) return nullptr;
    auto* c = new (mem) beta_counter();
    c->magic = BETA_COUNTER_MAGIC;
    blog(ctx->host, 0, "[beta] State 'beta.Counter' 已挂载 (gen " +
                           std::to_string(ctx->plugin_gen) + ")");
    return c;
}

/* --------------------------------------------------------------------------
 *  Handle: beta.Handle
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_handle_beta_Handle(const mdpsr_msg* msg, const uint8_t* body,
                                          uint32_t len, const mdpsr_ctx* ctx) {
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    uint32_t value = 1;
    mdpsr_read(body, len, &value, sizeof(value));

    switch (msg->cmd) {

    case MDPSR_CMD_INIT: {
        /* ★ 先问一句"对方在不在" —— gen_of 是无锁快照, 比"试着投一条看看"便宜得多。
         * 这就是 gen_of 存在的意义: 想确认某个服务在不在, 不需要去借它。 */
        const uint32_t agen = mdpsr_gen_of(H, mdpsr_hash64(DEMO_ALPHA_HANDLE));
        if (agen == 0) {
            blog(H, 0, "[beta] alpha 现在不在线, 这次就不打扰它了");
            return MDPSR_OK;
        }

        blog(H, 0, "[beta] cmd=0 初始化: alpha 在线 (gen " + std::to_string(agen) +
                       "), 给它发一条 PING 等它回 PONG");

        /* 跨插件请求: 只知道名字, 不需要 include 任何东西。
         * ★ src 显式写自己 —— 回包才知道该回给谁。 */
        uint32_t seq = 7;
        const int rc = mdpsr_emit_from(H, mdpsr_hash64(DEMO_ALPHA_HANDLE),
                                       mdpsr_hash64(DEMO_BETA_HANDLE),
                                       MDPSR_CMD_PING, &seq, sizeof(seq));
        /* 队列满不是错, 是"对方现在忙不过来" —— 记 INFO 就够了, 别当告警刷屏。 */
        const int lv = (rc == MDPSR_OK || rc == MDPSR_ERR_QUEUE_FULL) ? 0 : 1;
        blog(H, lv, "[beta] PING -> alpha.Handle rc=" + std::to_string(rc) +
                        (rc == MDPSR_ERR_QUEUE_FULL ? " (对方队列满, 等它闲下来再问)" : ""));
        return MDPSR_OK;
    }

    case MDPSR_CMD_PONG: {
        mdpsr_reply rep{};
        if (body && len >= sizeof(rep)) std::memcpy(&rep, body, sizeof(rep));
        blog(H, 0, "[beta] 收到 alpha 的 PONG (seq=" + std::to_string(rep.seq) + ")");
        return MDPSR_OK;
    }

    case MDPSR_CMD_FAIL: {
        mdpsr_fail f{};
        if (body && len >= sizeof(f)) std::memcpy(&f, body, sizeof(f));
        blog(H, 1, "[beta] 收到一条 FAIL: code=" + std::to_string(f.code) +
                       " reason=" + std::to_string(f.reason) +
                       " (说明我发给了一个已经不在的 handle)");
        return MDPSR_OK;
    }

    case BETA_CMD_BUMP: {
        void* p = nullptr; uint32_t g = 0;
        const int rc = mdpsr_acquire(H, mdpsr_hash64(DEMO_BETA_STATE), 0, &p, &g);
        if (rc != MDPSR_OK) return rc;
        auto* c = static_cast<beta_counter*>(p);
        if (c->magic == BETA_COUNTER_MAGIC) {
            c->bumps++;
            c->total += value;
        }
        mdpsr_release(H, mdpsr_hash64(DEMO_BETA_STATE));
        return MDPSR_OK;
    }

    case BETA_CMD_READ: {
        void* p = nullptr; uint32_t g = 0;
        const int rc = mdpsr_acquire(H, mdpsr_hash64(DEMO_BETA_STATE), 0, &p, &g);
        if (rc != MDPSR_OK) return rc;
        beta_counter snap{};
        std::memcpy(&snap, p, sizeof(snap));
        mdpsr_release(H, mdpsr_hash64(DEMO_BETA_STATE));

        blog(H, 0, "[beta] 被问了: bumps=" + std::to_string(snap.bumps) +
                       " total=" + std::to_string(static_cast<unsigned long long>(snap.total)));
        if (msg->src) return mdpsr_send_reply(H, msg, MDPSR_CMD_REPLY, &snap, sizeof(snap));
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
    blog(ctx->host, 0, "[beta] 已挂载 (gen " + std::to_string(ctx->plugin_gen) +
                           ", 没声明 Queue -> 走 " MDPSR_QUEUE_DEFAULT_NAME ")");
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(void) {
}
