/* ============================================================================
 *  mdpsr/scr/main/runtime.cpp
 *  Runtime: 路径 / 日志 / 模块 / 分流队列 / 分发 / 引导
 * ==========================================================================*/
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace mdpsr {

/* ==========================================================================
 *  构造 / 初始化
 * ==========================================================================*/
Runtime::Runtime()
    : _map(_pools.map()) {
    std::memset(&_api, 0, sizeof(_api));
    install_host_api(this, &_api);
}

Runtime::~Runtime() {
    stop_dispatchers();
    if (_log_file) std::fclose(_log_file);
}

int Runtime::init(const std::wstring& exe_dir) {
    _root = exe_dir;
    while (!_root.empty() && (_root.back() == L'\\' || _root.back() == L'/')) _root.pop_back();
    _root_utf8 = to_utf8(_root);
    SetCurrentDirectoryW(_root.c_str());
    return MDPSR_OK;
}

/* ==========================================================================
 *  日志
 * ==========================================================================*/
void Runtime::set_log_file(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(_log_mtx);
    if (_log_file) { std::fclose(_log_file); _log_file = nullptr; }
    FILE* f = _wfopen(path.c_str(), L"wb");
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

/* ==========================================================================
 *  路径
 * ==========================================================================*/
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
 *  模块
 * ==========================================================================*/
bool Runtime::module_loaded(uint64_t dll) const {
    std::lock_guard<std::mutex> lk(_dll_mtx);
    auto it = _modules.find(dll);
    return it != _modules.end() && !it->second.mods.empty();
}

std::vector<HMODULE> Runtime::module_handles(uint64_t dll) const {
    std::lock_guard<std::mutex> lk(_dll_mtx);
    auto it = _modules.find(dll);
    return it == _modules.end() ? std::vector<HMODULE>{} : it->second.mods;
}

int Runtime::module_load(uint64_t dll, const std::string& rel_path, void** out_module) {
    const std::wstring full = resolve(rel_path);

    HMODULE mod = ::LoadLibraryW(full.c_str());
    if (!mod) mod = ::LoadLibraryW(to_wide(rel_path).c_str());
    if (!mod) {
        log(2, "LoadLibraryW 失败 (" + std::to_string(GetLastError()) + "): " + rel_path);
        return MDPSR_ERR_PLUGIN_LOAD;
    }

    using abi_fn_t = uint32_t (*)(void);
    const DWORD deadline = GetTickCount() + 3000;
    for (;;) {
        abi_fn_t fn = reinterpret_cast<abi_fn_t>(::GetProcAddress(mod, "mdpsr_abi_version"));
        if (fn && fn() == MDPSR_ABI_VERSION) break;
        if (GetTickCount() > deadline) {
            log(2, "插件 ABI 不匹配或未导出 mdpsr_abi_version: " + rel_path);
            ::FreeLibrary(mod);
            return MDPSR_ERR_PLUGIN_ABI;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        ModuleInfo& mi = _modules[dll];
        mi.key      = dll;
        mi.rel_path = rel_path;
        mi.abs_path = full;
        mi.mods.push_back(mod);
    }
    if (out_module) *out_module = mod;
    log(0, "LoadLibrary 成功: " + rel_path);
    return MDPSR_OK;
}

int Runtime::module_ready(void* mod) {
    if (!mod) return MDPSR_ERR_NULLPTR;
    uint64_t key = 0;
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        for (auto& kv : _modules) {
            for (HMODULE mm : kv.second.mods) {
                if (mm == static_cast<HMODULE>(mod)) { key = kv.first; break; }
            }
            if (key) break;
        }
    }
    if (!key) return MDPSR_ERR_PLUGIN_NOT_FOUND;

    using init_fn_t = int (*)(const mdpsr_host*, uint64_t);
    init_fn_t fn = reinterpret_cast<init_fn_t>(
        ::GetProcAddress(static_cast<HMODULE>(mod), "mdpsr_module_init"));
    if (!fn) {
        log(2, "插件未导出 mdpsr_module_init");
        return MDPSR_ERR_PLUGIN_EXPORT;
    }
    const int r = fn(&_api, key);
    if (r != MDPSR_OK) {
        log(2, "mdpsr_module_init 返回 " + std::to_string(r));
        return r;
    }
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        auto it = _modules.find(key);
        if (it != _modules.end()) it->second.ready = true;
    }
    return MDPSR_OK;
}

int Runtime::module_unload(uint64_t dll) {
    std::vector<HMODULE> mods;
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        auto it = _modules.find(dll);
        if (it == _modules.end()) return MDPSR_ERR_PLUGIN_NOT_FOUND;
        mods = it->second.mods;
    }
    for (HMODULE mod : mods) {
        if (!mod) continue;
        using fini_fn_t = void (*)(uint64_t);
        fini_fn_t fini = reinterpret_cast<fini_fn_t>(::GetProcAddress(mod, "mdpsr_module_fini"));
        if (fini) fini(dll);
        ::FreeLibrary(mod);
    }
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        _modules.erase(dll);
    }
    log(0, "FreeLibrary 完成: dll=" + std::to_string(dll));
    return MDPSR_OK;
}

/* ==========================================================================
 *  DLL 专属池索引
 * ==========================================================================*/
mdpsr_dllptr* Runtime::dllptr(uint64_t dll) {
    std::lock_guard<std::mutex> lk(_dll_mtx);
    auto it = _dllptrs.find(dll);
    if (it != _dllptrs.end()) return it->second;

    std::pmr::memory_resource* res = _pools.map();
    void* mem = res->allocate(sizeof(mdpsr_dllptr), alignof(mdpsr_dllptr));
    mdpsr_dllptr* dp = new (mem) mdpsr_dllptr{};
    dp->dll   = dll;
    dp->flags = 1;
    dp->pool  = _pools.dll_pool(dll);
    std::snprintf(dp->name, sizeof(dp->name), "dll:%016llX", (unsigned long long)dll);
    _dllptrs.emplace(dll, dp);
    return dp;
}

int Runtime::dll_pool_create(uint64_t dll, const char* name, mdpsr_dllptr* out) {
    mdpsr_dllptr* dp = dllptr(dll);
    if (!dp) return MDPSR_ERR_MAP_POOL;
    if (name) std::snprintf(dp->name, sizeof(dp->name), "%s", name);
    if (out) *out = *dp;
    return MDPSR_OK;
}

/* ==========================================================================
 *  分流队列
 * ==========================================================================*/
int Runtime::queue_create(const std::string& name, uint32_t capacity,
                          uint64_t dll, bool core, mdpsr_queue** out) {
    if (name.empty()) return MDPSR_ERR_BAD_CONFIG;
    const uint64_t key = mdpsr_hash64(name.c_str());

    {
        std::lock_guard<std::mutex> lk(_queue_mtx);
        auto it = _queues.find(key);
        if (it != _queues.end()) {                 /* 幂等 */
            if (out) *out = it->second.info;
            return MDPSR_OK;
        }
    }

    /* 1. 描述符 —— 进 Pool_map */
    void* mem = _pools.map()->allocate(sizeof(mdpsr_queue), alignof(mdpsr_queue));
    mdpsr_queue* info = new (mem) mdpsr_queue{};
    info->name     = key;
    info->dll      = dll;
    info->capacity = capacity ? capacity : 4096;
    std::snprintf(info->label, sizeof(info->label), "%s", name.c_str());

    /* 2. 队列本体 —— 进 Pool_msg */
    void* qmem = _pools.msg()->allocate(sizeof(Queue), alignof(Queue));
    Queue* q = new (qmem) Queue(_pools.msg(), info->capacity);
    info->impl = q;

    /* 3. 注册进 Map: 与 State* / Classptr* / Handle* 并列 */
    mdpsr_entry ent{};
    ent.key  = key;
    ent.dll  = dll;
    ent.type = MDPSR_ENTRY_QUEUE;
    ent.name = name.c_str();
    ent.ptr  = info;
    const int r = _map.reg(ent);
    if (r != MDPSR_OK) {
        q->~Queue();
        _pools.msg()->deallocate(q, sizeof(Queue), alignof(Queue));
        info->~mdpsr_queue();
        _pools.map()->deallocate(info, sizeof(mdpsr_queue), alignof(mdpsr_queue));
        return r;
    }

    /* 4. dispatcher —— 进 Pool_dispatcher */
    Dispatcher* d = _dispatchers.create(this, q, info);

    /* 5. 线程 —— 进 Pool_thread */
    void* th = _threads.spawn(name, [d]() { d->run(); });
    d->set_thread_handle(th);

    QueueSlot slot;
    slot.key    = key;
    slot.info   = info;
    slot.queue  = q;
    slot.disp   = d;
    slot.thread = th;
    slot.core   = core;
    {
        std::lock_guard<std::mutex> lk(_queue_mtx);
        _queues.emplace(key, slot);
    }
    if (core) _core_queue_key = key;

    log(0, "分流队列已建立: '" + name + "' (容量 " + std::to_string(info->capacity) +
           "B, 专属分发线程已起" + (core ? ", 核心队列" : "") + ")");
    if (out) *out = info;
    return MDPSR_OK;
}

void Runtime::release_slot(QueueSlot& slot) {
    if (slot.disp) {
        _dispatchers.destroy(slot.disp);
        slot.disp = nullptr;
    }
    if (slot.queue) {
        slot.queue->~Queue();
        _pools.msg()->deallocate(slot.queue, sizeof(Queue), alignof(Queue));
        slot.queue = nullptr;
    }
    if (slot.info) {
        const uint64_t k = slot.info->name;
        _map.erase(k);
        slot.info->~mdpsr_queue();
        _pools.map()->deallocate(slot.info, sizeof(mdpsr_queue), alignof(mdpsr_queue));
        slot.info = nullptr;
    }
}

int Runtime::queue_destroy(mdpsr_queue* q) {
    if (!q) return MDPSR_ERR_NULLPTR;

    QueueSlot slot;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(_queue_mtx);
        auto it = _queues.find(q->name);
        if (it != _queues.end() && it->second.info == q) {
            slot = it->second;
            _queues.erase(it);
            found = true;
        }
    }
    if (!found) return MDPSR_ERR_QUEUE_NOT_FOUND;

    if (slot.disp && slot.disp->thread_id() == std::this_thread::get_id()) {
        /* 在自己的分发线程上销毁自己的队列 -> 塞回去, 拒绝 (会自 join 死锁) */
        std::lock_guard<std::mutex> lk(_queue_mtx);
        _queues.emplace(slot.key, slot);
        log(2, std::string("拒绝在自己的分发线程上销毁队列 '") +
               (slot.info ? slot.info->label : "?") + "'");
        return MDPSR_ERR_QUEUE_SELF;
    }

    if (slot.disp) slot.disp->request_stop();
    if (slot.thread) {
        _threads.join(slot.thread);
        slot.thread = nullptr;
    }
    const uint64_t key = slot.key;
    release_slot(slot);
    if (key == _core_queue_key) _core_queue_key = 0;
    log(0, "分流队列已销毁: 键=" + std::to_string(key));
    return MDPSR_OK;
}

int Runtime::queue_bind_key(uint64_t handle_key, uint64_t queue_key) {
    return _map.set_bind_queue(handle_key, queue_key);
}

int Runtime::queue_bind(uint64_t handle_key, mdpsr_queue* q) {
    return _map.set_bind_queue(handle_key, q ? q->name : 0);
}

mdpsr_queue* Runtime::queue_find(uint64_t key) {
    std::lock_guard<std::mutex> lk(_queue_mtx);
    auto it = _queues.find(key);
    return it == _queues.end() ? nullptr : it->second.info;
}

mdpsr_queue* Runtime::queue_core() {
    {
        std::lock_guard<std::mutex> lk(_queue_mtx);
        auto it = _queues.find(_core_queue_key);
        if (it != _queues.end()) return it->second.info;
    }
    return queue_find(mdpsr_hash64(MDPSR_QUEUE_CORE_NAME));
}

Queue* Runtime::queue_impl(mdpsr_queue* q) const {
    if (!q) return nullptr;
    std::lock_guard<std::mutex> lk(_queue_mtx);
    auto it = _queues.find(q->name);
    if (it == _queues.end() || it->second.info != q) return nullptr;
    return it->second.queue;
}

int Runtime::queue_list(mdpsr_queue** out, uint32_t cap, uint32_t* out_count) {
    std::lock_guard<std::mutex> lk(_queue_mtx);
    uint32_t n = 0;
    for (auto& kv : _queues) {
        if (out && n < cap) out[n] = kv.second.info;
        ++n;
    }
    if (out_count) *out_count = n;
    return MDPSR_OK;
}

size_t Runtime::dispatcher_count() const {
    return _dispatchers.count();
}

/* ==========================================================================
 *  消息投递
 * ==========================================================================*/
int Runtime::raw_emit(Queue* q, uint64_t handle_key, const void* body, size_t len) {
    if (!q) return MDPSR_ERR_QUEUE_NOT_FOUND;
    q->push(handle_key, body, len);
    emitted_total.fetch_add(1, std::memory_order_relaxed);
    return MDPSR_OK;
}

int Runtime::emit(uint64_t handle_key, const void* body, size_t len) {
    /* 1. 目标 handle 绑定了分流队列就投给它 */
    uint64_t qkey = 0;
    {
        Countptr* cp = nullptr;
        if (_map.acquire(handle_key, false, &cp) == MDPSR_OK && cp) {
            qkey = cp->bind_queue;
            _map.release(handle_key, false);
        }
    }
    if (qkey) {
        mdpsr_queue* q = queue_find(qkey);
        if (q && raw_emit(queue_impl(q), handle_key, body, len) == MDPSR_OK) return MDPSR_OK;
        fallback_total.fetch_add(1, std::memory_order_relaxed);
    }

    /* 2. 回退 queue_core (默认组件都走这里) */
    mdpsr_queue* cq = queue_core();
    if (!cq) {
        if (queue_create(MDPSR_QUEUE_CORE_NAME, 0, 0, true, &cq) != MDPSR_OK || !cq) {
            dispatch_errors.fetch_add(1, std::memory_order_relaxed);
            return MDPSR_ERR_QUEUE_NOT_FOUND;
        }
        log(1, "queue_core 在 DLL_init 之前被惰性创建");
    }
    return raw_emit(queue_impl(cq), handle_key, body, len);
}

int Runtime::queue_emit(mdpsr_queue* q, uint64_t handle_key, const void* body, size_t len) {
    if (q) {
        Queue* impl = queue_impl(q);
        if (impl && raw_emit(impl, handle_key, body, len) == MDPSR_OK) return MDPSR_OK;
        fallback_total.fetch_add(1, std::memory_order_relaxed);
    }
    mdpsr_queue* cq = queue_core();
    if (!cq) {
        if (queue_create(MDPSR_QUEUE_CORE_NAME, 0, 0, true, &cq) != MDPSR_OK || !cq) {
            return MDPSR_ERR_QUEUE_NOT_FOUND;
        }
    }
    return raw_emit(queue_impl(cq), handle_key, body, len);
}

/* ==========================================================================
 *  分发 (由 Dispatcher 线程调用, 每个 dispatcher 只吃自己的队列)
 * ==========================================================================*/
int Runtime::dispatch_message(Dispatcher* d, uint64_t handle_key,
                              const uint8_t* body, size_t len) {
    Countptr* hcp = nullptr;
    if (_map.acquire(handle_key, false, &hcp) != MDPSR_OK) {
        log(1, "丢弃消息: 未知或已失效的 handle 0x" + std::to_string(handle_key));
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    auto* h = static_cast<mdpsr_handle*>(hcp->ptr);
    if (!h || !h->fn) {
        _map.release(handle_key, false);
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    /* --- 绑定: 全部 state + object --- */
    mdpsr_state_slot slots[MDPSR_MAX_BIND_STATES];
    uint64_t acquired[MDPSR_MAX_BIND_STATES];
    size_t nslots = 0, nacc = 0;
    for (uint32_t i = 0; i < hcp->bind_state_count && nslots < MDPSR_MAX_BIND_STATES; ++i) {
        const uint64_t k = hcp->bind_states[i];
        if (!k) continue;
        Countptr* scp = nullptr;
        if (_map.acquire(k, false, &scp) != MDPSR_OK || !scp) continue;
        acquired[nacc++] = k;
        slots[nslots].key   = k;
        slots[nslots].state = static_cast<mdpsr_state*>(scp->ptr);
        ++nslots;
    }
    Countptr* ocp = nullptr;
    if (hcp->bind_object) _map.acquire(hcp->bind_object, false, &ocp);
    mdpsr_classptr* cp = ocp ? static_cast<mdpsr_classptr*>(ocp->ptr) : nullptr;
    mdpsr_state* st0 = nslots ? slots[0].state : nullptr;

    /* 本次分发所在的分流队列 —— 所有组件都能从上下文拿到 Map.Queue* */
    mdpsr_queue* cur_queue = d ? d->info() : nullptr;

    std::vector<uint8_t> ctx;
    if (hcp->type == MDPSR_ENTRY_HANDLE_CORE) {
        std::vector<mdpsr_classptr*> all;
        _map.collect_classptrs(all);
        mdpsr_classptr_table table{};
        table.items = all.empty() ? nullptr : all.data();
        table.count = all.size();

        std::vector<mdpsr_queue*> qs;
        {
            std::lock_guard<std::mutex> lk(_queue_mtx);
            for (auto& kv : _queues) qs.push_back(kv.second.info);
        }
        mdpsr_queue_table qtable{};
        qtable.items = qs.empty() ? nullptr : qs.data();
        qtable.count = qs.size();

        mdpsr_core_content cc{};
        cc.state        = st0;
        cc.classptr     = cp;
        cc.handle       = h;
        cc.handle_core  = reinterpret_cast<mdpsr_handle_core*>(h);
        cc.classptr_all = &table;
        cc.dllptr       = dllptr(hcp->dll);
        cc.host         = &_api;
        cc.states       = slots;
        cc.state_count  = nslots;
        cc.queue        = cur_queue;
        cc.queue_all    = &qtable;
        ctx.resize(sizeof(cc));
        std::memcpy(ctx.data(), &cc, sizeof(cc));
    } else {
        mdpsr_content c{};
        c.state       = st0;
        c.classptr    = cp;
        c.host        = &_api;
        c.states      = slots;
        c.state_count = nslots;
        c.queue       = cur_queue;
        ctx.resize(sizeof(c));
        std::memcpy(ctx.data(), &c, sizeof(c));
    }

    int r = MDPSR_ERR_CREATE_FAILED;
    try {
        r = h->fn(body, len, ctx.data(), ctx.size());
    } catch (const std::exception& e) {
        log(2, std::string("handle '") + h->name + "' 抛出异常: " + e.what());
    } catch (...) {
        log(2, std::string("handle '") + h->name + "' 抛出未知异常");
    }
    dispatched_total.fetch_add(1, std::memory_order_relaxed);

    if (ocp) _map.release(hcp->bind_object, false);
    for (size_t i = 0; i < nacc; ++i) _map.release(acquired[i], false);
    _map.release(handle_key, false);

    if (r != MDPSR_OK) {
        dispatch_errors.fetch_add(1, std::memory_order_relaxed);
        log(1, std::string("handle '") + h->name + "' 返回 " + std::to_string(r) +
               " (body " + std::to_string(len) + "B)");
    }
    return 1;
}

/* ==========================================================================
 *  引导 / 收尾
 * ==========================================================================*/
int Runtime::boot(uint32_t* out_plugins, std::string* out_init_cmd, int* out_backdoor_rc) {
    Json cfg;
    std::string err;
    if (!Json::load_file(resolve("config.json"), &cfg, true, &err)) {
        log(2, "读取 exe 同目录 config.json 失败: " + err);
        return MDPSR_ERR_BAD_CONFIG;
    }

    uint32_t np = 0;

    /* --- 1. 预先加载的插件 (统一走 plugin_load) --- */
    const Json& pl = cfg["plugin"];
    if (!pl.is_array()) {
        log(2, "config.json 缺少 plugin 列表");
        return MDPSR_ERR_BAD_CONFIG;
    }
    for (const Json& e : pl.elements()) {
        const std::string rel = e.as_string();
        if (rel.empty()) continue;
        uint64_t dll = 0;
        const int r = plugin_load(rel, &dll);
        if (r != MDPSR_OK) {
            log(2, "预加载插件失败(" + std::to_string(r) + "): " + rel);
            continue;
        }
        ++np;
    }

    /* --- 2. 后门级别的初始化函数: 直接调用, 不经过消息队列 --- */
    const std::string tag = cfg["init_cmd"].as_string();
    if (out_init_cmd) *out_init_cmd = tag;

    int brc = MDPSR_OK;
    if (tag.empty()) {
        log(1, "config.json 没有 init_cmd, 跳过初始化后门");
    } else {
        brc = call_backdoor(tag);
    }
    if (out_backdoor_rc) *out_backdoor_rc = brc;

    if (out_plugins) *out_plugins = np;
    log(0, "引导完成: 预加载插件 " + std::to_string(np) + " 个, 后门 '" + tag +
           "' 返回 " + std::to_string(brc));
    return MDPSR_OK;
}

int Runtime::call_backdoor(const std::string& tag) {
    if (tag.empty()) return MDPSR_ERR_NOT_FOUND;

    std::vector<HMODULE> mods;
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        for (auto& kv : _modules) {
            for (HMODULE m : kv.second.mods) mods.push_back(m);
        }
    }
    for (HMODULE m : mods) {
        FARPROC p = ::GetProcAddress(m, tag.c_str());
        if (!p) continue;
        auto fn = reinterpret_cast<mdpsr_backdoor_fn>(p);
        log(0, "调用后门函数: " + tag);
        const int r = fn(&_api);
        log(r == MDPSR_OK ? 0 : 2, "后门 " + tag + " 返回 " + std::to_string(r));
        return r;
    }
    log(2, "在所有已加载模块里都找不到后门函数: " + tag);
    return MDPSR_ERR_NOT_FOUND;
}

int Runtime::request_clear(uint32_t timeout_ms) {
    mdpsr_queue* cq = queue_core();
    if (!cq) {
        log(1, "没有 queue_core, 跳过清空区域");
        return MDPSR_OK;
    }
    uint8_t buf[sizeof(mdpsr_dllmgr_cmd) + 1];
    const size_t n = mdpsr_dllmgr_pack(buf, sizeof(buf), MDPSR_DLLMGR_CMD_CLEAR, 0, "");
    if (!n) return MDPSR_ERR_BAD_MESSAGE;
    emit(mdpsr_hash64("DLLMgr_handle"), buf, n);
    log(0, "已向 queue_core 注入 DLLMgr cmd=3 (清空区域)");

    Queue* q = queue_impl(cq);
    const auto t0 = std::chrono::steady_clock::now();
    auto last_busy = t0;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() >
            static_cast<long long>(timeout_ms)) {
            log(1, "清空区域超时");
            break;
        }
        if (!q || q->readable() == 0) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_busy).count() > 300)
                break;      /* 队列连续 300ms 空 -> 认为收拾干净了 */
        } else {
            last_busy = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return MDPSR_OK;
}

void Runtime::stop_dispatchers() {
    /* 1. 全部请求停止 */
    _dispatchers.stop_all();

    /* 2. 摘出所有还活着的队列槽位 */
    std::vector<QueueSlot> slots;
    {
        std::lock_guard<std::mutex> lk(_queue_mtx);
        for (auto& kv : _queues) slots.push_back(kv.second);
        _queues.clear();
        _core_queue_key = 0;
    }

    /* 3. 再置一次停 (防止有队列是刚建的), 然后统一 join */
    for (QueueSlot& s : slots) {
        if (s.disp) s.disp->request_stop();
    }
    for (QueueSlot& s : slots) {
        if (s.thread) { _threads.join(s.thread); s.thread = nullptr; }
    }

    /* 4. 释放 */
    for (QueueSlot& s : slots) release_slot(s);
}

int Runtime::unload_all() {
    std::vector<uint64_t> keys;
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        for (auto& kv : _modules) keys.push_back(kv.first);
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        plugin_unload(*it);
    }
    return MDPSR_OK;
}

} /* namespace mdpsr */
