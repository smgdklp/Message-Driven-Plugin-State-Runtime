#pragma once
/* ============================================================================
 *  mdpsr/runtime/runtime.h
 *  宿主的全部内部结构 —— MainMap / Queue / Runtime
 *
 *  设计要点 (对应收敛后的规格):
 *
 *    · MainMap: 一张大表 uint64_t -> Ptr, 外加四个按类型拆出的二级字典
 *      (object / state / handle / queue)。二级字典的值就是大表里的 Ptr*,
 *      所以"查表"是指针查询, 不产生拷贝, 也不需要生命周期跟踪。
 *
 *    · Ptr.valid 是门闸, Ptr.mtx 是工具。卸载的顺序固定为:
 *          先 map_attach 把全部资源挂上 -> 卸载时先 invalidate(标不有效)
 *          -> 再逐个 try_lock 拿到锁 (拿不到说明有人正在用, 等下一条重试)
 *          -> 释放资源。
 *
 *    · 每条 Queue 自带一条线程和一个限速计时器: 消息之间的间隔不小于
 *      pace_ms, 消息忙的时候也不空转 (用它挡住"拿队列当 while 用"的写法)。
 * ==========================================================================*/

#include "mdpsr/abi.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


#include "loader.h"

namespace mdpsr {

class Runtime;

/* ==========================================================================
 *  Ptr 的锁 —— 契约只给了定长存储, 真身由宿主决定
 * ==========================================================================*/
inline std::mutex* ptr_mtx(mdpsr_ptr* p) {
    return reinterpret_cast<std::mutex*>(p->_mtx_storage);
}
inline const std::mutex* ptr_mtx(const mdpsr_ptr* p) {
    return reinterpret_cast<const std::mutex*>(p->_mtx_storage);
}

/* ==========================================================================
 *  MainMap —— 一张大表 + 四个二级字典
 * ==========================================================================*/
class MainMap {
public:
    explicit MainMap(std::pmr::memory_resource* res);
    ~MainMap();
    MainMap(const MainMap&) = delete;
    MainMap& operator=(const MainMap&) = delete;

    /* ---- 大表 ---- */

    /* 查一个有效条目。valid == false 一律当作不存在, 返回 nullptr。 */
    mdpsr_ptr* get(uint64_t key);

    /* 挂一个条目 (kind 见 enum mdpsr_kind)。它同时决定进哪张二级字典。
     * 键已存在时: 旧条目已 invalid 则替换, 否则返回 ALREADY_EXISTS。 */
    int attach(int kind, uint64_t key, const char* name, void* ptr, mdpsr_ptr** out);

    /* 把一个条目从表里摘掉。要求调用方已经持有它的锁 ——
     * "拿到锁"就等于"没人在用", 所以摘表是安全的。 */
    int detach(uint64_t key);

    /* 只置 valid = false, 不摘表。这是"卸载第一步":
     * 它挡住所有新的 get(), 已经在手上的调用方不受影响。 */
    int invalidate(uint64_t key);

    /* 原子读一下 valid (不持锁) */
    int  is_valid(mdpsr_ptr* p);
    void* ptr_of(mdpsr_ptr* p) { return p ? p->ptr : nullptr; }

    /* Ptr.mtx —— "我在用这个资源"的唯一声明方式 */
    int  lock(mdpsr_ptr* p);
    int  try_lock(mdpsr_ptr* p);
    void unlock(mdpsr_ptr* p);

    /* 枚举全部键的快照 (键只增不减, 所以拿到就能用) */
    int  enum_keys(uint64_t* out, uint32_t cap, uint32_t* out_count);
    size_t size();

    /* 诊断用: 条目的名字副本 (池内, 生命周期与条目一致) */
    const char* name_of(uint64_t key);

    /* ---- 二级字典 ----
     * 元素类型统一是 mdpsr_ptr*, 供 Context 直接给插件查表。 */
    HashMap* table_object() { return &_object; }
    HashMap* table_state() { return &_state; }
    HashMap* table_handle() { return &_handle; }
    HashMap* table_queue() { return &_queue; }

    /* 按 kind 去选二级字典; kind 非法返回 nullptr */
    HashMap* table_of(int kind);

    /* 这个键属于哪张二级字典的条目 (不看 valid, 卸载路径要用) */
    int kind_of(uint64_t key);

    /* 供 dispatch 组装 Context 用: 按 key 列表抓一批 Ptr 塞进临时字典 */
    void gather(const uint64_t* keys, size_t n, HashMap* out);

    /* 锁序工具: 一次要拿多把锁时, 必须按键升序取, 否则就会死锁。
     * dispatch 和 unload 都用它, 走的是同一个顺序。 */
    static void sort_keys(uint64_t* keys, size_t n);

    /* 不走 valid 门闸地拿 Ptr (给卸载路径用): 卸载时我们已经 invalidate 了,
     * 但还需要那些 Ptr 来取锁和摘表。 */
    mdpsr_ptr* peek(uint64_t key);
    /* 摘表。要求调用方已持有该条目的锁。 */
    int detach_locked(uint64_t key);

private:
    mdpsr_ptr* alloc_ptr();
    void       free_ptr(mdpsr_ptr* p);

    std::pmr::memory_resource* _res;
    std::vector<mdpsr_ptr*>    _all;      /* 全部 Ptr, 只增不减 */

    mutable std::shared_mutex  _mtx;      /* 保护 _tbl / _kind / _names */
    HashMap                    _tbl;      /* uint64 -> mdpsr_ptr* */
    HashMap                    _names;    /* uint64 -> const char* (仅诊断) */
    std::unordered_map<uint64_t, int> _kind;   /* 条目类型 */

    HashMap                    _object;
    HashMap                    _state;
    HashMap                    _handle;
    HashMap                    _queue;
};

/* ==========================================================================
 *  handle 描述符 —— 它是"托管资源", 所以进 Pool_object, 由 Ptr 托管
 * ==========================================================================*/
struct mdpsr_handle_desc {
    mdpsr_handle_fn fn = nullptr;
    uint64_t        plugin = 0;      /* 属于哪个插件 */
    char            name[MDPSR_NAME_MAX] = { 0 };
};

/* 插件的键: 用 "plugin:<插件名>" 再哈希一次, 避免和它的 handle 撞键 */
inline uint64_t plugin_key(const std::string& plugin_name) {
    return mdpsr_hash64(("plugin:" + plugin_name).c_str());
}

/* ==========================================================================
 *  SharedMaps —— Context 里给插件直接查表的那几张字典 (每个线程一份)
 * ==========================================================================*/
struct SharedMaps {
    HashMap object;
    HashMap state;
    HashMap handle;
    HashMap queue;
    void clear() { object.clear(); state.clear(); handle.clear(); queue.clear(); }
};

/* ==========================================================================
 *  MultiLock —— 一次持有多个 Ptr 的锁
 *
 *  规则: **按键升序取锁**。dispatch 与 unload 都走这个顺序, 所以两边
 *  永远不会互相咬住。取不到任何一把就整批回滚 (要么全拿到, 要么一把不拿),
 *  这样调用方不需要处理"半个资源"的状态。
 * ==========================================================================*/
class MultiLock {
public:
    /* 一条待锁的条目。卸载路径必须用它 —— 那时条目已经 invalid,
     * 没法再通过 key 查 Ptr 了。 */
    struct Item { uint64_t key; mdpsr_ptr* p; };

    MultiLock() = default;
    ~MultiLock() { release(); }
    MultiLock(const MultiLock&) = delete;
    MultiLock& operator=(const MultiLock&) = delete;

    /* 分发路径: 按键列表取锁。跳过"根本没挂上"的键;
     * 拿到锁后发现条目已被置无效 -> 返回 MDPSR_ERR_MAP_INVALID。
     * 返回 MDPSR_OK / MDPSR_ERR_BUSY / MDPSR_ERR_MAP_INVALID */
    int acquire(MainMap& m, const uint64_t* keys, size_t n);

    /* 卸载路径: 直接对着已经查好的 (key, Ptr) 取锁。
     * 不看 valid —— 卸载方本来就是要把无效的东西收掉。 */
    int acquire_raw(MainMap& m, std::vector<Item> items);

    void release();

    size_t count() const { return _held.size(); }
    bool   held(uint64_t key) const;

private:
    MainMap*                _m = nullptr;
    std::vector<Item>       _order;   /* 按键升序 —— 取锁顺序的唯一依据 */
    std::vector<mdpsr_ptr*> _held;
};

/* ==========================================================================
 *  Queue —— 一条环形字节缓冲 + 一条自己的线程 + 限速计时器
 * ==========================================================================*/
class Queue {
public:
    struct Desc {
        uint32_t capacity = 4096;
        uint32_t pace_ms  = 0;
    };

    Queue(std::pmr::memory_resource* pool, std::string name, uint64_t key, const Desc& d);
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    uint64_t key() const { return _key; }
    const std::string& name() const { return _name; }
    uint32_t pace_ms() const { return _pace_ms; }

    /* 自己在 MainMap 里的条目 —— 队列也是托管资源 */
    void       attach_self(mdpsr_ptr* p) { _self_ptr = p; }
    mdpsr_ptr* self_ptr() const { return _self_ptr; }

    /* 组一帧入队 (线程安全, 多生产者) */
    void push(uint64_t handle, int32_t cmd, const void* body, size_t len);

    /* 取一条完整消息。body 指向内部缓冲, 仅在下次 pop 前有效。 */
    bool pop(uint64_t* handle, int32_t* cmd, const uint8_t** body, size_t* len);

    /* 扔掉队列里剩下的消息 (停止时用) */
    void drain();

    size_t readable() const;

    /* 自己那条线程 */
    void start(Runtime* rt, bool main_thread);
    void request_stop();
    void join();
    bool running() const { return _running.load(std::memory_order_acquire); }
    bool main_thread() const { return _main_thread; }
    std::thread::id thread_id() const { return _tid; }
    uint32_t main_tid_snapshot() const { return _main_tid; }

    /* 挂主线程的队列由宿主主循环反复调这个 (处理一条就返回, 最多等 wait_ms) */
    int pump_once(Runtime* rt, uint32_t wait_ms);

    uint64_t pushed() const;
    uint64_t popped() const;
    int64_t  dispatch_errors() const { return _errors.load(std::memory_order_relaxed); }

private:
    void run(Runtime* rt);
    void ensure(size_t need);
    void write_bytes(const uint8_t* p, size_t n);
    void read_bytes(uint8_t* p, size_t n);
    void skip_bytes(size_t n);
    uint8_t peek_at(size_t offset) const;

    mutable std::mutex        _mtx;
    std::condition_variable   _cv;
    std::pmr::vector<uint8_t> _buf;
    std::pmr::vector<uint8_t> _scratch;
    size_t                    _head = 0;
    size_t                    _size = 0;
    uint64_t                  _pushed = 0;
    uint64_t                  _popped = 0;

    std::string   _name;
    uint64_t      _key = 0;
    uint32_t      _pace_ms = 0;
    mdpsr_ptr*    _self = nullptr;     /* 自己在 MainMap 里的条目 */

    std::thread       _th;
    std::atomic<bool> _stop{ false };
    std::atomic<bool> _running{ false };
    bool              _main_thread = false;
    std::thread::id   _tid{};
    uint32_t          _main_tid = 0;
    /* 挂主线程的队列用它做限速 (不能用 sleep) */
    std::chrono::steady_clock::time_point _next_allowed{};
    mdpsr_ptr*        _self_ptr = nullptr;

    std::atomic<uint64_t> _total_paced{ 0 };   /* 被限速挡下的次数 (诊断) */
    std::atomic<int64_t>  _errors{ 0 };
};

/* ==========================================================================
 *  Runtime —— 池 / MainMap / 队列 / 装载 / 分发
 * ==========================================================================*/
class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    int  init(const std::wstring& exe_dir);
    int  boot();                       /* 读 config.json -> 装载 -> 投 cmd=0 */
    void pump_main(uint32_t wait_ms);  /* 主线程专用: 驱动 Queue_windowsgui */
    void shutdown();                   /* 停线程 + 卸全部插件 */

    /* ---- 日志 ---- */
    void log(int level, const std::string& msg);
    void set_log_file(const std::wstring& path);
    void set_log_level(int lv) { _log_level.store(lv, std::memory_order_relaxed); }

    /* ---- 路径 ---- */
    std::wstring to_wide(const std::string& utf8) const;
    std::string  to_utf8(const std::wstring& w) const;
    std::wstring resolve(const std::string& rel) const;
    const std::string& root_utf8() const { return _root_utf8; }

    /* ---- 池 ---- */
    std::pmr::memory_resource* pool_msg()   { return _pool_msg.get(); }
    std::pmr::memory_resource* pool_map()   { return _pool_map.get(); }
    std::pmr::memory_resource* pool_state() { return _pool_state.get(); }
    std::pmr::memory_resource* pool_object(){ return _pool_object.get(); }

    /* ---- 核心对象 ---- */
    MainMap& map() { return _map; }
    mdpsr_host* api() { return &_api; }

    /* ---- 队列 ---- */
    Queue* queue_of(uint64_t key);
    mdpsr_ptr* queue_ptr(uint64_t key);
    /* 插件的 handle 该跑在哪条队列上: 它声明的第一条存在的队列;
     * 没声明 / 都找不到 -> Queue_default */
    Queue* queue_for_plugin(uint64_t plugin_key);
    /* 插件默认去的那条队列的键 (没声明就是 Queue_default 的键) */
    uint64_t queue_key_for_plugin(uint64_t plugin_key);
    /* handle 到底归哪条队列: 0 表示还没绑 -> 用它所属插件的默认队列 */
    uint64_t queue_of_handle(uint64_t handle_key);
    int      bind_handle_queue(uint64_t handle_key, uint64_t queue_key);
    bool     handle_on_pluginmgr(uint64_t handle_key);
    int    queue_create(const std::string& name, const Queue::Desc& d,
                        uint64_t plugin, bool main_thread, Queue** out);
    int    queue_enum(uint64_t* out, uint32_t cap, uint32_t* out_count);
    int    emit(uint64_t handle_key, const void* body, size_t len);
    int    emit_to_queue(uint64_t queue_key, uint64_t handle_key,
                         const void* body, size_t len);

    /* ---- 分发 ---- */
    int    dispatch_one(Queue* q, uint64_t handle_key, int32_t cmd, const uint8_t* body, size_t len);

    /* ---- 装载 ---- */
    int    plugin_load(const std::string& manifest_rel, uint64_t* out_plugin);
    int    plugin_unload(uint64_t plugin);
    int    plugin_keys(uint64_t plugin, uint64_t* out, uint32_t cap, uint32_t* out_count);
    /* 插件声明的 Init handle 名字 (没有就填空串) */
    int    init_handle_of(uint64_t plugin, char* out, uint32_t cap);
    /* ---- 访问器 ---- */
    uint32_t main_thread_id() const { return _main_tid; }
    std::mutex& plugin_mtx() { return _plugin_mtx; }

    std::atomic<uint64_t> emitted_total{ 0 };
    std::atomic<uint64_t> dispatched_total{ 0 };
    std::atomic<uint64_t> dispatch_errors{ 0 };
    std::atomic<uint64_t> emitted_paced{ 0 };

    /* 给 host_api.cpp 用的内部操作 */
    int    raw_queue_create(const char* name, const mdpsr_queue_desc* d,
                            uint64_t plugin, mdpsr_ptr** out);
    void*  object_alloc(size_t bytes, size_t align);
    void   object_free(void* p, size_t bytes, size_t align);

private:
    void start_queue_threads();
    void stop_queue_threads();
    PluginInfo* find_plugin(uint64_t key);

    /* Context 组装: 每个线程一份可复用的字典, 避免每次分发都分配 */
    SharedMaps* local_maps();
    void build_ctx(const Manifest& m, uint64_t handle_key, SharedMaps* sm, mdpsr_context* out);

    static thread_local SharedMaps* t_maps;

    std::unique_ptr<std::pmr::synchronized_pool_resource> _pool_msg;
    std::unique_ptr<std::pmr::synchronized_pool_resource> _pool_map;
    std::unique_ptr<std::pmr::synchronized_pool_resource> _pool_state;
    std::unique_ptr<std::pmr::synchronized_pool_resource> _pool_object;

    MainMap   _map;
    mdpsr_host _api{};

    std::wstring _root;
    std::string  _root_utf8;
    uint32_t     _main_tid = 0;

    std::mutex                _log_mtx;
    std::FILE*                _log_file = nullptr;
    std::atomic<int>          _log_level{ 0 };

    mutable std::mutex        _q_mtx;
    std::vector<Queue*>       _queues;
    uint64_t                  _main_queue_key = 0;

    std::mutex                _plugin_mtx;
    std::vector<PluginInfo*>  _plugins;     /* 只增不减; 卸载只做资源的失效与释放 */
    /* handle 键 -> 所属插件键。emit 和 dispatch 都要靠它快速找到"这个 handle
     * 属于谁", 从而知道该投哪条队列、该锁哪些资源。 */
    std::unordered_map<uint64_t, uint64_t> _handle_owner;
    /* handle 键 -> 显式绑定的队列键 (0 = 没绑, 用它所属插件的默认队列) */
    std::unordered_map<uint64_t, uint64_t> _handle_queue;
};

void install_host_api(Runtime* rt, mdpsr_host* api);

} /* namespace mdpsr */
