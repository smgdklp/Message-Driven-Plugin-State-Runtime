/* ============================================================================
 *  mdpsr/runtime/host_api.cpp
 *  把 Runtime 的能力装配成 mdpsr_host 函数表。
 *
 *  铁律: abi.h 里声明的每一个槽位都必须在这里赋值。装配完会逐字段核对,
 *  漏一个就 return 0 —— 宁可启动失败, 也不要让插件调到空指针。
 * ==========================================================================*/
#include "runtime.h"

#include "loader.h"

#include <cstring>

namespace mdpsr {

static inline Runtime* RT(void* host) { return static_cast<Runtime*>(host); }

/* ------------------------------ 基础 ------------------------------ */
static uint64_t api_hash(const char* name) { return mdpsr_hash64(name); }
static void api_log(void* host, int level, const char* msg) {
    RT(host)->log(level, msg ? msg : "");
}
static const char* api_root_dir(void* host) { return RT(host)->root_utf8().c_str(); }

/* ------------------------------ MainMap ------------------------------ */
static mdpsr_ptr* api_map_get(void* host, uint64_t key) { return RT(host)->map().get(key); }

static mdpsr_ptr* api_map_find(mdpsr_map* tbl, uint64_t key) {
    if (!tbl) return nullptr;
    auto* h = tbl;
    return static_cast<mdpsr_ptr*>(h->get(key));
}
static uint32_t api_map_count(mdpsr_map* tbl) {
    if (!tbl) return 0;
    return static_cast<uint32_t>(tbl->size());
}
static int  api_map_is_valid(void* host, mdpsr_ptr* p) { return RT(host)->map().is_valid(p); }
static int  api_map_lock(void* host, mdpsr_ptr* p)     { return RT(host)->map().lock(p); }
static int  api_map_try_lock(void* host, mdpsr_ptr* p) { return RT(host)->map().try_lock(p); }
static void api_map_unlock(void* host, mdpsr_ptr* p)   { RT(host)->map().unlock(p); }
static void* api_map_ptr_of(void* host, mdpsr_ptr* p)  { return RT(host)->map().ptr_of(p); }

static int api_map_attach(void* host, int kind, uint64_t key, const char* name,
                          void* ptr, mdpsr_ptr** out) {
    return RT(host)->map().attach(kind, key, name, ptr, out);
}
static int api_map_detach(void* host, uint64_t key) {
    /* 只允许在"已持有该条目的锁"时调用; 这里替调用方做不做检查 —— 契约如此。 */
    return RT(host)->map().detach(key);
}
static int api_map_invalidate(void* host, uint64_t key) {
    return RT(host)->map().invalidate(key);
}
static int api_map_enum_keys(void* host, uint64_t* out, uint32_t cap, uint32_t* out_count) {
    return RT(host)->map().enum_keys(out, cap, out_count);
}


/* ------------------------------ 插件装载 ------------------------------ */
static int api_plugin_load(void* host, const char* manifest_rel, uint64_t* out_plugin) {
    if (!manifest_rel || !*manifest_rel) return MDPSR_ERR_NULLPTR;
    return RT(host)->plugin_load(manifest_rel, out_plugin);
}
static int api_plugin_unload(void* host, uint64_t plugin) {
    return RT(host)->plugin_unload(plugin);
}
static int api_plugin_keys(void* host, uint64_t plugin, uint64_t* out,
                           uint32_t cap, uint32_t* out_count) {
    return RT(host)->plugin_keys(plugin, out, cap, out_count);
}
static int api_init_handle_of(void* host, uint64_t plugin, char* out, uint32_t cap) {
    return RT(host)->init_handle_of(plugin, out, cap);
}

/* ------------------------------ 消息 ------------------------------ */
static int api_emit(void* host, uint64_t handle_key, const void* data, size_t len) {
    return RT(host)->emit(handle_key, data, len);
}
static int api_queue_emit(void* host, uint64_t queue_key, uint64_t handle_key,
                          const void* data, size_t len) {
    return RT(host)->emit_to_queue(queue_key, handle_key, data, len);
}

/* ------------------------------ 队列 / 主线程 ------------------------------ */
static mdpsr_ptr* api_queue_get(void* host, uint64_t key) {
    return RT(host)->queue_ptr(key);
}
static int api_queue_list(void* host, uint64_t* out, uint32_t cap, uint32_t* out_count) {
    return RT(host)->queue_enum(out, cap, out_count);
}
static int api_queue_create(void* host, const char* name, const mdpsr_queue_desc* d,
                            uint64_t plugin, mdpsr_ptr** out) {
    return RT(host)->raw_queue_create(name, d, plugin, out);
}
static int api_queue_bind(void* host, uint64_t handle_key, uint64_t queue_key) {
    return RT(host)->bind_handle_queue(handle_key, queue_key);
}
static uint64_t api_queue_key_for_plugin(void* host, uint64_t plugin) {
    return RT(host)->queue_key_for_plugin(plugin);
}
static uint32_t api_main_thread_id(void* host) { return RT(host)->main_thread_id(); }

/* ------------------------------ 装配 ------------------------------ */
void install_host_api(Runtime* rt, mdpsr_host* api) {
    std::memset(api, 0, sizeof(*api));
    api->abi_version = MDPSR_ABI_VERSION;
    api->struct_size = static_cast<uint32_t>(sizeof(mdpsr_host));
    api->host        = rt;

    api->hash     = api_hash;
    api->log      = api_log;
    api->root_dir = api_root_dir;

    api->map_get       = api_map_get;
    api->map_find      = api_map_find;
    api->map_count     = api_map_count;
    api->map_is_valid  = api_map_is_valid;
    api->map_lock      = api_map_lock;
    api->map_try_lock  = api_map_try_lock;
    api->map_unlock    = api_map_unlock;
    api->map_ptr_of    = api_map_ptr_of;
    api->map_attach    = api_map_attach;
    api->map_detach    = api_map_detach;
    api->map_invalidate= api_map_invalidate;
    api->map_enum_keys = api_map_enum_keys;



    api->plugin_load   = api_plugin_load;
    api->plugin_unload = api_plugin_unload;
    api->plugin_keys   = api_plugin_keys;
    api->init_handle_of= api_init_handle_of;

    api->emit       = api_emit;
    api->queue_emit = api_queue_emit;

    api->queue_get  = api_queue_get;
    api->queue_list = api_queue_list;
    api->queue_create = api_queue_create;
    api->queue_bind = api_queue_bind;
    api->queue_key_for_plugin = api_queue_key_for_plugin;
    api->main_thread_id = api_main_thread_id;

    /* 逐字段核对: 漏一个就说明装配漏了, 让启动直接失败而不是留个空指针地雷。
     * (旧版掉过这个坑: api_manifest_entry 漏填 bind_queue, 排查了很久。) */
#define MDPSR_NEED(f) do { if (!api->f) { rt->log(2, "host api 装配缺失: " #f); std::memset(api, 0, sizeof(*api)); return; } } while (0)
    MDPSR_NEED(hash);
    MDPSR_NEED(log);
    MDPSR_NEED(root_dir);
    MDPSR_NEED(map_get);
    MDPSR_NEED(map_find);
    MDPSR_NEED(map_count);
    MDPSR_NEED(map_is_valid);
    MDPSR_NEED(map_lock);
    MDPSR_NEED(map_try_lock);
    MDPSR_NEED(map_unlock);
    MDPSR_NEED(map_ptr_of);
    MDPSR_NEED(map_attach);
    MDPSR_NEED(map_detach);
    MDPSR_NEED(map_invalidate);
    MDPSR_NEED(map_enum_keys);
    MDPSR_NEED(plugin_load);
    MDPSR_NEED(plugin_unload);
    MDPSR_NEED(plugin_keys);
    MDPSR_NEED(init_handle_of);
    MDPSR_NEED(emit);
    MDPSR_NEED(queue_emit);
    MDPSR_NEED(queue_get);
    MDPSR_NEED(queue_list);
    MDPSR_NEED(queue_create);
    MDPSR_NEED(queue_bind);
    MDPSR_NEED(queue_key_for_plugin);
    MDPSR_NEED(main_thread_id);
#undef MDPSR_NEED
}

} /* namespace mdpsr */
