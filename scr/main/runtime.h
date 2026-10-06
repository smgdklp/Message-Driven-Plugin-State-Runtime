#pragma once
/* ============================================================================
 *  mdpsr/scr/main/runtime.h
 *  宿主 Runtime: 路径 / 清单 / 模块 / 分流队列 / 分发 / 引导
 *
 *  多线程分流之后, 宿主主线程不再自己泵消息:
 *  每个分流队列都有一条常驻线程 (Pool_thread) 跑着它自己的 dispatcher
 *  (Pool_dispatcher 里的对象), 一个 dispatcher 只吃自己那一个队列。
 * ==========================================================================*/

#include "dispatcher.h"
#include "queue_map.h"

#include <mdpsr/abi.h>
#include <mdpsr/json_small.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace mdpsr {

/* ==========================================================================
 *  清单
 * ==========================================================================*/
struct ManifestEntry {
    std::string              name;
    std::string              symbol;
    std::string              type;      /* handle | handle_core */
    int                      type_id = MDPSR_ENTRY_HANDLE;
    std::string              param;     /* param 是字符串时 */
    std::vector<std::string> list;      /* param 是数组时 */
    std::string              bind_object;
    std::string              bind_queue;    /* handle: 绑定的分流队列名 */
    std::vector<std::string> bind_states;
    uint32_t                 capacity = 0;  /* queue: 初始缓冲字节 */
};

struct Manifest {
    std::string                rel_path;
    std::string                dir;
    std::vector<std::string>   dlls;     /* 相对根目录 */
    std::vector<ManifestEntry> queues;   /* 组件申请的分流队列 */
    std::vector<ManifestEntry> handles;
    std::vector<ManifestEntry> objects;
    std::vector<ManifestEntry> states;
};

struct ModuleInfo {
    uint64_t              key = 0;
    std::string           rel_path;
    std::wstring          abs_path;
    std::vector<HMODULE>  mods;      /* 一个清单可以带多个 dll */
    bool                  ready = false;
};

/* 一个分流队列的完整槽位: Map 描述符 + 队列本体 + 它的 dispatcher + 线程 */
struct QueueSlot {
    uint64_t     key = 0;
    mdpsr_queue* info = nullptr;     /* Map 里的条目载荷 (Pool_map) */
    Queue*       queue = nullptr;    /* 环形缓冲本体 (Pool_msg) */
    Dispatcher*  disp = nullptr;     /* Pool_dispatcher 里的对象 */
    void*        thread = nullptr;   /* Pool_thread 里的线程句柄 */
    bool         core = false;       /* 是不是 queue_core */
};

/* ==========================================================================
 *  宿主
 * ==========================================================================*/
class Runtime {
public:
    Runtime();
    ~Runtime();

    int init(const std::wstring& exe_dir);

    /* ---- 日志 ---- */
    void log(int level, const std::string& msg);
    void set_log_file(const std::wstring& path);
    /* 低于这个级别的日志直接丢掉 (0=全部, 1=只留 WARN 以上, 2=只留 ERR) */
    void set_log_level(int lv) { _log_level.store(lv, std::memory_order_relaxed); }
    int  log_level() const { return _log_level.load(std::memory_order_relaxed); }

    /* ---- 路径 ---- */
    std::wstring to_wide(const std::string& utf8) const;
    std::string  to_utf8(const std::wstring& w) const;
    std::wstring resolve(const std::string& rel) const;
    const std::string& root_utf8() const { return _root_utf8; }

    /* ---- 清单 ---- */
    int             load_manifest(const std::string& rel_json, uint64_t* out_dll);
    const Manifest* manifest(uint64_t dll) const;

    /* ---- 模块 ---- */
    int  module_load(uint64_t dll, const std::string& rel_path, void** out_module);
    int  module_ready(void* mod);
    int  module_unload(uint64_t dll);
    bool module_loaded(uint64_t dll) const;
    std::vector<HMODULE> module_handles(uint64_t dll) const;

    /* ---- DLL 专属池 ---- */
    mdpsr_dllptr* dllptr(uint64_t dll);
    int           dll_pool_create(uint64_t dll, const char* name, mdpsr_dllptr* out);

    /* ---- 插件装载 ---- */
    int  plugin_load(const std::string& manifest_rel, uint64_t* out_dll);
    int  plugin_unload(uint64_t dll);
    int  plugin_keys(uint64_t dll, uint64_t* out, uint32_t cap, uint32_t* out_count);

    /* ---- 分流队列 ---- */
    int          queue_create(const std::string& name, uint32_t capacity,
                              uint64_t dll, bool core, mdpsr_queue** out);
    int          queue_destroy(mdpsr_queue* q);
    int          queue_bind(uint64_t handle_key, mdpsr_queue* q);
    int          queue_bind_key(uint64_t handle_key, uint64_t queue_key);
    int          queue_emit(mdpsr_queue* q, uint64_t handle_key,
                            const void* body, size_t len);
    mdpsr_queue* queue_find(uint64_t key);
    mdpsr_queue* queue_core();
    int          queue_list(mdpsr_queue** out, uint32_t cap, uint32_t* out_count);
    size_t       dispatcher_count() const;
    Queue*       queue_impl(mdpsr_queue* q) const;

    /* ---- 消息 ---- */
    int emit(uint64_t handle_key, const void* body, size_t len);

    /* ---- 分发 (由 dispatcher 线程调用) ---- */
    int dispatch_message(Dispatcher* d, uint64_t handle_key,
                         const uint8_t* body, size_t len);

    /* ---- 引导 / 收尾 ---- */
    int  boot(uint32_t* out_plugins, std::string* out_init_cmd, int* out_backdoor_rc);
    int  call_backdoor(const std::string& tag);
    int  request_clear(uint32_t timeout_ms);
    int  unload_all();
    void stop_dispatchers();

    /* ---- 访问器 ---- */
    Pools&            pools() { return _pools; }
    Queue*            core_queue_impl() { return queue_impl(queue_core()); }
    Map&              map()   { return _map; }
    const mdpsr_host* api() const { return &_api; }

    /* 这几个计数会被多条分发线程 / 生产者线程同时改, 必须是原子的 */
    std::atomic<uint64_t> emitted_total{ 0 };
    std::atomic<uint64_t> dispatched_total{ 0 };
    std::atomic<uint64_t> dispatch_errors{ 0 };
    std::atomic<uint64_t> fallback_total{ 0 };   /* 回退到 queue_core 的次数 */

    std::mutex& dll_mtx() { return _dll_mtx; }

private:
    void  release_slot(QueueSlot& slot);      /* 释放槽位资源 (不含 join) */
    int   raw_emit(Queue* q, uint64_t handle_key, const void* body, size_t len);

    Pools          _pools;
    Map            _map;
    mdpsr_host     _api{};

    ThreadPool     _threads;        /* Pool_thread */
    DispatcherPool _dispatchers;    /* Pool_dispatcher */

    std::wstring _root;
    std::string  _root_utf8;

    std::mutex _log_mtx;
    std::FILE* _log_file = nullptr;
    std::atomic<int> _log_level{ 0 };

    mutable std::mutex                          _dll_mtx;
    std::unordered_map<uint64_t, ModuleInfo>    _modules;
    std::unordered_map<uint64_t, Manifest>      _manifests;
    std::unordered_map<uint64_t, mdpsr_dllptr*> _dllptrs;
    std::unordered_map<uint64_t, std::vector<uint64_t>> _keys;

    mutable std::mutex                      _queue_mtx;
    std::unordered_map<uint64_t, QueueSlot> _queues;
    uint64_t                                _core_queue_key = 0;

    /* 串行化插件装载 / 卸载 (多 dispatcher 线程下必须) */
    std::mutex _plugin_mtx;
};

/* host_api.cpp 提供 */
void install_host_api(Runtime* rt, mdpsr_host* api);

} /* namespace mdpsr */
