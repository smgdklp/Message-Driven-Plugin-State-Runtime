/* ============================================================================
 *  sysmgr.cpp —— 内核组件
 *
 *  它干的事就是"装卸的策略": 谁该被装、装在哪、失败了怎么办。
 *  "机制" (LoadLibrary / 摘表 / 回收池 / FreeLibrary) 在宿主里, 由
 *  host->plugin_install / plugin_uninstall / plugin_reload 提供。
 *
 *  三个条目各司其职:
 *      State  sysmgr.Status   公开账本, 谁都借得来看 ("现在装了些什么")
 *      Object sysmgr.Mgr      私有干活对象, 只由 sysmgr.Handle 使用
 *      Handle sysmgr.Handle   无状态入口, 跑在 Queue_sys 上
 *
 *  ★ 一条必须遵守的纪律:
 *      "调用 plugin_install/uninstall/reload 的时候, 手上不要持有任何资源"。
 *      原因见 doc/架构.md: 借资源要求按键升序, 而装卸会去动别人的键,
 *      嵌套着借迟早违反顺序。所以这里的写法是"借一下 -> 记一笔 -> 还掉 ->
 *      干活 -> 再借一下 -> 记一笔 -> 还掉"。
 * ==========================================================================*/
#include "mdpsr/abi.h"
#include "demo_protocol.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

/* --------------------------------------------------------------------------
 *  小工具
 * ------------------------------------------------------------------------*/
static void slog(const mdpsr_host* H, int lv, const std::string& s) {
    if (H && H->log) H->log(H->self, lv, s.c_str());
}
static std::string s_of(uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

/* --------------------------------------------------------------------------
 *  State: sysmgr.Status (纯 POD, 从本插件的池分配)
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT void* mdpsr_state_sysmgr_Status(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(sys_status), alignof(sys_status));
    if (!mem) return nullptr;
    auto* s = new (mem) sys_status();
    s->magic  = SYS_STATUS_MAGIC;
    s->status = 1;
    slog(ctx->host, 0, "[sysmgr] State 'sysmgr.Status' 已挂载 (清单 list_count=" +
                           std::to_string(ctx->list_count) + ")");
    return s;
}

/* 这个析构除了打日志什么也不做 —— 它的存在是为了证明"卸载路径真的会调
 * 插件的 _destroy", 而不是像 v1 那样声明了却从来不调。 */
MDPSR_EXPORT void mdpsr_state_sysmgr_Status_destroy(void* inst, const mdpsr_factory_ctx* ctx) {
    (void)inst;
    slog(ctx ? ctx->host : nullptr, 0,
         "[sysmgr] State 'sysmgr.Status'._destroy 被调用 (卸载路径在工作)");
}

/* --------------------------------------------------------------------------
 *  Object: sysmgr.Mgr
 * ------------------------------------------------------------------------*/
struct SysmgrMgr {
    const mdpsr_host* host = nullptr;
    mdpsr_pool*       pool = nullptr;
    uint64_t          plugin = 0;

    uint32_t installs = 0;
    uint32_t uninstalls = 0;
    uint32_t reloads = 0;
    uint32_t fails = 0;
    uint32_t cycles = 0;
    uint32_t last_error = 0;
    uint64_t last_plugin = 0;

    void log(const std::string& s, int lv = 0) const { slog(host, lv, "[sysmgr] " + s); }
};

/* 借 Mgr + Status 两个条目。
 * 用 acquire_many: 宿主内部会按键升序借, 全有或全无 —— 比自己按顺序借安全。 */
static int borrow_all(const mdpsr_host* H, SysmgrMgr** mgr, sys_status** st) {
    const uint64_t keys[2] = { mdpsr_hash64(DEMO_SYSMGR_OBJECT), mdpsr_hash64(DEMO_SYSMGR_STATE) };
    void*          ptrs[2] = { nullptr, nullptr };
    uint32_t       gens[2] = { 0, 0 };
    const int rc = mdpsr_acquire_many(H, keys, nullptr, 2, ptrs, gens);
    if (rc != MDPSR_OK) return rc;
    if (!ptrs[0] || !ptrs[1]) { mdpsr_release_all(H); return MDPSR_ERR_BAD_STATE; }
    *mgr = static_cast<SysmgrMgr*>(ptrs[0]);
    *st  = static_cast<sys_status*>(ptrs[1]);
    return MDPSR_OK;
}
static void return_all(const mdpsr_host* H) { mdpsr_release_all(H); }

static void flush_status(SysmgrMgr* mgr, sys_status* st) {
    if (!mgr || !st) return;
    if (st->magic != SYS_STATUS_MAGIC) return;
    st->installs    = mgr->installs;
    st->uninstalls  = mgr->uninstalls;
    st->reloads     = mgr->reloads;
    st->fails       = mgr->fails;
    st->cycles_done = mgr->cycles;
    st->last_error  = mgr->last_error;
    st->last_plugin = mgr->last_plugin;
}

MDPSR_EXPORT void* mdpsr_object_sysmgr_Mgr(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = mdpsr_fctx_alloc(ctx, sizeof(SysmgrMgr), alignof(SysmgrMgr));
    if (!mem) return nullptr;
    auto* m = new (mem) SysmgrMgr();
    m->host = ctx->host;
    m->pool = ctx->pool;
    m->plugin = ctx->plugin;
    m->log("Object 'sysmgr.Mgr' 已构造 (gen " + std::to_string(ctx->plugin_gen) + ")");
    return m;
}

MDPSR_EXPORT void mdpsr_object_sysmgr_Mgr_destroy(void* inst, const mdpsr_factory_ctx* ctx) {
    (void)ctx;
    auto* m = static_cast<SysmgrMgr*>(inst);
    if (!m) return;
    m->log("Object 'sysmgr.Mgr'._destroy: 一共装了 " + std::to_string(m->installs) +
           " / 卸了 " + std::to_string(m->uninstalls) + " / 重载 " + std::to_string(m->reloads) +
           " / 失败 " + std::to_string(m->fails));
    m->~SysmgrMgr();
}

/* --------------------------------------------------------------------------
 *  管理面调用的"重试"包装
 *
 *  ★ 为什么需要它: 宿主对"插件代码发起的装卸"用**不阻塞**的策略 —— 拿不到管理面
 *    就返回 BUSY (11011), 而不是在那里等。理由见 doc/架构.md: 卸载路径持着管理面
 *    的锁去 join 队列线程, 插件代码要是也阻塞在那把锁上, 就是一个死环。
 *    于是"下轮再来"成了调用方的责任 —— 也就是这里 (sysmgr 就是那个"策略")。
 * ------------------------------------------------------------------------*/
static int call_mgmt(const mdpsr_host* H, const char* what, int32_t cmd, uint64_t plugin,
                     const char* manifest) {
    int rc = MDPSR_OK;
    for (int i = 0; i < 200; ++i) {
        if (cmd == SYS_CMD_INSTALL)        rc = H->plugin_install(H->self, manifest, nullptr, nullptr);
        else if (cmd == SYS_CMD_UNINSTALL) rc = H->plugin_uninstall(H->self, plugin, 0);
        else                               rc = H->plugin_reload(H->self, plugin, nullptr);
        if (rc != MDPSR_ERR_BUSY) return rc;
        if (H->sleep_ms) H->sleep_ms(H->self, 1);
    }
    slog(H, 1, std::string("[sysmgr] ") + what + " 因为管理面一直忙而放弃 (rc=BUSY)");
    return rc;
}

/* --------------------------------------------------------------------------
 *  已经知道的演示插件 (装载的"策略"就放在这里)
 * ------------------------------------------------------------------------*/
struct KnownPlugin { const char* name; const char* manifest; };
static const KnownPlugin kKnown[] = {
    { "sysmgr", "plugins/sysmgr/plugin.json" },
    { "alpha",  "plugins/alpha/plugin.json"  },
    { "beta",   "plugins/beta/plugin.json"   },
    { "gamma",  "plugins/gamma/plugin.json"  },
    /* GUI 插件与 GUI 客户端: 内核组件也认识它们, 这样 CYCLE 能直接拿它们做热插拔
     * (与 test/gui_selftest.cpp 里那几轮"卸载-装回-重载"是同一件事, 只是入口不同) */
    { "winmsg", "plugins/winmsg/plugin.json" },
    { "paint",  "plugins/paint/plugin.json"  },
    { "circ_a", "plugins/circ_a/plugin.json" },
    { "circ_b", "plugins/circ_b/plugin.json" },
    { "circ_c", "plugins/circ_c/plugin.json" },
};
static const char* manifest_of(uint64_t plugin_key) {
    for (const KnownPlugin& k : kKnown) {
        if (mdpsr_plugin_key(k.name) == plugin_key) return k.manifest;
    }
    return nullptr;
}
static const char* name_of(uint64_t plugin_key) {
    for (const KnownPlugin& k : kKnown) {
        if (mdpsr_plugin_key(k.name) == plugin_key) return k.name;
    }
    return "?";
}

/* 从载荷里取请求 (长度不够就当作只有 cmd) */
static void read_req(const uint8_t* body, uint32_t len, sys_req* out) {
    std::memset(out, 0, sizeof(*out));
    if (body && len >= sizeof(*out)) std::memcpy(out, body, sizeof(*out));
    else if (body && len >= 8) std::memcpy(out, body, 8);
}

/* --------------------------------------------------------------------------
 *  Handle: sysmgr.Handle
 * ------------------------------------------------------------------------*/
MDPSR_EXPORT int mdpsr_handle_sysmgr_Handle(const mdpsr_msg* msg, const uint8_t* body,
                                            uint32_t len, const mdpsr_ctx* ctx) {
    if (!msg || !ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    /* ★ 命令只读 msg->cmd。body 里的东西一律"长度够才读"。 */
    switch (msg->cmd) {

    case MDPSR_CMD_INIT: {
        SysmgrMgr* m = nullptr; sys_status* st = nullptr;
        const int rc = borrow_all(H, &m, &st);
        if (rc != MDPSR_OK) return rc;
        flush_status(m, st);
        m->log("cmd=0 初始化完成: 账本就绪 (" + std::to_string(st->installs) +
               " 装 / " + std::to_string(st->uninstalls) + " 卸)");
        return_all(H);
        return MDPSR_OK;
    }

    case MDPSR_CMD_PONG: {
        /* 别人回了我们一条 PONG; 记一笔就够了。演示"回包落在发起者队列上"。 */
        SysmgrMgr* m = nullptr; sys_status* st = nullptr;
        if (borrow_all(H, &m, &st) == MDPSR_OK) {
            flush_status(m, st);
            return_all(H);
        }
        return MDPSR_OK;
    }

    case MDPSR_CMD_FAIL: {
        /* 死信回执。日志压到 DBG, 免得大量热插拔时刷屏。 */
        mdpsr_fail f{};
        if (body && len >= sizeof(f)) std::memcpy(&f, body, sizeof(f));
        slog(H, 3, "[sysmgr] 收到 FAIL: code=" + std::to_string(f.code) +
                       " reason=" + std::to_string(f.reason));
        return MDPSR_OK;
    }

    case SYS_CMD_INSTALL:
    case SYS_CMD_UNINSTALL:
    case SYS_CMD_RELOAD: {
        sys_req r{};
        read_req(body, len, &r);
        const char* manifest = manifest_of(r.plugin);
        if (!manifest && msg->cmd != SYS_CMD_UNINSTALL) return MDPSR_ERR_PLUGIN_NOT_FOUND;

        /* 干活的这一段: 手上一个资源都不许拿; BUSY 由 call_mgmt 自己重试 */
        const int rc = call_mgmt(H, "装卸", msg->cmd, r.plugin, manifest);

        /* 记一笔 */
        SysmgrMgr* m = nullptr; sys_status* st = nullptr;
        if (borrow_all(H, &m, &st) == MDPSR_OK) {
            m->last_plugin = r.plugin;
            if (rc == MDPSR_OK) {
                if (msg->cmd == SYS_CMD_INSTALL)        ++m->installs;
                else if (msg->cmd == SYS_CMD_UNINSTALL) ++m->uninstalls;
                else                                    ++m->reloads;
            } else {
                ++m->fails;
                m->last_error = static_cast<uint32_t>(rc);
            }
            flush_status(m, st);
            const char* what = msg->cmd == SYS_CMD_INSTALL ? "装载"
                             : msg->cmd == SYS_CMD_UNINSTALL ? "卸载" : "重载";
            m->log(std::string(what) + " '" + name_of(r.plugin) + "' -> rc=" + std::to_string(rc),
                   rc == MDPSR_OK ? 0 : 1);
            return_all(H);
        }
        if (msg->cmd == SYS_CMD_INSTALL) {
            H->reply(H->self, msg, MDPSR_CMD_REPLY, nullptr, 0);
        }
        return rc;
    }

    case SYS_CMD_CYCLE: {
        sys_req r{};
        read_req(body, len, &r);
        if (r.rounds <= 0 || r.rounds > 100000) return MDPSR_ERR_OUT_OF_RANGE;
        const char* manifest = manifest_of(r.plugin);
        if (!manifest) return MDPSR_ERR_PLUGIN_NOT_FOUND;

        int ok = 0, failn = 0, first_err = 0;
        for (int i = 0; i < r.rounds; ++i) {
            /* 手上是空的: 可以放心调用装卸 (BUSY 由 call_mgmt 重试掉) */
            int rc = call_mgmt(H, "CYCLE 卸载", SYS_CMD_UNINSTALL, r.plugin, manifest);
            if (rc != MDPSR_OK) {
                ++failn;
                if (!first_err) first_err = rc;
            } else {
                ++ok;
            }
            rc = call_mgmt(H, "CYCLE 装载", SYS_CMD_INSTALL, r.plugin, manifest);
            if (rc != MDPSR_OK) {
                ++failn;
                if (!first_err) first_err = rc;
            } else {
                ++ok;
            }
        }

        SysmgrMgr* m = nullptr; sys_status* st = nullptr;
        if (borrow_all(H, &m, &st) == MDPSR_OK) {
            m->installs    += static_cast<uint32_t>(ok / 2 + ok % 2);
            m->uninstalls  += static_cast<uint32_t>(ok / 2);
            m->cycles      += 1;
            m->last_plugin  = r.plugin;
            if (failn) { m->fails += static_cast<uint32_t>(failn); m->last_error = static_cast<uint32_t>(first_err); }
            flush_status(m, st);
            m->log("CYCLE '" + std::string(name_of(r.plugin)) + "' x" + std::to_string(r.rounds) +
                   ": 成功 " + std::to_string(ok) + " 步, 失败 " + std::to_string(failn) + " 步",
                   failn ? 1 : 0);
            return_all(H);
        }

        mdpsr_reply rep{};
        rep.cmd   = SYS_CMD_CYCLE;
        rep.code  = failn;
        rep.src   = ctx->handle;
        rep.seq   = static_cast<uint32_t>(r.rounds);
        rep.value = static_cast<uint32_t>(ok);
        H->reply(H->self, msg, MDPSR_CMD_REPLY, &rep, sizeof(rep));
        return failn ? MDPSR_ERR_PLUGIN_BUSY : MDPSR_OK;
    }

    case SYS_CMD_LIST: {
        SysmgrMgr* m = nullptr; sys_status* st = nullptr;
        if (borrow_all(H, &m, &st) != MDPSR_OK) return MDPSR_ERR_BAD_STATE;
        uint64_t keys[64];
        uint32_t n = 0;
        H->plugin_list(H->self, keys, 64, &n);
        std::string all;
        for (uint32_t i = 0; i < n; ++i) {
            mdpsr_plugin_info pi{};
            pi.struct_size = sizeof(pi);
            if (H->plugin_info(H->self, keys[i], &pi) == MDPSR_OK) {
                if (!all.empty()) all += ", ";
                all += pi.name;
                all += "(gen " + std::to_string(pi.gen) + ", " + std::to_string(pi.entry_count) + " 项)";
            }
        }
        m->log("当前在线 " + std::to_string(n) + " 个插件: " + all);
        return_all(H);
        return MDPSR_OK;
    }

    case SYS_CMD_QUERY: {
        SysmgrMgr* m = nullptr; sys_status* st = nullptr;
        if (borrow_all(H, &m, &st) != MDPSR_OK) return MDPSR_ERR_BAD_STATE;
        /* 把账本原样回给发件人 */
        if (msg->src) H->reply(H->self, msg, MDPSR_CMD_REPLY, st, sizeof(*st));
        m->log("QUERY: 装 " + std::to_string(st->installs) + " / 卸 " +
               std::to_string(st->uninstalls) + " / 失败 " + std::to_string(st->fails));
        return_all(H);
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
    slog(ctx->host, 0, "[sysmgr] 内核组件已挂载 (plugin " +
                           s_of(ctx->plugin) + ", gen " + std::to_string(ctx->plugin_gen) + ")");
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(void) {
}
