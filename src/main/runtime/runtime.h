#pragma once
/* ============================================================================
 *  mdpsr/runtime/runtime.h
 *  Runtime —— 把注册表 / 队列 / 分发 / 装载 缝在一起
 *
 *  锁的层次 (永远从上往下拿, 不许反向):
 *
 *      1. _ctrl_mtx   装卸/重载串行化。持有它的时候可以做任何事,
 *                     但如果要调插件代码或 join 线程, 必须先确认没有别人
 *                     在等 _plg_mtx/_reg_mtx 里的某一把 —— 见 loader.cpp
 *                     顶部关于"为什么不会死锁"的说明。
 *      2. _plg_mtx    插件记录表的形状。短临界区, 绝不跨 join / 插件调用。
 *      3. _reg_mtx    (在 Registry 里) 注册表形状。短临界区。
 *      4. _q_mtx/_route_mtx/_log_mtx  各自独立的叶子锁。
 *      5. Entry::mtx  借用门闸。由 Registry 统一负责, 强制按键升序。
 *
 *  热路径 (dispatch_frame) 只碰 5 和 4, 不碰 1/2/3 的独占形态。
 * ==========================================================================*/

#include "mdpsr/abi.h"
#include "loader.h"
#include "queue.h"
#include "registry.h"

#include <atomic>
#include <cstdio>
#include <memory_resource>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mdpsr {

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    static uint32_t current_thread_id();

    int  init(const std::wstring& exe_dir);
    int  boot();                        /* 读 config.json -> 依次装载 */
    void shutdown();                     /* 停线程 + 反序卸载 + 汇总 */

    /* ---- 日志 / 路径 ---- */
    void log(int level, const std::string& msg);
    void set_log_file(const std::wstring& path);
    void set_log_level(int lv) { _log_level.store(lv, std::memory_order_relaxed); }
    void set_log_debug(bool on) { _log_debug.store(on, std::memory_order_relaxed); }

    std::wstring to_wide(const std::string& utf8) const;
    std::string  to_utf8(const std::wstring& w) const;
    std::wstring resolve(const std::string& rel) const;
    const std::string& root_utf8() const { return _root_utf8; }

    /* ---- 核心对象 ---- */
    Registry& reg() { return _reg; }
    mdpsr_host* api() { return &_api; }
    std::pmr::memory_resource* arena() { return _arena.get(); }
    uint32_t main_thread_id() const { return _main_tid; }

    /* ---- 分发 ---- */
    int dispatch_frame(Queue& q, const Frame& f);

    /* ---- 消息 ---- */
    int emit(uint64_t dst, uint64_t src, int32_t cmd, const void* body, uint32_t len);
    int emit_to(uint64_t queue_key, uint64_t dst, uint64_t src, int32_t cmd,
                const void* body, uint32_t len);
    int reply(const mdpsr_msg* req, int32_t cmd, const void* body, uint32_t len);

    /* ---- 队列 ----
     * _q_mtx 是 shared_mutex: 入队走共享锁 (查找+push 在同一把锁下完成,
     * 所以队列对象不可能在 push 中间被销毁), 摘除队列走独占锁。 */
    bool     queue_exists(uint64_t key);
    int      queue_create(const char* name, const Queue::Desc& d, Pool* pool, uint64_t owner,
                          uint64_t* out_key);
    int      queue_bind(uint64_t handle_key, uint64_t queue_key);
    uint64_t queue_of_handle(uint64_t handle_key);
    int      queue_list(uint64_t* out, uint32_t cap, uint32_t* out_count);
    int      queue_stats(uint64_t key, mdpsr_queue_stats* out);
    int      track_runtime_queue(uint64_t plugin, uint64_t queue_key);
    void     drop_queue(uint64_t queue_key);
    void     set_handle_route(uint64_t handle_key, uint64_t queue_key);
    void     forget_handle_routes(const std::vector<uint64_t>& keys);

    /* ---- 插件装卸 ---- */
    int plugin_install(const std::string& manifest_rel, const mdpsr_install_opts* opts,
                       uint64_t* out_plugin);
    int plugin_uninstall(uint64_t plugin, uint32_t flags);
    int plugin_reload(uint64_t plugin, const mdpsr_install_opts* opts);
    int plugin_find(const char* name, uint64_t* out_plugin, uint32_t* out_gen);
    int plugin_info(uint64_t plugin, mdpsr_plugin_info* out);
    int plugin_list(uint64_t* out, uint32_t cap, uint32_t* out_count);
    PluginRecord* record_of(uint64_t key);
    bool caller_is_kernel();

    /* 诊断: 把所有条目/队列的状态打一份出来 (运行卡住的时候用) */
    void watchdog_dump();

    /* ---- 汇总计数 (自测/排错用) ---- */
    std::atomic<uint64_t> t_emitted{ 0 };      /* 成功入队的消息 */
    std::atomic<uint64_t> t_dispatched{ 0 };   /* 真的调到了 handle */
    std::atomic<uint64_t> t_dead{ 0 };         /* 打到死 handle 的消息 */
    std::atomic<uint64_t> t_failed{ 0 };       /* handle 返回非 OK */
    std::atomic<uint64_t> t_queue_full{ 0 };   /* 因为队列满被拒 */
    std::atomic<uint64_t> t_dropped{ 0 };      /* 卸载时队列里被丢掉的 */
    std::atomic<uint64_t> t_busy_requeue{ 0 }; /* 因为"忙"被回滚重投的消息 */
    std::atomic<uint64_t> t_leaked{ 0 };       /* 插件漏还的锁 (宿主兜底还了) */
    std::atomic<uint64_t> t_manage{ 0 };       /* 装卸/重载次数 */

private:
    int   create_builtin_queues();
    void  stop_all_queues();
    int   run_init_handle(PluginRecord* rec, bool sync);
    std::string key_name(uint64_t key);

    std::unique_ptr<std::pmr::synchronized_pool_resource> _arena;
    Registry  _reg;
    mdpsr_host _api{};

    std::wstring _root;
    std::string  _root_utf8;
    uint32_t     _main_tid = 0;

    std::mutex   _log_mtx;
    std::FILE*   _log_file = nullptr;
    std::atomic<int>  _log_level{ 0 };
    std::atomic<bool> _log_debug{ false };

    std::shared_mutex   _q_mtx;
    std::vector<Queue*> _queues;

    std::mutex                             _route_mtx;
    std::unordered_map<uint64_t, uint64_t> _route;   /* handle 键 -> 队列键 */

    std::mutex                                 _plg_mtx;
    std::unordered_map<uint64_t, PluginRecord*> _records;
    std::vector<PluginRecord*>                 _load_order;

    std::mutex _ctrl_mtx;    /* 装卸/重载串行化: 持有它 = 独占整个管理面 */
};

void install_host_api(Runtime* rt, mdpsr_host* api);

} /* namespace mdpsr */
