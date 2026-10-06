/* ============================================================================
 *  mdpsr/scr/core/dllmgr.cpp
 *  内核组件 DLLMgr
 *
 *  它不再有任何特殊路径: 和普通插件一样, 全部配置写在 core/plugin.json 里,
 *  由宿主统一装载 (LoadLibrary -> module_init -> state/object/handle 注册)。
 *
 *  清单声明:
 *      handle  DLLMgr_handle           (handle_core)
 *      object  DLLMgr                  (管理器类)
 *      state   DLLMgr_resecoure_Dict   所有已加载 dll 的资源索引字典
 *      state   DLLMgr_link             要 link 的插件 plugin.json 路径列表 (json 里手填)
 *      state   DLLMgr_plugin           DLLMgr 自己的 plugin.json 相对位置
 *
 *  DLLMgr_handle 的字节流强类型化为:
 *      { int32_t _cmd; uint32_t _pad; uint64_t _dll; char _path[]; }
 *      cmd = 0 初始化 -> DLLMgr.cmd(0)
 *      cmd = 1 加载   -> DLLMgr.cmd(1, path)  完成后记录进 DLLMgr_resecoure_Dict
 *      cmd = 2 卸载   -> DLLMgr.cmd(2, dll)   完成后同步 DLLMgr_resecoure_Dict
 *      cmd = 3 清空   -> DLLMgr.cmd(3)
 * ==========================================================================*/
#include <mdpsr/abi.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory_resource>
#include <string>
#include <vector>

/* ==========================================================================
 *  类型标签
 * ==========================================================================*/
#define KIND_DICT   0x44494352u   /* 'DICR' */
#define KIND_LINK   0x4C494E4Bu   /* 'LINK' */
#define KIND_PLUGIN 0x504C5547u   /* 'PLUG' */

/* ==========================================================================
 *  State 载荷: 三个公用数据对象
 * ==========================================================================*/

/* DLLMgr_resecoure_Dict: 一个字典, 保存所有加载 dll 的资源索引 */
struct DictEntry {
    uint64_t                   dll = 0;
    std::pmr::vector<uint64_t> keys;
    explicit DictEntry(std::pmr::memory_resource* r) : keys(r) {}
};

struct ResourceDict {
    std::pmr::map<uint64_t, DictEntry*> table;
    mdpsr_resource*                     res = nullptr;
    explicit ResourceDict(std::pmr::memory_resource* r) : table(r), res(r) {}
    DictEntry* find(uint64_t dll) {
        auto it = table.find(dll);
        return it == table.end() ? nullptr : it->second;
    }
};

/* DLLMgr_link: 路径列表 */
struct LinkList {
    std::pmr::vector<std::pmr::string> items;
    explicit LinkList(std::pmr::memory_resource* r) : items(r) {}
};

/* DLLMgr_plugin: 字符串 */
struct PluginPath {
    std::pmr::string path;
    PluginPath(std::pmr::memory_resource* r, const char* p) : path(p ? p : "", r) {}
};

/* ==========================================================================
 *  小工具
 * ==========================================================================*/
static void dlog(const mdpsr_host* H, int level, const std::string& s) {
    if (H && H->log) H->log(H->host, level, s.c_str());
}

template <typename T>
static T* pool_new(mdpsr_resource* res) {
    void* m = res->allocate(sizeof(T), alignof(T));
    if (!m) return nullptr;
    return new (m) T{};
}

/* 需要一个 memory_resource* 构造参数的容器类型 */
template <typename T>
static T* pool_new_res(mdpsr_resource* res) {
    void* m = res->allocate(sizeof(T), alignof(T));
    if (!m) return nullptr;
    return new (m) T(res);
}

template <typename T>
static void pool_delete(mdpsr_resource* res, T* p) {
    if (!p) return;
    p->~T();
    res->deallocate(p, sizeof(T), alignof(T));
}

/* 把 C++ 公用对象放进 State 载荷, 并登记析构 */
template <typename T>
static void state_destroy(mdpsr_state* st) {
    if (st && st->payload) static_cast<T*>(st->payload)->~T();
}

template <typename T, typename... Args>
static T* state_emplace(mdpsr_state* st, Args&&... args) {
    T* p = new (st->payload) T(static_cast<Args&&>(args)...);
    st->destroy = &state_destroy<T>;
    return p;
}

/* 往队列里回滚一条 DLLMgr 消息。
   优先投进"当前正在驱动我的那条分流队列", 投不进去由宿主回退到 queue_core。 */
static int emit_cmd(const mdpsr_host* H, mdpsr_queue* q, int cmd, uint64_t dll, const char* path) {
    uint8_t buf[sizeof(mdpsr_dllmgr_cmd) + MDPSR_PATH_MAX];
    const size_t n = mdpsr_dllmgr_pack(buf, sizeof(buf), cmd, dll, path ? path : "");
    if (!n) return MDPSR_ERR_BAD_MESSAGE;
    return H->queue_emit(H->host, q, mdpsr_hash64("DLLMgr_handle"), buf, n);
}

/* ==========================================================================
 *  Object: DLLMgr 管理器类
 * ==========================================================================*/
class DllMgr {
public:
    DllMgr(std::pmr::memory_resource* pool, const mdpsr_host* host, const char* name)
        : _pool(pool), _host(host) {
        std::snprintf(_name, sizeof(_name), "%s", name ? name : "DLLMgr");
    }
    ~DllMgr() = default;

    mdpsr_resource* Pool() const { return _pool; }

    /* q = 本次调用所在的分流队列 (来自 Content::queue) */
    int cmd(int c, uint64_t dll, const char* path,
            ResourceDict* dict, LinkList* link, PluginPath* self, mdpsr_queue* q) {
        switch (c) {
        case MDPSR_DLLMGR_CMD_INIT:   return on_init(dict, link, self, q);
        case MDPSR_DLLMGR_CMD_LOAD:   return on_load(path, dict, q);
        case MDPSR_DLLMGR_CMD_UNLOAD: return on_unload(dll, dict, q);
        case MDPSR_DLLMGR_CMD_CLEAR:  return on_clear(dict, q);
        default:
            dlog(_host, 1, "[DLLMgr] 未知命令 " + std::to_string(c));
            return MDPSR_ERR_UNKNOWN_CMD;
        }
    }

private:
    /* --- cmd = 0: 解析 DLLMgr_plugin, 把每个要 link 的插件变成一条加载消息回滚进 queue --- */
    int on_init(ResourceDict* dict, LinkList* link, PluginPath* self, mdpsr_queue* q) {
        if (!dict || !link) return MDPSR_ERR_BAD_STATE;
        if (self) {
            dlog(_host, 0, std::string("[DLLMgr] cmd=0 解析 DLLMgr_plugin = ") +
                              (self->path.empty() ? "(未声明)" : self->path.c_str()));
        }
        _last_drain = static_cast<size_t>(-1);

        if (link->items.empty()) {
            dlog(_host, 1, "[DLLMgr] DLLMgr_link 为空, 没有要 link 的插件");
            return MDPSR_OK;
        }
        for (const std::pmr::string& item : link->items) {
            /* DLLMgr 自己不用再发加载消息 */
            if (self && !self->path.empty() && item == self->path) continue;
            dlog(_host, 0, "[DLLMgr] cmd=0 回滚加载消息: " + std::string(item.c_str()));
            emit_cmd(_host, q, MDPSR_DLLMGR_CMD_LOAD, 0, item.c_str());
        }
        return MDPSR_OK;
    }

    /* --- cmd = 1: 加载一个插件, 完成后把它的资源键记进 DLLMgr_resecoure_Dict --- */
    int on_load(const char* path, ResourceDict* dict, mdpsr_queue* q) {
        (void)q;
        if (!path || !*path) return MDPSR_ERR_BAD_CONFIG;
        if (!dict) return MDPSR_ERR_BAD_STATE;

        uint64_t dll = 0;
        const int r = _host->plugin_load(_host->host, path, &dll);
        if (r != MDPSR_OK) {
            dlog(_host, 2, std::string("[DLLMgr] cmd=1 加载失败(") + std::to_string(r) + "): " + path);
            return r;
        }

        DictEntry* e = dict->find(dll);
        if (!e) {
            e = pool_new_res<DictEntry>(dict->res);
            e->dll = dll;
            dict->table.emplace(dll, e);
        }
        e->keys.clear();
        uint32_t n = 0;
        _host->plugin_keys(_host->host, dll, nullptr, 0, &n);
        if (n) {
            e->keys.resize(n);
            _host->plugin_keys(_host->host, dll, e->keys.data(), n, &n);
            e->keys.resize(n);
        }
        dlog(_host, 0, "[DLLMgr] cmd=1 完成: " + std::string(path) +
                          " -> dll=" + std::to_string(dll) +
                          ", DLLMgr_resecoure_Dict 记录 " + std::to_string(e->keys.size()) + " 项");
        return MDPSR_OK;
    }

    /* --- cmd = 2: 卸载, 完成后同步 DLLMgr_resecoure_Dict --- */
    int on_unload(uint64_t dll, ResourceDict* dict, mdpsr_queue* q) {
        (void)q;
        if (!dict) return MDPSR_ERR_BAD_STATE;
        const int r = _host->plugin_unload(_host->host, dll);
        if (r != MDPSR_OK) {
            dlog(_host, 1, "[DLLMgr] cmd=2 卸载失败(" + std::to_string(r) +
                              "): dll=" + std::to_string(dll));
            return r;
        }
        auto it = dict->table.find(dll);
        if (it != dict->table.end()) {
            pool_delete(dict->res, it->second);
            dict->table.erase(it);
        }
        dlog(_host, 0, "[DLLMgr] cmd=2 完成: dll=" + std::to_string(dll) +
                          ", DLLMgr_resecoure_Dict 剩余 " + std::to_string(dict->table.size()) + " 项");
        return MDPSR_OK;
    }

    /* --- cmd = 3: 清空区域 ---
     * 把字典里所有插件都回滚成卸载指令; 如果当前还非空, 再回滚一条 cmd=3。
     * 卸载消息被处理完后字典会变小, 直到空 -> 下一条 cmd=3 直接返回, 收敛。 */
    int on_clear(ResourceDict* dict, mdpsr_queue* q) {
        if (!dict) return MDPSR_ERR_BAD_STATE;
        const size_t n = dict->table.size();
        if (n == 0) {
            _last_drain = 0;
            dlog(_host, 0, "[DLLMgr] cmd=3 完成: 区域已空");
            return MDPSR_OK;
        }
        if (n == _last_drain) {
            dlog(_host, 2, "[DLLMgr] cmd=3 停滞: 仍有 " + std::to_string(n) +
                              " 项卸载不掉, 停止回滚以免死循环");
            return MDPSR_ERR_PLUGIN_BUSY;
        }
        _last_drain = n;

        std::vector<uint64_t> dlls;
        dlls.reserve(n);
        for (auto& kv : dict->table) dlls.push_back(kv.first);

        for (uint64_t d : dlls) {
            dlog(_host, 0, "[DLLMgr] cmd=3 回滚卸载消息: dll=" + std::to_string(d));
            emit_cmd(_host, q, MDPSR_DLLMGR_CMD_UNLOAD, d, "");
        }
        /* 当前非空 -> 再回滚一条 cmd=3 收尾 */
        dlog(_host, 0, "[DLLMgr] cmd=3 区域非空, 再回滚一条 cmd=3");
        emit_cmd(_host, q, MDPSR_DLLMGR_CMD_CLEAR, 0, "");
        return MDPSR_OK;
    }

    mdpsr_resource*   _pool;
    const mdpsr_host* _host;
    char              _name[MDPSR_NAME_MAX] = { 0 };
    size_t            _last_drain = static_cast<size_t>(-1);
};

/* ==========================================================================
 *  state 工厂
 * ==========================================================================*/
MDPSR_EXPORT mdpsr_state* mdpsr_state_resource_dict(const mdpsr_state_ctx* ctx) {
    if (!ctx || !ctx->pool) return nullptr;
    mdpsr_state* st = mdpsr_state_new(ctx->pool, KIND_DICT, sizeof(ResourceDict));
    if (!st) return nullptr;
    state_emplace<ResourceDict>(st, ctx->pool);
    if (ctx->host && ctx->host->log) {
        ctx->host->log(ctx->host->host, 0,
                       (std::string("[DLLMgr] state '") + ctx->name + "' 已挂载 (资源索引字典)").c_str());
    }
    return st;
}

MDPSR_EXPORT mdpsr_state* mdpsr_state_link(const mdpsr_state_ctx* ctx) {
    if (!ctx || !ctx->pool) return nullptr;
    mdpsr_state* st = mdpsr_state_new(ctx->pool, KIND_LINK, sizeof(LinkList));
    if (!st) return nullptr;
    LinkList* ll = state_emplace<LinkList>(st, ctx->pool);
    for (size_t i = 0; i < ctx->list_count; ++i) {
        char buf[MDPSR_PATH_MAX];
        mdpsr_path_join(buf, sizeof(buf), ctx->manifest_dir, ctx->list[i]);
        ll->items.emplace_back(buf);
    }
    if (ctx->host && ctx->host->log) {
        char line[256];
        std::snprintf(line, sizeof(line), "[DLLMgr] state '%s' 已挂载 (link %zu 个插件)",
                      ctx->name ? ctx->name : "?", ll->items.size());
        ctx->host->log(ctx->host->host, 0, line);
        for (const std::pmr::string& s : ll->items) {
            ctx->host->log(ctx->host->host, 0, (std::string("[DLLMgr]   link: ") + s.c_str()).c_str());
        }
    }
    return st;
}

MDPSR_EXPORT mdpsr_state* mdpsr_state_plugin(const mdpsr_state_ctx* ctx) {
    if (!ctx || !ctx->pool) return nullptr;
    mdpsr_state* st = mdpsr_state_new(ctx->pool, KIND_PLUGIN, sizeof(PluginPath));
    if (!st) return nullptr;
    state_emplace<PluginPath>(st, ctx->pool, ctx->manifest ? ctx->manifest : "");
    return st;
}

/* ==========================================================================
 *  object 工厂
 * ==========================================================================*/
MDPSR_EXPORT void* mdpsr_object_dllmgr(const mdpsr_object_ctx* ctx) {
    if (!ctx || !ctx->pool) return nullptr;
    try {
        std::pmr::polymorphic_allocator<DllMgr> a(ctx->pool);
        DllMgr* o = a.new_object<DllMgr>(ctx->pool, ctx->host, ctx->name);
        if (ctx->host && ctx->host->log) {
            ctx->host->log(ctx->host->host, 0,
                           (std::string("[DLLMgr] object '") + (ctx->name ? ctx->name : "?") +
                            "' 已在 dll 专属池构造").c_str());
        }
        return o;
    } catch (...) {
        return nullptr;
    }
}

MDPSR_EXPORT void mdpsr_object_dllmgr_destroy(void* instance) {
    auto* o = static_cast<DllMgr*>(instance);
    if (!o) return;
    std::pmr::memory_resource* pool = o->Pool();
    std::pmr::polymorphic_allocator<DllMgr> a(pool);
    a.delete_object(o);
}

/* ==========================================================================
 *  handle: DLLMgr_handle (handle_core)
 * ==========================================================================*/
MDPSR_EXPORT int mdpsr_handle_dllmgr(const uint8_t* msgData, size_t msgLen,
                                     const uint8_t* ctxData, size_t ctxLen) {
    mdpsr_core_content cc{};
    if (!mdpsr_read_pod(ctxData, ctxLen, &cc)) return MDPSR_ERR_NULLPTR;
    const mdpsr_host* H = cc.host;
    if (!H) return MDPSR_ERR_NULLPTR;

    mdpsr_dllmgr_cmd head{};
    if (!mdpsr_read_pod(msgData, msgLen, &head)) {
        dlog(H, 1, "[DLLMgr] 载荷长度不足");
        return MDPSR_ERR_BAD_MESSAGE;
    }
    const char* path = mdpsr_read_str(msgData, msgLen, sizeof(mdpsr_dllmgr_cmd));
    if (!path) path = "";

    auto* obj = cc.classptr ? static_cast<DllMgr*>(cc.classptr->instance) : nullptr;
    if (!obj) {
        dlog(H, 2, "[DLLMgr] 未绑定 object 'DLLMgr'");
        return MDPSR_ERR_BAD_STATE;
    }

    auto* dict = mdpsr_state_as<ResourceDict>(
        mdpsr_find_state(cc.states, cc.state_count, "DLLMgr_resecoure_Dict"));
    auto* link = mdpsr_state_as<LinkList>(
        mdpsr_find_state(cc.states, cc.state_count, "DLLMgr_link"));
    auto* self = mdpsr_state_as<PluginPath>(
        mdpsr_find_state(cc.states, cc.state_count, "DLLMgr_plugin"));

    /* cc.queue 就是发起本次分发的 Map.Queue*, 回滚消息优先留在它上面 */
    return obj->cmd(head._cmd, head._dll, path, dict, link, self, cc.queue);
}

/* ==========================================================================
 *  后门: DLL_init
 *
 *  宿主读完 config.json 之后会直接用 GetProcAddress("DLL_init") 调用它 ——
 *  不经过消息队列。它负责点火:
 *      1. 建立核心分流队列 queue_core (配方里含它自己的 dispatcher 与常驻线程),
 *         名字挂到 Map.Queue* 上;
 *      2. 把内核自己的 handle 绑到 queue_core;
 *      3. 往 queue_core 投出第一条消息 (cmd=0 初始化), 让正常链路跑起来。
 * ==========================================================================*/
MDPSR_EXPORT int DLL_init(const mdpsr_host* H) {
    if (!H || !H->abi_version) return MDPSR_ERR_NULLPTR;
    if (H->abi_version != MDPSR_ABI_VERSION) return MDPSR_ERR_PLUGIN_ABI;
    if (!H->queue_create || !H->queue_emit) return MDPSR_ERR_PLUGIN_ABI;

    /* 1. queue_core + 它的分发线程 */
    mdpsr_queue* core = nullptr;
    const int r = H->queue_create(H->host, MDPSR_QUEUE_CORE_NAME, 0, 0, &core);
    if (r != MDPSR_OK || !core) {
        dlog(H, 2, "[DLLMgr] DLL_init: 建立 queue_core 失败(" + std::to_string(r) + ")");
        return r != MDPSR_OK ? r : MDPSR_ERR_CREATE_FAILED;
    }

    /* 2. 内核 handle 绑到 queue_core (别的组件不声明 queue 时默认也走这条) */
    const uint64_t h = mdpsr_hash64("DLLMgr_handle");
    H->queue_bind(H->host, h, core);

    dlog(H, 0, std::string("[DLLMgr] DLL_init: Map.Queue* '") + core->label +
                  "' 已挂载, 容量 " + std::to_string(core->capacity) + "B");

    /* 3. 投出第一条消息 */
    uint8_t buf[sizeof(mdpsr_dllmgr_cmd) + 1];
    const size_t n = mdpsr_dllmgr_pack(buf, sizeof(buf), MDPSR_DLLMGR_CMD_INIT, 0, "");
    if (!n) return MDPSR_ERR_BAD_MESSAGE;
    const int er = H->queue_emit(H->host, core, h, buf, n);
    dlog(H, er == MDPSR_OK ? 0 : 2,
         "[DLLMgr] DLL_init: 首条 cmd=0 已投进 queue_core (rc=" + std::to_string(er) + ")");
    return er;
}

/* ==========================================================================
 *  模块导出
 * ==========================================================================*/
MDPSR_EXPORT uint32_t mdpsr_abi_version(void) { return MDPSR_ABI_VERSION; }

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_host* host, uint64_t dll) {
    if (!host) return MDPSR_ERR_NULLPTR;
    if (host->abi_version != MDPSR_ABI_VERSION) return MDPSR_ERR_PLUGIN_ABI;
    if (host->struct_size < sizeof(mdpsr_host)) return MDPSR_ERR_PLUGIN_ABI;
    if (host->log) {
        host->log(host->host, 0,
                  ("[DLLMgr] 内核组件已挂载 (全部配置来自 core/plugin.json), dll=" +
                   std::to_string(dll)).c_str());
    }
    return MDPSR_OK;
}

MDPSR_EXPORT void mdpsr_module_fini(uint64_t) {
}
