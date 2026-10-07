/* ============================================================================
 *  core_plugin.cpp —— 内核组件的导出层
 *
 *  一个插件的全部对外东西都在这里。符号名 = mdpsr_<种类>_<清单条目名>:
 *
 *      plugin.json                  导出
 *      ------------------------------------------------------------------
 *      State  "core_State_ResourceDict"   mdpsr_state_core_State_ResourceDict
 *      Object "core_Mgr"                  mdpsr_object_core_Mgr (+ _destroy)
 *      Handle "core_Handle_Mgr"           mdpsr_handle_core_Handle_Mgr
 *      后门                               core_Handle_Mgr
 *
 *  三个东西各司其职:
 *      mdpsr_handle_core_Handle_Mgr  无状态入口: 解析字节流 -> 交给 core_Mgr -> 按需回滚
 *      mdpsr_object_core_Mgr         管理对象: 负责全局资源的增删 (装载/卸载)
 *      core_State_ResourceDict       公用字典: 已装载插件 -> 它们的资源键
 *
 *  【消息】载荷 = { int32 _cmd; uint32 _pad; uint64 _plugin; char _path[]; }
 *      cmd = 0  初始化
 *      cmd = 1  装载 (用 _path): 装完给新插件的 Init handle 点火
 *      cmd = 2  卸载 (用 _plugin): 忙就原样回滚给自己, 直到成功或超次数
 * ==========================================================================*/
#include "core_mgr.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

using namespace mdpsr;
using namespace mdpsr::core;

/* ==========================================================================
 *  载荷
 * ==========================================================================*/
struct CoreCmd {
    int32_t  _cmd;
    uint32_t _pad;
    uint64_t _plugin;
    /* char _path[]; 紧跟其后, NUL 结尾 */
};

#define CORE_CMD_INIT   0
#define CORE_CMD_LOAD   1
#define CORE_CMD_UNLOAD 2

/* 条目名 —— 必须和 plugin.json 里逐字一致, 并且**全局唯一**
 * (MainMap 的键是 hash(条目名), 不分种类共用同一个键空间)。
 * 所以这里不能只写 "Mgr": core 的 State/Object/Handle 全叫 Mgr 会互相撞键。 */
static const char* kHandleName = "core_Handle_Mgr";
static const char* kObjectName = "core_Mgr";
static const char* kStateName  = "core_State_ResourceDict";
static const char* kPluginName = "core";

static void clog(const mdpsr_host* H, int level, const std::string& s) {
    if (H && H->log) H->log(H->host, level, s.c_str());
}

/* 组一条 core 消息 */
static size_t pack_cmd(uint8_t* out, size_t cap, int32_t cmd, uint64_t plugin, const char* path) {
    const size_t plen = path ? std::strlen(path) : 0;
    if (!out || cap < sizeof(CoreCmd) + plen + 1) return 0;
    CoreCmd h{};
    h._cmd = cmd;
    h._plugin = plugin;
    std::memcpy(out, &h, sizeof(h));
    size_t n = sizeof(h);
    if (plen) { std::memcpy(out + n, path, plen); n += plen; }
    out[n++] = 0;
    return n;
}

/* ==========================================================================
 *  State: core_State_ResourceDict —— 纯 POD, 从 ctx->pool (== Pool_state) 分配
 * ==========================================================================*/
MDPSR_EXPORT mdpsr_state* mdpsr_state_core_State_ResourceDict(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;

    void* mem = ctx->pool->allocate(sizeof(ResourceDict), alignof(ResourceDict));
    if (!mem) return nullptr;
    auto* d = new (mem) ResourceDict{};
    d->magic = ResourceDict::kMagic;
    d->count = 0;

    clog(ctx->host, 0, std::string("[core] State '") + kStateName + "' 已挂载: " +
                           std::to_string(CORE_MAX_PLUGINS) + " 个插件槽 x " +
                           std::to_string(CORE_MAX_KEYS) + " 项资源");
    return reinterpret_cast<mdpsr_state*>(d);
}

/* ==========================================================================
 *  Object: core_Mgr
 * ==========================================================================*/
MDPSR_EXPORT void* mdpsr_object_core_Mgr(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->pool || !ctx->host) return nullptr;
    void* mem = ctx->pool->allocate(sizeof(DllMgr), alignof(DllMgr));
    if (!mem) return nullptr;
    try {
        auto* o = new (mem) DllMgr(ctx->host, ctx->pool);
        clog(ctx->host, 0, "[core] Object 'core_Mgr' 已构造");
        return o;
    } catch (...) {
        ctx->pool->deallocate(mem, sizeof(DllMgr), alignof(DllMgr));
        clog(ctx->host, 2, "[core] Object 构造异常");
        return nullptr;
    }
}

MDPSR_EXPORT void mdpsr_object_core_Mgr_destroy(void* instance) {
    /* 析构必须走池, 不能用 delete —— 对象是从 ctx->pool 分配出来的。
     * 池指针在 Destroy 之前从对象上取 (析构之后就取不到了)。 */
    auto* o = static_cast<DllMgr*>(instance);
    if (!o) return;
    std::pmr::memory_resource* pool = o->pool();
    o->~DllMgr();
    if (pool) pool->deallocate(o, sizeof(DllMgr), alignof(DllMgr));
}

/* ==========================================================================
 *  Handle: core_Handle_Mgr —— 无状态入口
 * ==========================================================================*/
MDPSR_EXPORT int mdpsr_handle_core_Handle_Mgr(const uint8_t* msgData, size_t msgLen,
                                              const mdpsr_context* ctx) {
    if (!ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = ctx->host;

    CoreCmd head{};
    if (!mdpsr_read_pod(msgData, msgLen, &head)) {
        clog(H, 1, "[core] 载荷长度不足 (至少需要 CoreCmd)");
        return MDPSR_ERR_BAD_MESSAGE;
    }
    const char* path = mdpsr_read_str(msgData, msgLen, sizeof(CoreCmd));

    /* 管理对象: 从 object 字典按名字取 (本插件自己的 object, 不会被别人碰) */
    mdpsr_ptr* op = ctx->object ? H->map_find(ctx->object, mdpsr_hash64(kObjectName)) : nullptr;
    if (!op) {
        clog(H, 2, std::string("[core] 找不到 object '") + kObjectName + "'");
        return MDPSR_ERR_BAD_STATE;
    }
    auto* mgr = static_cast<DllMgr*>(H->map_ptr_of(H->host, op));
    if (!mgr) return MDPSR_ERR_BAD_STATE;

    DllMgr::Cmd c{};
    c.cmd    = head._cmd;
    c.plugin = head._plugin;
    c.path   = path;

    bool need_retry = false;
    const int r = mgr->handle_cmd(c, ctx->state, &need_retry);

    /* ---- 卸载被推迟: 把这条消息原样回滚给自己, 下轮再来 ---- */
    if (need_retry) {
        mgr->bump_retry(head._plugin);
        if (mgr->retries_of(head._plugin) > DllMgr::kMaxRetry) {
            clog(H, 2, "[core] 卸载重试 " + std::to_string(DllMgr::kMaxRetry) +
                           " 次仍然忙, 放弃: plugin=" + std::to_string(head._plugin));
            mgr->clear_retry(head._plugin);
            return MDPSR_ERR_PLUGIN_BUSY;
        }
        /* 投回自己所在的队列 —— 先用 queue 字典确认那条队列还在,
         * 再按它的键投回去。这样卸载消息留在同一条线程上, 下一次调度再试。 */
        const uint64_t qk = mdpsr_hash64(MDPSR_QUEUE_PLUGINMGR_NAME);
        mdpsr_ptr* qp = ctx->queue ? H->map_find(ctx->queue, qk) : nullptr;
        if (!qp) {
            clog(H, 2, "[core] 找不到 " MDPSR_QUEUE_PLUGINMGR_NAME ", 卸载消息没法回滚");
            return MDPSR_ERR_NOT_FOUND;
        }
        return H->queue_emit(H->host, qk, mdpsr_hash64(kHandleName), msgData, msgLen);
    }

    if (r != MDPSR_OK) return r;

    /* ---- 装载成功: 给新装上的插件点火 ----
     * 这一步必须由装载者做: 只有它能问出刚装进来的插件叫什么、Init 是谁。 */
    if (head._cmd == CORE_CMD_LOAD) {
        char hname[MDPSR_NAME_MAX] = { 0 };
        if (H->init_handle_of(H->host, head._plugin, hname, sizeof(hname)) == MDPSR_OK &&
            hname[0]) {
            mdpsr_init_cmd init{};
            init._cmd = MDPSR_CMD_INIT;
            const int er = H->emit(H->host, mdpsr_hash64(hname), &init, sizeof(init));
            clog(H, er == MDPSR_OK ? 0 : 2,
                 std::string("[core] 给新插件点火 -> ") + hname +
                     " (rc=" + std::to_string(er) + ")");
        } else {
            clog(H, 1, "[core] 新插件没有声明 Init handle, 不点火");
        }
    }
    return MDPSR_OK;
}

/* ==========================================================================
 *  后门: core_Handle_Mgr
 *
 *  宿主读完 config.json 之后直接调用它 —— 不经过消息队列。它负责:
 *      1. 建 Queue_pluginmgr
 *      2. 把内核 handle 绑到那条队列上
 *      3. 投出第一条 cmd=0 (初始化资源字典)
 *
 *  注意它【不】负责装载其它插件: 那是消息驱动的正常链路, 由宿主 boot()
 *  之后投 cmd=1 完成 —— 后门只负责"把内核自己立起来"。
 * ==========================================================================*/
MDPSR_EXPORT int core_Handle_Mgr(const mdpsr_host* H) {
    if (!H || !H->abi_version) return MDPSR_ERR_NULLPTR;
    if (H->abi_version != MDPSR_ABI_VERSION) return MDPSR_ERR_PLUGIN_ABI;
    if (!H->queue_create || !H->queue_bind || !H->queue_emit) return MDPSR_ERR_PLUGIN_ABI;

    /* --- 1. 建 Queue_pluginmgr --- */
    mdpsr_queue_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.capacity    = 8192;   /* 装载/卸载是重活, 缓冲开大点 */
    desc.pace_ms     = 0;      /* 内核队列不节流: 它按需干活, 没有自转循环 */

    const uint64_t self_plugin =
        mdpsr_hash64(("plugin:" + std::string(kPluginName)).c_str());

    mdpsr_ptr* qp = nullptr;
    int r = H->queue_create(H->host, MDPSR_QUEUE_PLUGINMGR_NAME, &desc, self_plugin, &qp);
    if (r != MDPSR_OK) {
        clog(H, 2, std::string("[core] 后门: 建立 ") + MDPSR_QUEUE_PLUGINMGR_NAME +
                       " 失败 (" + std::to_string(r) + ")");
        return r;
    }
    const uint64_t qk = mdpsr_hash64(MDPSR_QUEUE_PLUGINMGR_NAME);

    /* --- 2. 内核 handle 绑到它 --- */
    const uint64_t hk = mdpsr_hash64(kHandleName);
    r = H->queue_bind(H->host, hk, qk);
    clog(H, r == MDPSR_OK ? 0 : 1,
         std::string("[core] 后门: handle '") + kHandleName + "' -> " +
             MDPSR_QUEUE_PLUGINMGR_NAME + " (rc=" + std::to_string(r) + ")");

    /* --- 3. 投出第一条 cmd=0 --- */
    uint8_t buf[sizeof(CoreCmd) + 1];
    const size_t n = pack_cmd(buf, sizeof(buf), CORE_CMD_INIT, 0, nullptr);
    if (!n) return MDPSR_ERR_BAD_MESSAGE;
    const int er = H->queue_emit(H->host, qk, hk, buf, n);
    clog(H, er == MDPSR_OK ? 0 : 2,
         std::string("[core] 后门: 首条 cmd=0 已投进 ") + MDPSR_QUEUE_PLUGINMGR_NAME +
             " (rc=" + std::to_string(er) + ")");
    return er;
}

/* ==========================================================================
 *  模块导出
 * ==========================================================================*/
MDPSR_EXPORT uint32_t mdpsr_abi_version(void) { return MDPSR_ABI_VERSION; }

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_host* host, uint64_t plugin) {
    if (!host) return MDPSR_ERR_NULLPTR;
    if (host->abi_version != MDPSR_ABI_VERSION) return MDPSR_ERR_PLUGIN_ABI;
    if (host->struct_size < sizeof(mdpsr_host)) return MDPSR_ERR_PLUGIN_ABI;
    clog(host, 0, "[core] 内核组件已挂载 (plugin=" + std::to_string(plugin) + ")");
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(uint64_t plugin) {
    (void)plugin;
}
