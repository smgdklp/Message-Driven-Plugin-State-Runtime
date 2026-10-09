/* ============================================================================
 *  mdpsr/runtime/host_api.cpp
 *  把 Runtime 的能力装配成 mdpsr_host 函数表。
 *
 *  铁律: abi.h 里声明的每一个槽位都必须在这里赋值, 装完逐字段核对,
 *  漏一个就 return (api 全零) —— 宁可启动失败, 也不要给插件留个空指针地雷。
 * ==========================================================================*/
#include "runtime.h"
#include "registry.h"

#include <cstring>
#include <windows.h>

namespace mdpsr {

static inline Runtime* RT(void* self) { return static_cast<Runtime*>(self); }

/* ------------------------------ 基础 ------------------------------ */
static uint64_t api_hash(const char* name) { return mdpsr_hash64(name); }
static void api_log(void* self, int level, const char* msg) {
    RT(self)->log(level, msg ? msg : "");
}
static const char* api_root_dir(void* self) { return RT(self)->root_utf8().c_str(); }
static uint64_t api_millis(void* self) { (void)self; return static_cast<uint64_t>(::GetTickCount64()); }
static uint32_t api_thread_id(void* self) { (void)self; return Runtime::current_thread_id(); }
static uint32_t api_main_thread_id(void* self) { return RT(self)->main_thread_id(); }
static void api_sleep_ms(void* self, uint32_t ms) { (void)self; ::Sleep(ms); }

/* ------------------------------ 注册表 ------------------------------ */
static int api_acquire(void* self, uint64_t key, uint32_t gen, void** p, uint32_t* og) {
    return RT(self)->reg().acquire(key, gen, p, og);
}
static int api_acquire_many(void* self, const uint64_t* keys, const uint32_t* gens,
                            uint32_t n, void** ptrs, uint32_t* ogs) {
    return RT(self)->reg().acquire_many(keys, gens, n, ptrs, ogs);
}
static void api_release(void* self, uint64_t key) { RT(self)->reg().release(key); }
static void api_release_all(void* self) { RT(self)->reg().release_all(); }
static int  api_held_count(void* self) { return RT(self)->reg().held_count(); }
static uint32_t api_gen_of(void* self, uint64_t key) { return RT(self)->reg().gen_of(key); }
static int  api_kind_of(void* self, uint64_t key) { return RT(self)->reg().kind_of(key); }
static int  api_name_of(void* self, uint64_t key, char* out, uint32_t cap) {
    return RT(self)->reg().name_of(key, out, cap);
}

/* ------------------------------ 内存 ------------------------------ */
static void* api_pool_alloc(void* self, mdpsr_pool* pool, size_t bytes, size_t align) {
    Pool* p = reinterpret_cast<Pool*>(pool);
    if (!p || bytes == 0) return nullptr;
    if (align == 0) align = alignof(void*);
    const uint64_t cp = tls().cur_plugin;
    if (cp != 0 && p->owner != cp) {
        RT(self)->log(1, "pool_alloc 被拒: 想用别的插件的池");
        return nullptr;
    }
    return RT(self)->reg().pool_alloc(p, bytes, align);
}
static void api_pool_free(void* self, mdpsr_pool* pool, void* q, size_t bytes, size_t align) {
    Pool* p = reinterpret_cast<Pool*>(pool);
    if (!p || !q) return;
    if (align == 0) align = alignof(void*);
    /* 和 alloc 对称: 还到别人的池里会破坏那个池的自由链 (它记的 bytes/align 对不上),
     * 所以这里也要拦 —— 不能只有"借"的那一半被保护。 */
    const uint64_t cp = tls().cur_plugin;
    if (cp != 0 && p->owner != cp) {
        RT(self)->log(1, "pool_free 被拒: 想还到别的插件的池里");
        return;
    }
    RT(self)->reg().pool_free(p, q, bytes, align);
}

/* ------------------------------ 消息 ------------------------------ */
static int api_emit(void* self, uint64_t dst, int32_t cmd, const void* body, uint32_t len) {
    return RT(self)->emit(dst, 0, cmd, body, len);
}
static int api_emit_from(void* self, uint64_t dst, uint64_t src, int32_t cmd,
                         const void* body, uint32_t len) {
    return RT(self)->emit(dst, src, cmd, body, len);
}
static int api_emit_to(void* self, uint64_t q, uint64_t dst, uint64_t src, int32_t cmd,
                       const void* body, uint32_t len) {
    return RT(self)->emit_to(q, dst, src, cmd, body, len);
}
static int api_reply(void* self, const mdpsr_msg* req, int32_t cmd,
                     const void* body, uint32_t len) {
    return RT(self)->reply(req, cmd, body, len);
}

/* ------------------------------ 队列 ------------------------------ */
static int api_queue_create(void* self, const char* name, const mdpsr_queue_desc* d,
                            uint64_t* out_key) {
    Runtime* rt = RT(self);
    const uint64_t cp = tls().cur_plugin;
    if (cp == 0) return MDPSR_ERR_NO_PERM;        /* 宿主走内部接口 */
    PluginRecord* rec = rt->record_of(cp);
    if (!rec || !rec->pool) return MDPSR_ERR_NOT_READY;

    Queue::Desc qd;
    if (d && d->struct_size >= sizeof(mdpsr_queue_desc)) {
        qd.capacity = d->capacity;
        qd.pace_ms  = d->pace_ms;
    }
    uint64_t qk = 0;
    int rc = rt->queue_create(name, qd, rec->pool, cp, &qk);
    if (rc != MDPSR_OK) return rc;
    /* 登记到本插件名下 —— 不登记的话卸载时收不回来 (线程会活过 FreeLibrary) */
    rc = rt->track_runtime_queue(cp, qk);
    if (rc != MDPSR_OK) {
        rt->drop_queue(qk);
        rt->reg().detach(qk);
        return rc;
    }
    if (out_key) *out_key = qk;
    return MDPSR_OK;
}
static int api_queue_bind(void* self, uint64_t handle_key, uint64_t queue_key) {
    return RT(self)->queue_bind(handle_key, queue_key);
}
static uint64_t api_queue_of(void* self, uint64_t handle_key) {
    return RT(self)->queue_of_handle(handle_key);
}
static int api_queue_stats(void* self, uint64_t key, mdpsr_queue_stats* out) {
    if (!out) return MDPSR_ERR_NULLPTR;
    return RT(self)->queue_stats(key, out);
}
static int api_queue_list(void* self, uint64_t* out, uint32_t cap, uint32_t* n) {
    return RT(self)->queue_list(out, cap, n);
}

/* ------------------------------ 插件 ------------------------------ */
static int api_plugin_install(void* self, const char* rel, const mdpsr_install_opts* o,
                              uint64_t* out) {
    if (!rel || !*rel) return MDPSR_ERR_NULLPTR;
    return RT(self)->plugin_install(rel, o, out);
}
static int api_plugin_uninstall(void* self, uint64_t plugin, uint32_t flags) {
    return RT(self)->plugin_uninstall(plugin, flags);
}
static int api_plugin_reload(void* self, uint64_t plugin, const mdpsr_install_opts* o) {
    return RT(self)->plugin_reload(plugin, o);
}
static int api_plugin_find(void* self, const char* name, uint64_t* out, uint32_t* gen) {
    return RT(self)->plugin_find(name, out, gen);
}
static int api_plugin_info(void* self, uint64_t plugin, mdpsr_plugin_info* out) {
    return RT(self)->plugin_info(plugin, out);
}
static int api_plugin_list(void* self, uint64_t* out, uint32_t cap, uint32_t* n) {
    return RT(self)->plugin_list(out, cap, n);
}
static uint64_t api_current_plugin(void* self) { (void)self; return tls().cur_plugin; }

/* ------------------------------ 装配 ------------------------------ */
void install_host_api(Runtime* rt, mdpsr_host* api) {
    std::memset(api, 0, sizeof(*api));
    api->abi_version = MDPSR_ABI_VERSION;
    api->struct_size = static_cast<uint32_t>(sizeof(mdpsr_host));
    api->self        = rt;

    api->hash            = api_hash;
    api->log             = api_log;
    api->root_dir        = api_root_dir;
    api->millis          = api_millis;
    api->thread_id       = api_thread_id;
    api->main_thread_id  = api_main_thread_id;
    api->sleep_ms        = api_sleep_ms;

    api->acquire         = api_acquire;
    api->acquire_many    = api_acquire_many;
    api->release         = api_release;
    api->release_all     = api_release_all;
    api->held_count      = api_held_count;
    api->gen_of          = api_gen_of;
    api->kind_of         = api_kind_of;
    api->name_of         = api_name_of;

    api->pool_alloc      = api_pool_alloc;
    api->pool_free       = api_pool_free;

    api->emit            = api_emit;
    api->emit_from       = api_emit_from;
    api->emit_to         = api_emit_to;
    api->reply           = api_reply;

    api->queue_create    = api_queue_create;
    api->queue_bind      = api_queue_bind;
    api->queue_of        = api_queue_of;
    api->queue_stats     = api_queue_stats;
    api->queue_list      = api_queue_list;

    api->plugin_install  = api_plugin_install;
    api->plugin_uninstall= api_plugin_uninstall;
    api->plugin_reload   = api_plugin_reload;
    api->plugin_find     = api_plugin_find;
    api->plugin_info     = api_plugin_info;
    api->plugin_list     = api_plugin_list;
    api->current_plugin  = api_current_plugin;

    /* 逐字段核对: 漏一个就说明装配漏了, 让启动直接失败而不是留个空指针地雷。 */
#define MDPSR_NEED(f) do { if (!api->f) { rt->log(2, "host api 装配缺失: " #f); \
        std::memset(api, 0, sizeof(*api)); return; } } while (0)
    MDPSR_NEED(hash);            MDPSR_NEED(log);              MDPSR_NEED(root_dir);
    MDPSR_NEED(millis);          MDPSR_NEED(thread_id);        MDPSR_NEED(main_thread_id);
    MDPSR_NEED(sleep_ms);
    MDPSR_NEED(acquire);         MDPSR_NEED(acquire_many);     MDPSR_NEED(release);
    MDPSR_NEED(release_all);     MDPSR_NEED(held_count);       MDPSR_NEED(gen_of);
    MDPSR_NEED(kind_of);         MDPSR_NEED(name_of);
    MDPSR_NEED(pool_alloc);      MDPSR_NEED(pool_free);
    MDPSR_NEED(emit);            MDPSR_NEED(emit_from);        MDPSR_NEED(emit_to);
    MDPSR_NEED(reply);
    MDPSR_NEED(queue_create);    MDPSR_NEED(queue_bind);       MDPSR_NEED(queue_of);
    MDPSR_NEED(queue_stats);     MDPSR_NEED(queue_list);
    MDPSR_NEED(plugin_install);  MDPSR_NEED(plugin_uninstall); MDPSR_NEED(plugin_reload);
    MDPSR_NEED(plugin_find);     MDPSR_NEED(plugin_info);      MDPSR_NEED(plugin_list);
    MDPSR_NEED(current_plugin);
#undef MDPSR_NEED
}

} /* namespace mdpsr */
