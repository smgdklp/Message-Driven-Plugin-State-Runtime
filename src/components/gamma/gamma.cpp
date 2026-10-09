/* ============================================================================
 *  gamma.cpp —— 演示插件: 两个 handle 共用一条队列 / 运行时自己建队列
 *
 *  跑的机制:
 *      · 一个插件可以声明多个 Handle, 它们默认都跑在自己声明的那条队列上
 *      · 插件在运行时可以用 host->queue_create 再建队列 (会被登记在自己名下,
 *        卸载时一起收掉 —— 见 selftest 里"队列数回到基线"那一项)
 *      · State + Object 都在卸载时被析构: 日志里能看到 _destroy 被调用,
 *        而且它们的**内存**随本插件的池一起销毁 (不是留在全局池里不管)
 *      · 防御写法: 全部参数都是"长度够才读", 收到 4 字节的短消息也不会变砖
 * ==========================================================================*/
#include "mdpsr/abi.h"
#include "demo_protocol.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

static void glog(const mdpsr_host* H, int lv, const std::string& s) {
    if (H && H->log) H->log(H->self, lv, s.c_str());
}

/* --------------------------------------------------------------------------
 *  State: gamma.Ledger
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT void* mdpsr_state_gamma_Ledger(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(gamma_ledger), alignof(gamma_ledger));
    if (!mem) return nullptr;
    auto* l = new (mem) gamma_ledger();
    l->magic = GAMMA_LEDGER_MAGIC;
    glog(ctx->host, 0, "[gamma] State 'gamma.Ledger' 已挂载 (gen " +
                           std::to_string(ctx->plugin_gen) + ")");
    return l;
}

/* --------------------------------------------------------------------------
 *  Object: gamma.Machine
 * ------------------------------------------------------------------------*/
struct GammaMachine {
    const mdpsr_host* host = nullptr;
    mdpsr_pool*       pool = nullptr;
    uint32_t local_jobs = 0;
};

MDPSR_EXPORT void* mdpsr_object_gamma_Machine(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(GammaMachine), alignof(GammaMachine));
    if (!mem) return nullptr;
    auto* m = new (mem) GammaMachine();
    m->host = ctx->host;
    m->pool = ctx->pool;
    glog(ctx->host, 0, "[gamma] Object 'gamma.Machine' 已构造");
    return m;
}

MDPSR_EXPORT void mdpsr_object_gamma_Machine_destroy(void* inst, const mdpsr_factory_ctx* ctx) {
    (void)ctx;
    auto* m = static_cast<GammaMachine*>(inst);
    if (!m) return;
    glog(m->host, 0, "[gamma] Object 'gamma.Machine'._destroy: 本对象内部干过 " +
                        std::to_string(m->local_jobs) + " 个活");
    m->~GammaMachine();
}

/* --------------------------------------------------------------------------
 *  队列工厂: Queue_gamma (清单里声明的)
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_queue_Queue_gamma(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out) {
    if (!ctx || !out) return MDPSR_ERR_NULLPTR;
    out->struct_size = sizeof(mdpsr_queue_desc);
    out->capacity    = 4096;
    out->pace_ms     = 0;      /* 干活队列不节流 */
    out->flags       = 0;
    return MDPSR_OK;
}

/* --------------------------------------------------------------------------
 *  借用
 * ------------------------------------------------------------------------*/
static int borrow(const mdpsr_host* H, gamma_ledger** lg, GammaMachine** mc) {
    const uint64_t keys[2] = { mdpsr_hash64(DEMO_GAMMA_STATE), mdpsr_hash64(DEMO_GAMMA_OBJECT) };
    void*          ptrs[2] = { nullptr, nullptr };
    uint32_t       gens[2] = { 0, 0 };
    const int rc = mdpsr_acquire_many(H, keys, nullptr, 2, ptrs, gens);
    if (rc != MDPSR_OK) return rc;
    if (!ptrs[0] || !ptrs[1]) { mdpsr_release_all(H); return MDPSR_ERR_BAD_STATE; }
    *lg = static_cast<gamma_ledger*>(ptrs[0]);
    *mc = static_cast<GammaMachine*>(ptrs[1]);
    if ((*lg)->magic != GAMMA_LEDGER_MAGIC) { mdpsr_release_all(H); return MDPSR_ERR_TYPE_MISMATCH; }
    return MDPSR_OK;
}

/* 框架回执: 保留命令一律礼貌处理, 绝不回 UNKNOWN_CMD */
static int handle_framework(const mdpsr_host* H, const mdpsr_msg* msg, const uint8_t* body,
                            uint32_t len, int* handled) {
    *handled = 1;
    switch (msg->cmd) {
    case MDPSR_CMD_FAIL: {
        mdpsr_fail f{};
        if (body && len >= sizeof(f)) std::memcpy(&f, body, sizeof(f));
        glog(H, 3, "[gamma] 收到 FAIL 回执: code=" + std::to_string(f.code));
        return MDPSR_OK;
    }
    case MDPSR_CMD_PING: {
        mdpsr_reply rep{};
        rep.cmd  = MDPSR_CMD_PING;
        rep.src  = msg->dst;
        return mdpsr_send_reply(H, msg, MDPSR_CMD_PONG, &rep, sizeof(rep));
    }
    default:
        *handled = 0;
        return MDPSR_OK;
    }
}

/* --------------------------------------------------------------------------
 *  Handle: gamma.Handle.Work  (Init 指向它)
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_handle_gamma_Handle_Work(const mdpsr_msg* msg, const uint8_t* body,
                                                uint32_t len, const mdpsr_ctx* ctx) {
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    int handled = 0;
    const int fr = handle_framework(H, msg, body, len, &handled);
    if (handled) return fr;

    uint32_t value = 1;
    mdpsr_read(body, len, &value, sizeof(value));

    switch (msg->cmd) {

    case MDPSR_CMD_INIT: {
        glog(H, 0, "[gamma] cmd=0 初始化");

        /* 运行时再建一条队列 —— 它会被登记到本插件名下, 卸载时一起收掉 */
        mdpsr_queue_desc d{};
        d.struct_size = sizeof(d);
        d.capacity    = 2048;
        d.pace_ms     = 5;
        uint64_t qk = 0;
        const int rc = H->queue_create(H->self, DEMO_GAMMA_AUX, &d, &qk);
        glog(H, rc == MDPSR_OK ? 0 : 1,
             std::string("[gamma] 运行时建队列 '") + DEMO_GAMMA_AUX + "' rc=" +
                 std::to_string(rc) + " key=" + std::to_string(static_cast<unsigned long long>(qk)));

        /* 顺便演示"同一条命令可以只有 4 字节, 也可以带更多字段" —— 再给自己投一条
         * 带字符串标签的 WORK。标签只在 --debug 里打出来, 免得刷屏。 */
        {
            uint8_t buf[32] = { 0 };
            const uint32_t v = 1;
            std::memcpy(buf, &v, 4);
            const char tag[] = "boot";
            std::memcpy(buf + 4, tag, sizeof(tag));
            mdpsr_emit(H, msg->dst, GAMMA_CMD_WORK, buf, 4 + (uint32_t)sizeof(tag));
        }

        /* 顺便看一眼自己的账本 */
        gamma_ledger* lg = nullptr; GammaMachine* mc = nullptr;
        if (borrow(H, &lg, &mc) == MDPSR_OK) {
            glog(H, 0, "[gamma] 账本: jobs=" + std::to_string(lg->jobs) +
                           " total=" + std::to_string(static_cast<unsigned long long>(lg->total)));
            mdpsr_release_all(H);
        }
        return MDPSR_OK;
    }

    case GAMMA_CMD_WORK: {
        gamma_ledger* lg = nullptr; GammaMachine* mc = nullptr;
        const int rc = borrow(H, &lg, &mc);
        if (rc != MDPSR_OK) return rc;
        lg->jobs++;
        lg->total += value;
        if (value > lg->max_seen) lg->max_seen = value;
        mc->local_jobs++;
        mdpsr_release_all(H);
        /* 长度够的话, value 后面还能跟一个 NUL 结尾的标签。
         * 顺便演示"同一条命令可以只有 4 字节, 也可以带更多字段"。 */
        const char* label = mdpsr_read_str(body, len, 4);
        if (label) glog(H, 3, std::string("[gamma] 这条活带标签: '") + label + "'");
        return MDPSR_OK;
    }

    case GAMMA_CMD_REPORT: {
        gamma_ledger* lg = nullptr; GammaMachine* mc = nullptr;
        const int rc = borrow(H, &lg, &mc);
        if (rc != MDPSR_OK) return rc;
        gamma_ledger snap = *lg;
        mdpsr_release_all(H);

        glog(H, 0, "[gamma] 报表: jobs=" + std::to_string(snap.jobs) +
                       " total=" + std::to_string(static_cast<unsigned long long>(snap.total)) +
                       " max=" + std::to_string(static_cast<unsigned long long>(snap.max_seen)));
        if (msg->src) return mdpsr_send_reply(H, msg, MDPSR_CMD_REPLY, &snap, sizeof(snap));
        return MDPSR_OK;
    }

    default:
        return MDPSR_ERR_UNKNOWN_CMD;
    }
}

/* --------------------------------------------------------------------------
 *  Handle: gamma.Handle.Report
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_handle_gamma_Handle_Report(const mdpsr_msg* msg, const uint8_t* body,
                                                  uint32_t len, const mdpsr_ctx* ctx) {
    (void)len; (void)body;
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    int handled = 0;
    const int fr = handle_framework(H, msg, body, len, &handled);
    if (handled) return fr;

    if (msg->cmd == MDPSR_CMD_INIT) {
        glog(H, 0, "[gamma] 报表 handle 就绪 (它和 Work 共用 " DEMO_GAMMA_QUEUE " 这条队列)");
        return MDPSR_OK;
    }
    if (msg->cmd != GAMMA_CMD_REPORT) return MDPSR_ERR_UNKNOWN_CMD;

    gamma_ledger* lg = nullptr; GammaMachine* mc = nullptr;
    const int rc = borrow(H, &lg, &mc);
    if (rc != MDPSR_OK) return rc;
    gamma_ledger snap = *lg;
    mdpsr_release_all(H);

    if (msg->src) return mdpsr_send_reply(H, msg, MDPSR_CMD_REPLY, &snap, sizeof(snap));
    return MDPSR_OK;
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
    glog(ctx->host, 0, "[gamma] 已挂载 (gen " + std::to_string(ctx->plugin_gen) +
                           ", list=[" + list + "])");
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(void) {
}
