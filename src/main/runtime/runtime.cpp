/* ============================================================================
 *  mdpsr/runtime/runtime.cpp
 *  Runtime: 池 / MainMap / 三条队列 / 分发 / 装载
 *
 *  卸载的顺序 (与 dispatch 共用同一套锁序, 所以两边永不互咬):
 *
 *    1. 置 unloading, 把插件名下的**全部条目** invalidate (挡住所有新的 get)
 *    2. 按键升序 try_lock 全部条目
 *         · 有一把拿不到 -> 说明有人正在用 -> 整个卸载回滚, 返回 BUSY,
 *           core_Mgr 会把它回滚成一条 cmd=2 丢回 Queue_pluginmgr 下轮再来
 *         · 全部拿到 -> "没人在用" 已经被锁证明了 -> 直接删
 *    3. 释放 Object / State / Handle, 摘表
 *    4. 队列随插件一起停 (有自己的线程要 join)
 *    5. FreeLibrary
 * ==========================================================================*/
#include "runtime.h"

#include "json.h"
#include "loader.h"

#include <algorithm>
#include <cstring>
#include <windows.h>

namespace mdpsr {

thread_local SharedMaps* Runtime::t_maps = nullptr;

/* ==========================================================================
 *  构造 / 初始化
 * ==========================================================================*/
Runtime::Runtime()
    : _pool_msg(std::make_unique<std::pmr::synchronized_pool_resource>())
    , _pool_map(std::make_unique<std::pmr::synchronized_pool_resource>())
    , _pool_state(std::make_unique<std::pmr::synchronized_pool_resource>())
    , _pool_object(std::make_unique<std::pmr::synchronized_pool_resource>())
    , _map(_pool_map.get()) {
    std::memset(&_api, 0, sizeof(_api));
    install_host_api(this, &_api);
}

Runtime::~Runtime() {
    for (PluginInfo* pi : _plugins) delete pi;
    _plugins.clear();
    {
        std::lock_guard<std::mutex> lk(_q_mtx);
        for (Queue* q : _queues) {
            q->~Queue();
            _pool_object->deallocate(q, sizeof(Queue), alignof(Queue));
        }
        _queues.clear();
    }
    if (_log_file) std::fclose(_log_file);
}

int Runtime::init(const std::wstring& exe_dir) {
    _root = exe_dir;
    while (!_root.empty() && (_root.back() == L'\\' || _root.back() == L'/')) _root.pop_back();
    _root_utf8 = to_utf8(_root);
    SetCurrentDirectoryW(_root.c_str());

    /* init 一定是在主线程上调的 */
    _main_tid = ::GetCurrentThreadId();

    /* ---- 三条固定队列 ----
     *   Queue_pluginmgr  : 核心组件自己的线程 (装载/卸载都在它上面跑)
     *   Queue_default    : 给所有组件的默认队列
     *   Queue_windowsgui : 挂宿主主线程, 保证 GUI 操作在主线程上
     *
     * 顺序就是"谁先能用"的顺序: 先有 pluginmgr, 才有默认队列给插件用。 */
    Queue* q = nullptr;
    Queue::Desc d;

    d.capacity = 4096; d.pace_ms = 0;
    if (queue_create(MDPSR_QUEUE_PLUGINMGR_NAME, d, 0, false, &q) != MDPSR_OK) {
        log(2, "建立 " MDPSR_QUEUE_PLUGINMGR_NAME " 失败");
        return MDPSR_ERR_CREATE_FAILED;
    }

    /* 默认队列: 限速 0 —— 它的公平性由"每条消息跑完才取下一条"保证。
     * 真正需要限速的是"把消息回滚给自己的常驻循环", 那种插件应当在
     * 自己的 plugin.json 里声明 pace。 */
    d.capacity = 65536; d.pace_ms = 0;
    if (queue_create(MDPSR_QUEUE_DEFAULT_NAME, d, 0, false, &q) != MDPSR_OK) {
        log(2, "建立 " MDPSR_QUEUE_DEFAULT_NAME " 失败");
        return MDPSR_ERR_CREATE_FAILED;
    }

    /* GUI 主线: 不起线程, 由主循环 pump_main 驱动。给它一点限速,
     * 免得一个自转的 GUI 循环把主线程占满 (限速在主线程上是"直接返回",
     * 不 sleep, 所以不会堵 Win32 消息泵)。 */
    d.capacity = 16384; d.pace_ms = 8;
    if (queue_create(MDPSR_QUEUE_WINDOWSGUI_NAME, d, 0, true, &q) != MDPSR_OK) {
        log(2, "建立 " MDPSR_QUEUE_WINDOWSGUI_NAME " 失败");
        return MDPSR_ERR_CREATE_FAILED;
    }
    _main_queue_key = mdpsr_hash64(MDPSR_QUEUE_WINDOWSGUI_NAME);

    log(0, "三条固定队列已就绪: " MDPSR_QUEUE_PLUGINMGR_NAME " / "
           MDPSR_QUEUE_DEFAULT_NAME " / " MDPSR_QUEUE_WINDOWSGUI_NAME
           " (最后一条挂在主线程上)");
    return MDPSR_OK;
}

/* ==========================================================================
 *  日志 / 路径
 * ==========================================================================*/
void Runtime::set_log_file(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(_log_mtx);
    if (_log_file) { std::fclose(_log_file); _log_file = nullptr; }
    std::FILE* f = _wfopen(path.c_str(), L"wb");
    if (f) {
        _log_file = f;
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        std::fwrite(bom, 1, 3, f);
    }
}

void Runtime::log(int level, const std::string& msg) {
    if (level < _log_level.load(std::memory_order_relaxed)) return;
    static const char* tags[] = { "INFO", "WARN", "ERR ", "DBG " };
    const char* tag = (level >= 0 && level < 4) ? tags[level] : "????";
    SYSTEMTIME st;
    GetLocalTime(&st);
    const unsigned long tid = (unsigned long)(GetCurrentThreadId() % 100000);
    char line[4096];
    std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d][T%-5lu][%s] %s",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tid, tag, msg.c_str());
    std::lock_guard<std::mutex> lk(_log_mtx);
    std::printf("%s\n", line);
    std::fflush(stdout);
    if (_log_file) {
        std::fprintf(_log_file, "%s\n", line);
        std::fflush(_log_file);
    }
}

std::wstring Runtime::to_wide(const std::string& utf8) const {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], n);
    return w;
}

std::string Runtime::to_utf8(const std::wstring& w) const {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Runtime::resolve(const std::string& rel) const {
    std::wstring w = to_wide(rel);
    for (auto& c : w) if (c == L'/') c = L'\\';
    if (w.size() >= 2 && w[1] == L':') return w;
    if (!w.empty() && w[0] == L'\\') return w;
    if (_root.empty()) return w;
    std::wstring full = _root;
    if (!full.empty() && full.back() != L'\\') full += L'\\';
    return full + w;
}

/* ==========================================================================
 *  队列
 * ==========================================================================*/
int Runtime::queue_create(const std::string& name, const Queue::Desc& d,
                          uint64_t plugin, bool main_thread, Queue** out) {
    if (name.empty()) return MDPSR_ERR_BAD_CONFIG;
    const uint64_t key = mdpsr_hash64(name.c_str());

    {
        std::lock_guard<std::mutex> lk(_q_mtx);
        for (Queue* q : _queues) {
            if (q->key() == key) {                 /* 幂等 */
                if (out) *out = q;
                return MDPSR_OK;
            }
        }
    }

    bool on_main = main_thread;
    /* Queue_windowsgui 是唯一允许挂主线程的队列; 别的名字要求挂主线程一律拒绝,
     * 免得出现"第二条主线"这种没人泵的哑巴队列。 */
    if (on_main && key != _main_queue_key && mdpsr_hash64(MDPSR_QUEUE_WINDOWSGUI_NAME) != key) {
        return MDPSR_ERR_BAD_CONFIG;
    }

    void* mem = _pool_object->allocate(sizeof(Queue), alignof(Queue));
    if (!mem) return MDPSR_ERR_MAP_NO_SPACE;
    Queue* q = new (mem) Queue(_pool_msg.get(), name, key, d);

    /* 队列自己也是一个托管资源 -> 进 MainMap 的 queue 表 */
    mdpsr_ptr* p = nullptr;
    const int r = _map.attach(MDPSR_KIND_QUEUE, key, name.c_str(), q, &p);
    if (r != MDPSR_OK) {
        q->~Queue();
        _pool_object->deallocate(q, sizeof(Queue), alignof(Queue));
        return r;
    }
    q->attach_self(p);

    {
        std::lock_guard<std::mutex> lk(_q_mtx);
        _queues.push_back(q);
    }
    q->start(this, on_main);

    log(0, "队列已建立: '" + name + "' (容量 " + std::to_string(d.capacity ? d.capacity : 4096) +
           "B, 限速 " + std::to_string(d.pace_ms) + "ms" +
           (on_main ? ", 挂主线程" : ", 专属线程已起") + ")");
    if (out) *out = q;
    return MDPSR_OK;
}

Queue* Runtime::queue_of(uint64_t key) {
    std::lock_guard<std::mutex> lk(_q_mtx);
    for (Queue* q : _queues) if (q->key() == key) return q;
    return nullptr;
}

mdpsr_ptr* Runtime::queue_ptr(uint64_t key) {
    return _map.get(key);
}

int Runtime::queue_enum(uint64_t* out, uint32_t cap, uint32_t* out_count) {
    std::lock_guard<std::mutex> lk(_q_mtx);
    uint32_t n = 0;
    for (Queue* q : _queues) {
        if (out && n < cap) out[n] = q->key();
        ++n;
    }
    if (out_count) *out_count = n;
    return MDPSR_OK;
}

int Runtime::raw_queue_create(const char* name, const mdpsr_queue_desc* d,
                              uint64_t plugin, mdpsr_ptr** out) {
    if (!name || !*name) return MDPSR_ERR_NULLPTR;
    Queue::Desc qd;
    if (d) {
        qd.capacity = d->capacity ? d->capacity : 4096;
        qd.pace_ms  = d->pace_ms;
    }
    Queue* q = nullptr;
    const int r = queue_create(name, qd, plugin, false, &q);
    if (r != MDPSR_OK) return r;
    if (out) *out = q->self_ptr();
    return MDPSR_OK;
}

/* manifest 里的队列名 -> 队列键 (O(1), 不用每次遍历) */
static uint64_t body_cmd(const void* body, size_t len) {
    int32_t c = MDPSR_CMD_INIT;
    if (body && len >= sizeof(int32_t)) std::memcpy(&c, body, sizeof(int32_t));
    return static_cast<uint64_t>(c);
}

/* 给新装上的插件点火用的标准载荷。
 * 固定 16 字节 —— 组件只该看前 4 字节的 _cmd, 后面是给我以后加东西用的。 */
struct InitCmd {
    int32_t  _cmd;
    int32_t  _pad;
    uint64_t _arg;
};

Queue* Runtime::queue_for_plugin(uint64_t plugin_key) {
    PluginInfo* pi = find_plugin(plugin_key);
    if (pi) {
        for (const auto& qe : pi->m.queues) {
            if (qe.name == MDPSR_QUEUE_PLUGINMGR_NAME) continue;  /* 内核专属, 不当默认 */
            Queue* q = queue_of(mdpsr_hash64(qe.name.c_str()));
            if (q) return q;
        }
    }
    return queue_of(mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME));
}

uint64_t Runtime::queue_of_handle(uint64_t handle_key) {
    std::lock_guard<std::mutex> lk(_plugin_mtx);
    auto it = _handle_queue.find(handle_key);
    if (it != _handle_queue.end() && it->second) return it->second;
    return 0;
}

int Runtime::bind_handle_queue(uint64_t handle_key, uint64_t queue_key) {
    std::lock_guard<std::mutex> lk(_plugin_mtx);
    if (_handle_owner.find(handle_key) == _handle_owner.end()) {
        return MDPSR_ERR_MAP_KEY_MISSING;
    }
    _handle_queue[handle_key] = queue_key;
    return MDPSR_OK;
}

uint64_t Runtime::queue_key_for_plugin(uint64_t plugin_key) {
    Queue* q = queue_for_plugin(plugin_key);
    if (q) return q->key();
    return mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME);
}

bool Runtime::handle_on_pluginmgr(uint64_t handle_key) {
    return queue_of_handle(handle_key) == mdpsr_hash64(MDPSR_QUEUE_PLUGINMGR_NAME);
}

int Runtime::emit(uint64_t handle_key, const void* body, size_t len) {
    Queue* target = nullptr;

    const uint64_t explicit_q = queue_of_handle(handle_key);
    if (explicit_q) target = queue_of(explicit_q);

    if (!target) {
        uint64_t owner = 0;
        {
            std::lock_guard<std::mutex> lk(_plugin_mtx);
            auto it = _handle_owner.find(handle_key);
            if (it != _handle_owner.end()) owner = it->second;
        }
        if (owner) target = queue_for_plugin(owner);
    }
    if (!target) target = queue_of(mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME));
    if (!target) return MDPSR_ERR_NOT_FOUND;

    target->push(handle_key, static_cast<int32_t>(body_cmd(body, len)), body, len);
    emitted_total.fetch_add(1, std::memory_order_relaxed);
    return MDPSR_OK;
}

int Runtime::emit_to_queue(uint64_t queue_key, uint64_t handle_key,
                           const void* body, size_t len) {
    Queue* q = queue_of(queue_key);
    if (!q) {
        q = queue_of(mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME));
        if (!q) return MDPSR_ERR_QUEUE_NOT_FOUND;
    }
    q->push(handle_key, static_cast<int32_t>(body_cmd(body, len)), body, len);
    emitted_total.fetch_add(1, std::memory_order_relaxed);
    return MDPSR_OK;
}

/* ==========================================================================
 *  Context 组装
 * ==========================================================================*/
SharedMaps* Runtime::local_maps() {
    if (!t_maps) t_maps = new SharedMaps();
    return t_maps;
}

void Runtime::build_ctx(const Manifest& m, uint64_t handle_key,
                        SharedMaps* sm, mdpsr_context* out) {
    sm->clear();
    (void)m;
    (void)handle_key;

    /* handle 表: MainMap 里的全部 handle, 原样给插件 (插件靠它互相投消息,
     * 也靠它查 Queue_default) */
    _map.table_handle()->each([&](uint64_t k, void* v) {
        if (v) sm->handle.set(k, v);
    });

    /* state 表: 全部 state。
     * 为什么给全部而不是"只给声明的": state 是跨组件公用的配置对象,
     * 规格里"Context 提供字典指针"就是这个意思; 隔离靠 valid + 锁,
     * 不靠藏起来 (藏起来反而让插件写出更脆的代码)。 */
    _map.table_state()->each([&](uint64_t k, void* v) {
        if (v) sm->state.set(k, v);
    });

    /* object 表: 只给本插件自己的 object —— 类实例不允许别的组件碰 */
    {
        uint64_t keys[64];
        size_t n = 0;
        for (const auto& o : m.objects) {
            if (n >= 64) break;
            keys[n++] = mdpsr_hash64(o.name.c_str());
        }
        _map.gather(keys, n, &sm->object);
    }

    /* queue 表: 全部队列 (含 Queue_pluginmgr / Queue_default / Queue_windowsgui) */
    _map.table_queue()->each([&](uint64_t k, void* v) {
        if (v) sm->queue.set(k, v);
    });

    out->object = reinterpret_cast<mdpsr_map*>(&sm->object);
    out->state  = reinterpret_cast<mdpsr_map*>(&sm->state);
    out->handle = reinterpret_cast<mdpsr_map*>(&sm->handle);
    out->queue  = reinterpret_cast<mdpsr_map*>(&sm->queue);
    out->host   = &_api;
}

/* ==========================================================================
 *  分发
 *
 *  一条消息的命运:
 *      查 handle -> 按键升序锁住 handle + 它声明的 state/object
 *      -> 调用 -> 释放
 *  锁定期间卸载方拿不到锁, 所以卸载不可能删掉正在跑的东西。
 * ==========================================================================*/
int Runtime::dispatch_one(Queue* q, uint64_t handle_key, int32_t cmd,
                          const uint8_t* body, size_t len) {
    mdpsr_ptr* hp = _map.get(handle_key);
    if (!hp) {
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        log(1, "丢弃消息: 未知或已失效的 handle 0x" + std::to_string(handle_key));
        return MDPSR_ERR_MAP_KEY_MISSING;
    }

    /* 找到拥有这个 handle 的插件。
     * 注意: 必须用"锁到手之后重新取到的那个 h->plugin" —— handle 描述符里
     * 自己就记着所属插件, 这是最权威的来源。绝对不要反过来去查
     * "哪个插件声明了这个 handle" 那种表: 那种查表在多插件场景下容易拿到
     * 别的插件 (这里踩过), 而且多一次加锁。 */
    mdpsr_ptr* hp2 = _map.get(handle_key);
    if (!hp2 || !hp2->ptr) {
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        return MDPSR_ERR_MAP_INVALID;
    }
    auto* hdesc = static_cast<mdpsr_handle_desc*>(hp2->ptr);
    const uint64_t owner_key = hdesc->plugin;

    const Manifest* owner = nullptr;
    {
        std::lock_guard<std::mutex> lk(_plugin_mtx);
        PluginInfo* pi = find_plugin(owner_key);
        if (pi) owner = &pi->m;
    }

    /* 收集要锁的键: handle 自己 + 本插件的 object (+ 本插件声明的 state)。
     * 顺序统一升序 (MultiLock 内部排序), 所以 dispatch 和 unload 不会互咬。 */
    uint64_t keys[1 + 64 + 64];
    size_t   n = 0;
    keys[n++] = handle_key;
    if (owner) {
        for (const auto& s : owner->states)  { if (n < 129) keys[n++] = mdpsr_hash64(s.name.c_str()); }
        for (const auto& o : owner->objects) { if (n < 129) keys[n++] = mdpsr_hash64(o.name.c_str()); }
    }

    MultiLock locks;
    const int lr = locks.acquire(_map, keys, n);
    if (lr == MDPSR_ERR_BUSY) {
        /* 有人正在用这些资源 —— 这条消息暂时办不了。
         * 回滚到同一条队列, 下轮再来 (绝不在这里忙等)。 */
        emit_to_queue(q ? q->key() : mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME),
                      handle_key, body, len);
        return MDPSR_ERR_BUSY;
    }
    if (lr != MDPSR_OK) {
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        return lr;
    }

    /* 锁到手之后再取一次指针: 拿锁过程中可能已经被置无效 */
    hp = _map.get(handle_key);
    if (!hp || !hp->ptr) {
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        return MDPSR_ERR_MAP_INVALID;
    }
    auto* h = static_cast<mdpsr_handle_desc*>(hp->ptr);
    if (!h->fn) {
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        return MDPSR_ERR_BAD_STATE;
    }

    SharedMaps* sm = local_maps();
    mdpsr_context ctx{};
    if (owner) build_ctx(*owner, handle_key, sm, &ctx);
    else {
        sm->clear();
        ctx.object = reinterpret_cast<mdpsr_map*>(&sm->object);
        ctx.state  = reinterpret_cast<mdpsr_map*>(&sm->state);
        ctx.handle = reinterpret_cast<mdpsr_map*>(&sm->handle);
        ctx.queue  = reinterpret_cast<mdpsr_map*>(&sm->queue);
        ctx.host   = &_api;
    }

    int rc = MDPSR_ERR_CREATE_FAILED;
    try {
        rc = h->fn(body, len, &ctx);
    } catch (const std::exception& e) {
        log(2, std::string("handle '") + h->name + "' 抛出异常: " + e.what());
        rc = MDPSR_ERR_BAD_STATE;
    } catch (...) {
        log(2, std::string("handle '") + h->name + "' 抛出未知异常");
        rc = MDPSR_ERR_BAD_STATE;
    }

    dispatched_total.fetch_add(1, std::memory_order_relaxed);
    locks.release();                      /* 调用完立刻放锁 */

    if (rc != MDPSR_OK) {
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        log(1, std::string("handle '") + h->name + "' 返回 " + std::to_string(rc) +
               " (cmd=" + std::to_string(cmd) + ", body " + std::to_string(len) + "B)");
    }
    return rc;
}

/* ==========================================================================
 *  引导
 * ==========================================================================*/
int Runtime::boot() {
    Json cfg;
    std::string err;
    if (!Json::load_file(resolve("config.json"), &cfg, &err)) {
        log(2, "读取 config.json 失败: " + err);
        return MDPSR_ERR_BAD_CONFIG;
    }

    /* { "plugin": [ "core/plugin.json", ... ], "init_cmd": "core_Handle_Mgr" } */
    const Json& pl = cfg["plugin"];
    if (!pl.is_array()) {
        log(2, "config.json 的 plugin 必须是一个数组");
        return MDPSR_ERR_BAD_CONFIG;
    }

    int loaded = 0;
    std::vector<uint64_t> order;
    for (const Json& e : pl.elements()) {
        const std::string rel = e.as_string();
        if (rel.empty()) continue;
        uint64_t plugin = 0;
        const int r = plugin_load(rel, &plugin);
        if (r != MDPSR_OK) {
            log(2, "装载插件失败(" + std::to_string(r) + "): " + rel);
            continue;
        }
        order.push_back(plugin);
        ++loaded;
    }

    /* 后门: 直接调用, 不经过消息队列 */
    const std::string init_tag = cfg["init_cmd"].as_string();
    if (!init_tag.empty()) {
        bool called = false;
        UINT n = 0;
        for (uint64_t k : order) {
            PluginInfo* pi = find_plugin(k);
            if (!pi) continue;
            for (void* mod : pi->modules) {
                const FARPROC p = ::GetProcAddress(static_cast<HMODULE>(mod), init_tag.c_str());
                if (!p) continue;
                auto fn = reinterpret_cast<mdpsr_backdoor_fn>(p);
                log(0, "调用后门: " + init_tag);
                const int r = fn(&_api);
                log(r == MDPSR_OK ? 0 : 2, "后门 " + init_tag + " 返回 " + std::to_string(r));
                called = true;
                break;
            }
            if (called) break;
        }
        if (!called) {
            /* 没写后门不算错: 没有 core 的骨架也能跑 */
            log(1, "没有找到后门函数 " + init_tag + " (跳过)");
        }
    }

    /* 给每个 handle 投一条 cmd = 0 点火。
     * 跳过跑在 Queue_pluginmgr 上的内核 handle —— 它的 cmd=0 由后门自己投。 */
    int fired = 0;
    for (uint64_t k : order) {
        PluginInfo* pi = find_plugin(k);
        if (!pi) continue;
        for (const auto& he : pi->m.handles) {
            if (!pi->m.init_handle.empty() && he.name != pi->m.init_handle) continue;
            const uint64_t hk = mdpsr_hash64(he.name.c_str());
            if (handle_on_pluginmgr(hk)) continue;

            InitCmd init{};
            init._cmd = MDPSR_CMD_INIT;
            init._arg = 0;
            const int r = emit(hk, &init, sizeof(init));
            log(r == MDPSR_OK ? 0 : 1,
                "cmd=0 点火 -> " + he.name + " (rc=" + std::to_string(r) + ")");
            if (r == MDPSR_OK) ++fired;
        }
    }

    log(0, "引导完成: 装载 " + std::to_string(loaded) + " 个插件, 点火 " +
           std::to_string(fired) + " 个 handle");
    return MDPSR_OK;
}

/* ==========================================================================
 *  主线程泵
 * ==========================================================================*/
void Runtime::pump_main(uint32_t wait_ms) {
    if (_main_tid != ::GetCurrentThreadId()) return;   /* 只在主线程上干活 */
    Queue* q = queue_of(_main_queue_key);
    if (!q) return;
    q->pump_once(this, wait_ms);
}

/* ==========================================================================
 *  收尾
 * ==========================================================================*/
void Runtime::stop_queue_threads() {
    std::vector<Queue*> snapshot;
    {
        std::lock_guard<std::mutex> lk(_q_mtx);
        snapshot = _queues;
    }
    for (Queue* q : snapshot) q->request_stop();
    for (Queue* q : snapshot) q->join();          /* 线程先停干净, 再谈释放 */
}

void Runtime::shutdown() {
    /* 队列线程先停 —— 停在 FreeLibrary 之前是硬要求, 否则线程可能正在
     * 执行插件代码, 而 dll 已经被卸掉。 */
    log(0, "[shutdown] 1/3 停队列线程");
    stop_queue_threads();

    /* 卸载全部插件 (反序, 后装的先卸) */
    log(0, "[shutdown] 2/3 卸载插件");
    std::vector<uint64_t> keys;
    {
        std::lock_guard<std::mutex> lk(_plugin_mtx);
        for (PluginInfo* pi : _plugins) keys.push_back(pi->key);
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        log(0, "[shutdown]   卸载 " + std::to_string(*it));
        plugin_unload(*it);
    }

    log(0, "[shutdown] 3/3 完成. 入队 " + std::to_string(emitted_total.load()) +
           " / 分发 " + std::to_string(dispatched_total.load()) +
           " / 限速挡下 " + std::to_string(emitted_paced.load()) +
           " / 错误 " + std::to_string(dispatch_errors.load()));
}

/* ==========================================================================
 *  池
 * ==========================================================================*/
void* Runtime::object_alloc(size_t bytes, size_t align) {
    return _pool_object->allocate(bytes, align);
}
void Runtime::object_free(void* p, size_t bytes, size_t align) {
    if (p) _pool_object->deallocate(p, bytes, align);
}

} /* namespace mdpsr */
