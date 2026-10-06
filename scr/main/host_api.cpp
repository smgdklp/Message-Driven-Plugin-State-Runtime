/* ============================================================================
 *  mdpsr/scr/main/host_api.cpp
 *  把 Runtime 的能力装配成 mdpsr_host 函数表, 供 DLLMgr 与所有插件回调。
 * ==========================================================================*/
#include "runtime.h"

#include <cstdio>
#include <cstring>

namespace mdpsr {

static inline Runtime* RT(void* host) { return static_cast<Runtime*>(host); }

/* ------------------------------ 基础 ------------------------------ */
static uint64_t api_hash(const char* name) { return mdpsr_hash64(name); }

static void api_log(void* host, int level, const char* msg) {
    RT(host)->log(level, msg ? msg : "");
}

static const char* api_root_dir(void* host) { return RT(host)->root_utf8().c_str(); }

/* ------------------------------ 池 ------------------------------ */
static mdpsr_resource* api_pool_state(void* host) { return RT(host)->pools().state(); }
static mdpsr_resource* api_pool_map(void* host)   { return RT(host)->pools().map(); }
static mdpsr_resource* api_pool_msg(void* host)   { return RT(host)->pools().msg(); }

/* ------------------------------ Map ------------------------------ */
static int api_map_register(void* host, const mdpsr_entry* e) {
    if (!e) return MDPSR_ERR_NULLPTR;
    const int r = RT(host)->map().reg(*e);
    if (r == MDPSR_ERR_ALREADY_EXISTS) {
        RT(host)->log(1, std::string("Map 键冲突, 已存在: ") + (e->name ? e->name : "?"));
    }
    return r;
}

static int api_map_lookup(void* host, uint64_t key, void** out_ptr, int write) {
    Countptr* cp = nullptr;
    const int r = RT(host)->map().acquire(key, write != 0, &cp);
    if (r != MDPSR_OK) return r;
    if (out_ptr) *out_ptr = cp->ptr;
    return MDPSR_OK;
}

static int api_map_release(void* host, uint64_t key, int write) {
    return RT(host)->map().release(key, write != 0);
}
static int api_map_invalidate(void* host, uint64_t key) { return RT(host)->map().invalidate(key); }
static int api_map_erase(void* host, uint64_t key)      { return RT(host)->map().erase(key); }
static int api_map_wait_idle(void* host, uint64_t key, uint32_t t) { return RT(host)->map().wait_idle(key, t); }
static int api_map_dll_idle(void* host, uint64_t dll, uint32_t t)   { return RT(host)->map().dll_idle(dll, t); }

static int api_map_snapshot(void* host, uint64_t dll, mdpsr_entry_info* out,
                            uint32_t cap, uint32_t* out_count) {
    return RT(host)->map().snapshot(dll, out, cap, out_count);
}

/* ------------------------------ DLL 池 ------------------------------ */
static int api_dll_pool_create(void* host, uint64_t dll, const char* name, mdpsr_dllptr* out) {
    return RT(host)->dll_pool_create(dll, name, out);
}

static int api_dll_pool_get(void* host, uint64_t dll, mdpsr_dllptr* out) {
    mdpsr_dllptr* dp = RT(host)->dllptr(dll);
    if (!dp) return MDPSR_ERR_MAP_POOL;
    if (out) *out = *dp;
    return MDPSR_OK;
}

static int api_dll_pool_release(void* host, uint64_t dll) {
    return RT(host)->pools().dll_pool_drop(dll) ? MDPSR_OK : MDPSR_ERR_NOT_FOUND;
}

/* ------------------------------ 消息 ------------------------------ */
static int api_emit(void* host, uint64_t handle_key, const void* data, size_t len) {
    return RT(host)->emit(handle_key, data, len);
}

static int api_queue_emit(void* host, mdpsr_queue* q, uint64_t handle_key,
                          const void* data, size_t len) {
    return RT(host)->queue_emit(q, handle_key, data, len);
}

/* ------------------------------ 分流队列 / 分发器 ------------------------------ */
static int api_queue_create(void* host, const char* name, uint32_t capacity,
                            uint64_t dll, mdpsr_queue** out) {
    if (!name || !*name) return MDPSR_ERR_NULLPTR;
    return RT(host)->queue_create(name, capacity, dll, false, out);
}

static int api_queue_destroy(void* host, mdpsr_queue* q) {
    return RT(host)->queue_destroy(q);
}

static int api_queue_bind(void* host, uint64_t handle_key, mdpsr_queue* q) {
    return RT(host)->queue_bind(handle_key, q);
}

static mdpsr_queue* api_queue_core(void* host) {
    return RT(host)->queue_core();
}

static int api_queue_list(void* host, mdpsr_queue** out, uint32_t cap, uint32_t* out_count) {
    return RT(host)->queue_list(out, cap, out_count);
}

static size_t api_dispatcher_count(void* host) {
    return RT(host)->dispatcher_count();
}

/* ------------------------------ 清单 ------------------------------ */
static int api_manifest_dll(void* host, uint64_t dll, uint32_t index, char* out, uint32_t cap) {
    const Manifest* m = RT(host)->manifest(dll);
    if (!m || index >= m->dlls.size()) return MDPSR_ERR_NOT_FOUND;
    if (out && cap) std::snprintf(out, cap, "%s", m->dlls[index].c_str());
    return MDPSR_OK;
}

static int api_manifest_count(void* host, uint64_t dll, int kind, uint32_t* out_count) {
    const Manifest* m = RT(host)->manifest(dll);
    if (!m) return MDPSR_ERR_NOT_FOUND;
    size_t n = 0;
    switch (kind) {
    case MDPSR_ENTRY_STATE:       n = m->states.size();  break;
    case MDPSR_ENTRY_OBJECT:      n = m->objects.size(); break;
    case MDPSR_ENTRY_HANDLE:
    case MDPSR_ENTRY_HANDLE_CORE: n = m->handles.size(); break;
    default: return MDPSR_ERR_OUT_OF_RANGE;
    }
    if (out_count) *out_count = static_cast<uint32_t>(n);
    return MDPSR_OK;
}

static int api_manifest_entry(void* host, uint64_t dll, int kind, uint32_t index,
                              mdpsr_manifest_entry* out) {
    const Manifest* m = RT(host)->manifest(dll);
    if (!m) return MDPSR_ERR_NOT_FOUND;

    const std::vector<ManifestEntry>* list = nullptr;
    switch (kind) {
    case MDPSR_ENTRY_STATE:       list = &m->states;  break;
    case MDPSR_ENTRY_OBJECT:      list = &m->objects; break;
    case MDPSR_ENTRY_HANDLE:
    case MDPSR_ENTRY_HANDLE_CORE: list = &m->handles; break;
    default: return MDPSR_ERR_OUT_OF_RANGE;
    }
    if (index >= list->size()) return MDPSR_ERR_OUT_OF_RANGE;
    if (!out) return MDPSR_ERR_NULLPTR;

    const ManifestEntry& e = (*list)[index];
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->name, sizeof(out->name), "%s", e.name.c_str());
    std::snprintf(out->symbol, sizeof(out->symbol), "%s", e.symbol.c_str());
    std::snprintf(out->type, sizeof(out->type), "%s", e.type.c_str());
    std::snprintf(out->param, sizeof(out->param), "%s", e.param.c_str());
    std::snprintf(out->bind_object, sizeof(out->bind_object), "%s", e.bind_object.c_str());
    out->param_count      = static_cast<uint32_t>(e.list.size());
    out->type_id          = e.type_id;
    out->bind_state_count = 0;
    for (const std::string& s : e.bind_states) {
        if (out->bind_state_count >= MDPSR_MAX_BIND_STATES) break;
        std::snprintf(out->bind_states[out->bind_state_count], MDPSR_NAME_MAX, "%s", s.c_str());
        out->bind_state_count++;
    }
    return MDPSR_OK;
}

static const char* api_manifest_path(void* host, uint64_t dll) {
    const Manifest* m = RT(host)->manifest(dll);
    return m ? m->rel_path.c_str() : "";
}

/* ------------------------------ 插件装载 ------------------------------ */
static int api_plugin_load(void* host, const char* manifest_rel, uint64_t* out_dll) {
    if (!manifest_rel || !*manifest_rel) return MDPSR_ERR_NULLPTR;
    return RT(host)->plugin_load(manifest_rel, out_dll);
}

static int api_plugin_unload(void* host, uint64_t dll) {
    return RT(host)->plugin_unload(dll);
}

static int api_plugin_loaded(void* host, uint64_t dll) {
    return RT(host)->module_loaded(dll) ? 1 : 0;
}

static int api_plugin_keys(void* host, uint64_t dll, uint64_t* out,
                           uint32_t cap, uint32_t* out_count) {
    return RT(host)->plugin_keys(dll, out, cap, out_count);
}

/* ------------------------------ 组装 ------------------------------ */
void install_host_api(Runtime* rt, mdpsr_host* api) {
    std::memset(api, 0, sizeof(*api));
    api->abi_version = MDPSR_ABI_VERSION;
    api->struct_size = static_cast<uint32_t>(sizeof(mdpsr_host));
    api->host        = rt;

    api->hash     = api_hash;
    api->log      = api_log;
    api->root_dir = api_root_dir;

    api->pool_state = api_pool_state;
    api->pool_map   = api_pool_map;
    api->pool_msg   = api_pool_msg;

    api->map_register   = api_map_register;
    api->map_lookup     = api_map_lookup;
    api->map_release    = api_map_release;
    api->map_invalidate = api_map_invalidate;
    api->map_erase      = api_map_erase;
    api->map_wait_idle  = api_map_wait_idle;
    api->map_dll_idle   = api_map_dll_idle;
    api->map_snapshot   = api_map_snapshot;

    api->dll_pool_create  = api_dll_pool_create;
    api->dll_pool_get     = api_dll_pool_get;
    api->dll_pool_release = api_dll_pool_release;

    api->emit       = api_emit;
    api->queue_emit = api_queue_emit;

    api->queue_create       = api_queue_create;
    api->queue_destroy      = api_queue_destroy;
    api->queue_bind         = api_queue_bind;
    api->queue_core         = api_queue_core;
    api->queue_list         = api_queue_list;
    api->dispatcher_count   = api_dispatcher_count;

    api->manifest_dll   = api_manifest_dll;
    api->manifest_count = api_manifest_count;
    api->manifest_entry = api_manifest_entry;
    api->manifest_path  = api_manifest_path;

    api->plugin_load   = api_plugin_load;
    api->plugin_unload = api_plugin_unload;
    api->plugin_loaded = api_plugin_loaded;
    api->plugin_keys   = api_plugin_keys;
}

} /* namespace mdpsr */
