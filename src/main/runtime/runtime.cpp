/* ============================================================================
 *  mdpsr/runtime/runtime.cpp
 *  池 / 注册表 / 队列 / 分发 / 引导 / 收尾
 * ==========================================================================*/
#include "runtime.h"

#include "json.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#pragma comment(lib, "winmm.lib")

namespace mdpsr {

/* ==========================================================================
 *  构造 / 析构
 * ==========================================================================*/
Runtime::Runtime()
    : _arena(std::make_unique<std::pmr::synchronized_pool_resource>())
    , _reg(_arena.get()) {
    std::memset(&_api, 0, sizeof(_api));
    install_host_api(this, &_api);
}

Runtime::~Runtime() {
    /* 队列对象显式析构 (停线程 + 放缓冲); 内存随各自的池回收。
     * 注意顺序: 插件队列在卸载时就已经从 _queues 里摘掉并析构过了,
     * 所以这里剩下的只有宿主自己的几条。 */
    for (Queue* q : _queues) {
        if (q) q->~Queue();
    }
    _queues.clear();
    if (_log_file) std::fclose(_log_file);
    _log_file = nullptr;
}

uint32_t Runtime::current_thread_id() {
    return static_cast<uint32_t>(::GetCurrentThreadId());
}

/* ==========================================================================
 *  初始化
 * ==========================================================================*/
int Runtime::init(const std::wstring& exe_dir) {
    _root = exe_dir;
    while (!_root.empty() && (_root.back() == L'\\' || _root.back() == L'/')) _root.pop_back();
    _root_utf8 = to_utf8(_root);
    ::SetCurrentDirectoryW(_root.c_str());

    _main_tid = current_thread_id();

    /* ★ 提高系统计时器精度到 1ms。
     * 不这么做的话 Windows 的 sleep 粒度是 ~15.6ms, 声明 pace_ms = 20 实际
     * 会跑成 ~31ms —— v1 就是这么跑的。限速是这个框架的招牌功能, 不准就没意义。 */
    ::timeBeginPeriod(1);

    const int rc = create_builtin_queues();
    if (rc != MDPSR_OK) return rc;

    log(0, "运行时根目录: " + _root_utf8);
    log(0, "ABI 版本: " + std::to_string(MDPSR_ABI_VERSION) + ", 帧头 " +
           std::to_string(MDPSR_MSG_HEADER) + " 字节");
    return MDPSR_OK;
}

int Runtime::create_builtin_queues() {
    Queue::Desc d;
    uint64_t k = 0;

    d.capacity = 65536; d.pace_ms = 0;
    if (queue_create(MDPSR_QUEUE_SYS_NAME, d, nullptr, 0, &k) != MDPSR_OK) {
        log(2, "建立 " MDPSR_QUEUE_SYS_NAME " 失败");
        return MDPSR_ERR_CREATE_FAILED;
    }
    d.capacity = 262144; d.pace_ms = 0;
    if (queue_create(MDPSR_QUEUE_DEFAULT_NAME, d, nullptr, 0, &k) != MDPSR_OK) {
        log(2, "建立 " MDPSR_QUEUE_DEFAULT_NAME " 失败");
        return MDPSR_ERR_CREATE_FAILED;
    }
    /* ★ 宿主不再建"挂主线程"的队列。需要线程亲和的插件自己开线程
     * (references/components/winmsg), 宿主主线程只负责 init/boot/等退出。 */
    log(0, "固定队列已就绪: " MDPSR_QUEUE_SYS_NAME " / " MDPSR_QUEUE_DEFAULT_NAME
           " (各自一条专属线程; 宿主主线程不参与分发)");
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
    /* 等级: 0=INFO 1=WARN 2=ERR 3=DBG。
     * _log_level 是"最低可见等级" (--quiet 设成 1 = 只看 WARN 以上);
     * DBG 是另一条独立的开关 (--debug), 默认不显示 —— 否则那些
     * "跑起来会刷屏的细节日志" 就等于默认打开了。 */
    if (level == 3) {
        if (!_log_debug.load(std::memory_order_relaxed)) return;
    } else if (level < _log_level.load(std::memory_order_relaxed)) {
        return;
    }
    static const char* tags[] = { "INFO", "WARN", "ERR ", "DBG " };
    const char* tag = (level >= 0 && level < 4) ? tags[level] : "????";
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    const unsigned long tid = static_cast<unsigned long>(::GetCurrentThreadId() % 1000000ul);
    char line[4096];
    std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d][T%-6lu][%s] %s",
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
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], n);
    return w;
}

std::string Runtime::to_utf8(const std::wstring& w) const {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
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
bool Runtime::queue_exists(uint64_t key) {
    std::shared_lock<std::shared_mutex> lk(_q_mtx);
    for (Queue* q : _queues) if (q->key() == key) return true;
    return false;
}

int Runtime::queue_create(const char* name, const Queue::Desc& d, Pool* pool, uint64_t owner,
                          uint64_t* out_key) {
    if (!name || !*name) return MDPSR_ERR_BAD_CONFIG;
    const uint64_t key = mdpsr_hash64(name);
    if (out_key) *out_key = key;

    {   /* 幂等: 重名直接返回已有的那条 —— 但只对"同一个主人"幂等。
         * ★ 别人的队列不能靠重名蹭过来: 否则第二个插件会把这个键登记成自己的,
         *   卸载时把别人的队列线程一起拆掉 (消息丢光)。清单装载期的条目名全局
         *   唯一检查就是这条规则的"静态版", 运行时创建必须自己检查一遍。 */
        std::shared_lock<std::shared_mutex> lk(_q_mtx);
        for (Queue* q : _queues) {
            if (q->key() != key) continue;
            Entry* ex = _reg.find(key);
            const uint64_t qowner = ex ? ex->plugin : 0;
            if (owner && qowner && qowner != owner) return MDPSR_ERR_ALREADY_EXISTS;
            return MDPSR_OK;
        }
    }

    std::pmr::memory_resource* res = pool ? static_cast<std::pmr::memory_resource*>(&pool->res)
                                          : _arena.get();
    void* mem = res->allocate(sizeof(Queue), alignof(Queue));
    if (!mem) return MDPSR_ERR_NO_SPACE;
    Queue* q = new (mem) Queue(res, name, key, d);

    Entry* e = nullptr;
    uint32_t owner_gen = 0;
    if (pool && owner) {
        PluginRecord* rec = record_of(owner);
        if (rec) owner_gen = rec->gen;
    }
    /* 队列不是"可借"资源 (acquire 一个 QUEUE 会拿 TYPE_MISMATCH), 所以直接上线。 */
    const int rc = _reg.attach(key, MDPSR_KIND_QUEUE, owner, owner_gen, name, q, nullptr, nullptr,
                               true, &e);
    if (rc != MDPSR_OK) {
        q->~Queue();
        return rc;
    }
    {
        std::unique_lock<std::shared_mutex> lk(_q_mtx);
        _queues.push_back(q);
    }
    q->start(this);
    log(0, "队列已建立: '" + std::string(name) + "' (容量 " + std::to_string(d.capacity) +
           "B, 限速 " + std::to_string(d.pace_ms) + "ms, 专属线程)");
    return MDPSR_OK;
}

void Runtime::drop_queue(uint64_t queue_key) {
    Queue* q = nullptr;

    /* A. 先让它停 (共享锁就够: request_stop 只是翻个标志 + 唤醒) */
    {
        std::shared_lock<std::shared_mutex> lk(_q_mtx);
        for (Queue* it : _queues) if (it->key() == queue_key) { q = it; break; }
    }
    if (!q) return;
    q->request_stop();

    /* B. 等它退出。★ 这一步不能持任何锁: 线程退出前可能还要走一次
     *    "查找+入队" (共享锁), 我们拿着独占锁等它 = 死锁。 */
    q->join();
    const size_t dropped = q->drain_pending();
    if (dropped) {
        t_dropped.fetch_add(dropped, std::memory_order_relaxed);
        /* 卸载时队列里还剩消息是正常的 (插件本来就要走了), 所以只记 DBG,
         * 总数在收尾汇总里报一次。 */
        log(3, "队列 '" + q->name() + "' 结束时丢掉 " + std::to_string(dropped) + " 条消息");
    }

    /* C. 独占一次, 确认"没人正在 push", 然后从表里摘掉。
     *    入队路径是"查找 + push 在同一把共享锁下", 所以拿到独占锁 = 没有
     *    任何线程还握着这个队列对象。摘掉之后新的查找就找不到它了。 */
    {
        std::unique_lock<std::shared_mutex> lk(_q_mtx);
        bool still = false;
        for (auto it = _queues.begin(); it != _queues.end(); ++it) {
            if ((*it)->key() == queue_key) { _queues.erase(it); still = true; break; }
        }
        if (!still) return;
    }
    q->~Queue();      /* 内存归它的池, 这里只跑析构 */
}

int Runtime::queue_bind(uint64_t handle_key, uint64_t queue_key) {
    Entry* e = _reg.find(handle_key);
    if (!e || e->kind != MDPSR_KIND_HANDLE || e->gen.load(std::memory_order_acquire) == 0) {
        return MDPSR_ERR_KEY_MISSING;
    }
    /* 只能绑自己的 handle (宿主例外): 否则 A 插件能把 B 插件的消息改道。 */
    const uint64_t cp = tls().cur_plugin;
    if (cp != 0 && e->plugin != cp) return MDPSR_ERR_NO_PERM;

    /* ★ 队列也得是"自己的"或宿主的。
     * 只查 handle 归属是不够的: 把消息改道到别人的队列上, 等于让别人来跑自己的
     * 代码 —— 而且卸载别人的插件时那条队列线程会被 join 掉, 谁在上面跑插件代码
     * 谁就可能和管理面(装卸)咬成一个环 (见 doc/架构.md 第九节)。 */
    Entry* qe = _reg.find(queue_key);
    if (!qe || qe->kind != MDPSR_KIND_QUEUE || qe->gen.load(std::memory_order_acquire) == 0) {
        return MDPSR_ERR_QUEUE_NOT_FOUND;
    }
    if (cp != 0 && qe->plugin != 0 && qe->plugin != cp) return MDPSR_ERR_NO_PERM;

    set_handle_route(handle_key, queue_key);
    return MDPSR_OK;
}

uint64_t Runtime::queue_of_handle(uint64_t handle_key) {
    std::lock_guard<std::mutex> lk(_route_mtx);
    auto it = _route.find(handle_key);
    return it == _route.end() ? 0 : it->second;
}

void Runtime::set_handle_route(uint64_t handle_key, uint64_t queue_key) {
    std::lock_guard<std::mutex> lk(_route_mtx);
    _route[handle_key] = queue_key;
}

void Runtime::forget_handle_routes(const std::vector<uint64_t>& keys) {
    std::lock_guard<std::mutex> lk(_route_mtx);
    for (uint64_t k : keys) _route.erase(k);
}

int Runtime::queue_list(uint64_t* out, uint32_t cap, uint32_t* out_count) {
    std::shared_lock<std::shared_mutex> lk(_q_mtx);
    uint32_t n = 0;
    for (Queue* q : _queues) {
        if (out && n < cap) out[n] = q->key();
        ++n;
    }
    if (out_count) *out_count = n;
    return MDPSR_OK;
}

int Runtime::queue_stats(uint64_t key, mdpsr_queue_stats* out) {
    std::shared_lock<std::shared_mutex> lk(_q_mtx);
    for (Queue* q : _queues) {
        if (q->key() != key) continue;
        q->stats(out);                 /* push 走的是 Queue 自己的锁, 不冲突 */
        return MDPSR_OK;
    }
    return MDPSR_ERR_QUEUE_NOT_FOUND;
}

int Runtime::track_runtime_queue(uint64_t plugin, uint64_t queue_key) {
    std::lock_guard<std::mutex> lk(_plg_mtx);
    auto it = _records.find(plugin);
    if (it == _records.end()) return MDPSR_ERR_PLUGIN_NOT_FOUND;
    PluginRecord* rec = it->second;
    /* ★ 必须在同一个临界区里同时"看状态"和"登记":
     * 卸载方也是在 _plg_mtx 里把状态改成 UNLOADING 之后才快照 entries 的,
     * 所以这条队列要么被卸载方看到, 要么根本建不出来 —— 不会漏。 */
    if (rec->status.load(std::memory_order_acquire) != MDPSR_PLUGIN_READY) {
        return MDPSR_ERR_NOT_READY;
    }
    /* 同一个键只登记一次: 重复登记会让卸载时对同一条队列 drop 两次
     * (第二次是空转, 但基线核对会变得难读)。 */
    for (uint64_t k : rec->queue_keys) if (k == queue_key) return MDPSR_OK;
    rec->queue_keys.push_back(queue_key);
    rec->entries.push_back(queue_key);
    return MDPSR_OK;
}

/* ==========================================================================
 *  消息
 * ==========================================================================*/
int Runtime::emit_to(uint64_t queue_key, uint64_t dst, uint64_t src, int32_t cmd,
                     const void* body, uint32_t len) {
    int rc = MDPSR_ERR_QUEUE_NOT_FOUND;
    int full_hit = 0;
    {
        /* ★ 查找和入队在同一把共享锁下完成 —— 队列对象绝不可能在 push 中间
         *   被销毁 (销毁方要拿独占锁)。共享锁允许多个生产者同时入队,
         *   每条队列内部再自己串行化。 */
        std::shared_lock<std::shared_mutex> lk(_q_mtx);
        Queue* q = nullptr;
        for (Queue* it : _queues) if (it->key() == queue_key) { q = it; break; }
        /* ★ 找不到就是找不到: 绝不偷偷改投 Queue_default。
         *   显式指名队列的投递错了地方, 比投不进去难查一个数量级 —— "回退到默认
         *   队列"这种事只允许出现在不指名队列的 emit() 里 (那里是设计好的兜底)。 */
        if (!q) return MDPSR_ERR_QUEUE_NOT_FOUND;

        rc = q->push(dst, src, cmd, body, len);
        if (rc == MDPSR_OK) {
            t_emitted.fetch_add(1, std::memory_order_relaxed);
        } else if (rc == MDPSR_ERR_QUEUE_FULL) {
            ++full_hit;
        }
    }
    /* ★ 日志放在锁外面: log() 要拿 _log_mtx 还可能写盘, 在共享锁里做这件事
     * 会把"摘队列"的独占锁一起拖住。 */
    if (full_hit && t_queue_full.fetch_add(1, std::memory_order_relaxed) < 3) {
        log(1, "队列满了, 消息被拒 (调用方收到 QUEUE_FULL)");
    }
    return rc;
}

int Runtime::emit(uint64_t dst, uint64_t src, int32_t cmd, const void* body, uint32_t len) {
    if (src == 0) src = tls().cur_handle;      /* 默认发件人 = 当前正在跑的 handle */
    uint64_t qk = queue_of_handle(dst);
    if (!qk) qk = mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME);   /* 没有路由 -> 默认队列 */
    return emit_to(qk, dst, src, cmd, body, len);
}

int Runtime::reply(const mdpsr_msg* req, int32_t cmd, const void* body, uint32_t len) {
    if (!req || req->src == 0) return MDPSR_OK;   /* 没有发件人, 没什么可回的 */
    return emit(req->src, tls().cur_handle, cmd, body, len);
}

/* ==========================================================================
 *  分发
 * ==========================================================================*/
int Runtime::dispatch_frame(Queue& q, const Frame& f) {
    q.mark("d:find");
    Entry* e = _reg.find(f.dst);

    auto dead = [&](int code, int reason) -> int {
        q.mark("d:dead");
        if (t_dead.fetch_add(1, std::memory_order_relaxed) < 5) {
            log(1, "死信: handle '" + key_name(f.dst) + "' 不存在或已失效 (原因 " +
                   std::to_string(reason) + ")");
        }
        q.mark("d:dead-emit");
        if (f.src != 0) {
            mdpsr_fail fail{};
            fail.code = code;
            fail.reason = reason;
            fail.dst = f.dst;
            uint64_t back = queue_of_handle(f.src);
            if (!back) back = mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME);
            emit_to(back, f.src, 0, MDPSR_CMD_FAIL, &fail, sizeof(fail));
        }
        q.mark("d:dead-done");
        return code;
    };

    if (!e || e->kind != MDPSR_KIND_HANDLE) return dead(MDPSR_ERR_KEY_MISSING, MDPSR_FAIL_DEAD_HANDLE);
    if (e->gen.load(std::memory_order_acquire) == 0) {
        return dead(MDPSR_ERR_ENTRY_INVALID, MDPSR_FAIL_DEAD_HANDLE);
    }
    /* ★ 装载中途的 handle (条目挂上了, 但插件还没上线 READY): 既不能调它, 也不能
     *   当死信 —— 它是马上要来的服务, 不是已经走了的服务。
     *   回 BUSY 走"回滚重投": 消息留在队列里, 等它上线下一轮再跑。
     *   (上一版这里没有这道闸, 于是装载窗口里跑起来的 handle 会发现自己借不到
     *    自己的 State —— 自测直接抓到 2 次 11019。) */
    if (!e->published.load(std::memory_order_acquire)) {
        q.mark("d:not-published");
        t_busy_requeue.fetch_add(1, std::memory_order_relaxed);
        emit_to(q.key(), f.dst, f.src, f.cmd, f.body, f.body_len);
        return MDPSR_ERR_BUSY;
    }

    /* ★ 先 try_lock, 拿不到再分情况:
     *
     *   · 如果 gen 已经是 0 —— 说明有人正在卸这个插件, 而且卸载方是
     *     "先全部置无效, 再独占全部条目" 的顺序, 所以它一定已经 invalidate 过了。
     *     这时候我们【绝不能阻塞】: 卸载方正握着这把锁去 join 本线程所在的队列,
     *     我们一阻塞, 它 join 我、我等它放锁 —— 死锁。
     *     正确做法是把这条消息当死信扔掉, 然后回到循环顶部, 让 _stop 生效。
     *
     *   · 如果 gen 还有效 —— 那就是普通的并发争用 (比如装载时在调用者线程上
     *     同步跑 cmd=0, 正好和队列线程撞上), 老老实实等, 等的时间有界。 */
    /* ★ 有界等待地锁 handle。
     *
     * 卸载方的顺序: 置 gen=0 -> 独占全部条目 -> 停队列 / join 队列线程 -> 卸 dll。
     * 于是"锁被握着"通常就意味着 gen 已经是 0, 我们立刻当死信扔掉走人;
     * 但万一碰到"锁被握着而 gen 还没翻"的窗口, 无界等待就会卡死:
     * 卸载方握着这把锁去 join 本线程所在的队列 —— 它等我退出, 我等它放锁。
     * 加上等待上界 + 每轮复查 gen / 队列是否在停, 这个环就永远闭合不了。 */
    int lr = MDPSR_OK;
    for (int i = 0; i < MDPSR_LOCK_TRIES; ++i) {
        if (e->mtx.try_lock()) { lr = MDPSR_OK; break; }
        if (e->gen.load(std::memory_order_acquire) == 0) {
            return dead(MDPSR_ERR_ENTRY_INVALID, MDPSR_FAIL_DEAD_HANDLE);
        }
        if (q.stopping()) return dead(MDPSR_ERR_ENTRY_INVALID, MDPSR_FAIL_DEAD_HANDLE);
        if (e->mtx.try_lock_for(std::chrono::milliseconds(MDPSR_LOCK_WAIT_MS))) { lr = MDPSR_OK; break; }
        if (e->gen.load(std::memory_order_acquire) == 0) {
            return dead(MDPSR_ERR_ENTRY_INVALID, MDPSR_FAIL_DEAD_HANDLE);
        }
        if (q.stopping()) return dead(MDPSR_ERR_ENTRY_INVALID, MDPSR_FAIL_DEAD_HANDLE);
        lr = MDPSR_ERR_BUSY;
    }
    if (lr != MDPSR_OK) {
        return dead(MDPSR_ERR_BUSY, MDPSR_FAIL_REJECTED);
    }
    q.mark("d:locked");

    const uint32_t g = e->gen.load(std::memory_order_acquire);
    if (g == 0) {                       /* 等锁期间被卸载了 -> 读 ptr 之前就退出 */
        e->mtx.unlock();
        return dead(MDPSR_ERR_ENTRY_INVALID, MDPSR_FAIL_DEAD_HANDLE);
    }
    void* p = e->ptr.load(std::memory_order_acquire);
    if (!p) {
        e->mtx.unlock();
        return dead(MDPSR_ERR_ENTRY_INVALID, MDPSR_FAIL_DEAD_HANDLE);
    }

    auto* hd = static_cast<HandleDesc*>(p);
    if (!hd->fn) {
        e->mtx.unlock();
        return dead(MDPSR_ERR_BAD_STATE, MDPSR_FAIL_INTERNAL);
    }

    mdpsr_ctx ctx{};
    ctx.handle     = f.dst;
    ctx.plugin     = hd->plugin;
    ctx.plugin_gen = hd->plugin_gen;
    ctx.host       = &_api;

    mdpsr_msg msg{};
    msg.dst = f.dst;
    msg.src = f.src;
    msg.cmd = f.cmd;
    msg.len = static_cast<int32_t>(MDPSR_MSG_HEADER + f.body_len);

    int rc = MDPSR_ERR_CREATE_FAILED;
    {
        CallScope scope(hd->plugin, f.dst, e->name);
        q.mark("d:plugin-call");
        try {
            rc = hd->fn(&msg, f.body, f.body_len, &ctx);
        } catch (const std::exception& ex) {
            log(2, std::string("handle '") + e->name + "' 抛异常: " + ex.what());
            rc = MDPSR_ERR_BAD_STATE;
        } catch (...) {
            log(2, std::string("handle '") + e->name + "' 抛未知异常");
            rc = MDPSR_ERR_BAD_STATE;
        }
        if (scope.leaked()) {
            t_leaked.fetch_add(scope.leaked(), std::memory_order_relaxed);
            log(1, std::string("handle '") + e->name + "' 漏还了 " +
                   std::to_string(scope.leaked()) + " 个资源 (宿主已兜底)");
        }
    }

    e->mtx.unlock();                    /* 调用一返回就放锁 */
    q.mark("d:unlocked");
    t_dispatched.fetch_add(1, std::memory_order_relaxed);

    /* ★ 插件借不到资源 -> 它返回 BUSY。这不是失败, 是"现在忙, 等会儿再来"。
     * 把消息原样回滚到同一条队列 (锁已经放掉了, 入队只需要队列自己那把锁),
     * 下轮再试 —— 持有者是调用期间持有, 有界, 所以回滚一定会终止。 */
    if (rc == MDPSR_ERR_BUSY) {
        t_busy_requeue.fetch_add(1, std::memory_order_relaxed);
        q.mark("d:requeue-busy");
        const uint8_t* body = f.body;
        const uint32_t blen = f.body_len;
        emit_to(q.key(), f.dst, f.src, f.cmd, body, blen);
        return MDPSR_ERR_BUSY;
    }

    if (rc != MDPSR_OK) {
        t_failed.fetch_add(1, std::memory_order_relaxed);
        q.mark("d:fail-log");
        log(1, std::string("handle '") + e->name + "' 返回 " + std::to_string(rc) +
               " (cmd=" + std::to_string(f.cmd) + ", body " + std::to_string(f.body_len) + "B)");
    }
    q.mark("d:done");
    return rc;
}

/* ==========================================================================
 *  看门狗 —— 卡住时唯一的抓手
 * ==========================================================================*/
void Runtime::watchdog_dump() {
    std::string s = "[watchdog] 现在的状态:\n";

    s += "   插件:\n";
    {
        std::lock_guard<std::mutex> lk(_plg_mtx);
        if (_records.empty()) s += "      (一个也没有)\n";
        for (const auto& kv : _records) {
            PluginRecord* rec = kv.second;
            const char* st = "?";
            switch (rec->status.load(std::memory_order_acquire)) {
            case MDPSR_PLUGIN_EMPTY:     st = "EMPTY"; break;
            case MDPSR_PLUGIN_LOADING:   st = "LOADING"; break;
            case MDPSR_PLUGIN_READY:     st = "READY"; break;
            case MDPSR_PLUGIN_UNLOADING: st = "UNLOADING"; break;
            case MDPSR_PLUGIN_FAILED:    st = "FAILED"; break;
            default: break;
            }
            s += "      ";
            s += rec->name;
            s += " gen=" + std::to_string(rec->gen) + " " + st +
                 " 条目=" + std::to_string(rec->entries.size()) + "\n";
        }
    }

    s += "   被握着的条目:\n";
    _reg.dump_held(&s);

    s += "   队列:\n";
    {
        std::shared_lock<std::shared_mutex> lk(_q_mtx);
        for (Queue* q : _queues) {
            mdpsr_queue_stats stq{};
            q->stats(&stq);
            s += "      ";
            s += q->name();
            s += " used=" + std::to_string(stq.used) + "/" + std::to_string(stq.capacity) +
                 " pushed=" + std::to_string(stq.pushed) + " popped=" + std::to_string(stq.popped) +
                 " full=" + std::to_string(stq.full) + " paced=" + std::to_string(stq.paced) +
                 " tid=" + std::to_string(q->thread_id()) +
                 " where=" + (q->where() ? q->where() : "?") + "\n";
        }
    }
    s += "   累计: 借 " + std::to_string(_reg.total_acquires()) +
         " / 忙退 " + std::to_string(_reg.total_busy()) +
         " / 越序 " + std::to_string(_reg.total_order_violations()) +
         " / 漏还 " + std::to_string(_reg.total_leaks());

    log(0, s);
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
    const Json& pl = cfg["plugin"];
    if (!pl.is_array()) {
        log(2, "config.json 的 plugin 必须是一个数组");
        return MDPSR_ERR_BAD_CONFIG;
    }

    mdpsr_install_opts o{};
    o.struct_size = sizeof(o);
    o.flags = MDPSR_INSTALL_SYNC_INIT;

    int ok = 0, bad = 0;
    for (const Json& e : pl.elements()) {
        if (!e.is_string()) continue;
        const std::string rel = e.as_string();
        if (rel.empty()) continue;
        uint64_t k = 0;
        const int rc = plugin_install(rel, &o, &k);
        if (rc == MDPSR_OK) { ++ok; }
        else {
            ++bad;
            log(2, "装载失败 (" + std::to_string(rc) + "): " + rel);
        }
    }
    log(0, "引导完成: 成功 " + std::to_string(ok) + " / 失败 " + std::to_string(bad) +
           " (装载顺序就是依赖顺序: 前面的先装好, 后面的才看得到)");
    /* 声明了却装不上 = 配置错误。宁可现在失败, 也别带着半个系统往下跑。 */
    return bad ? MDPSR_ERR_PLUGIN_LOAD : MDPSR_OK;
}

/* ==========================================================================
 *  收尾
 * ==========================================================================*/
void Runtime::stop_all_queues() {
    std::vector<Queue*> snapshot;
    {
        std::unique_lock<std::shared_mutex> lk(_q_mtx);
        snapshot = _queues;
    }
    for (Queue* q : snapshot) q->request_stop();
    for (Queue* q : snapshot) q->join();     /* 线程先停干净, 再谈 FreeLibrary */
}

void Runtime::shutdown() {
    log(0, "[shutdown] 1/3 停队列线程");
    stop_all_queues();

    log(0, "[shutdown] 2/3 反序卸载插件");
    std::vector<uint64_t> keys;
    {
        std::lock_guard<std::mutex> lk(_plg_mtx);
        for (PluginRecord* rec : _load_order) keys.push_back(rec->key);
    }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        const int rc = plugin_uninstall(*it, 0);
        if (rc != MDPSR_OK) log(1, "收尾时卸载 " + key_name(*it) + " 返回 " + std::to_string(rc));
    }

    log(0, "[shutdown] 3/3 汇总: 入队 " + std::to_string(t_emitted.load()) +
           " / 分发 " + std::to_string(t_dispatched.load()) +
           " / 死信 " + std::to_string(t_dead.load()) +
           " / handle 失败 " + std::to_string(t_failed.load()) +
           " / 队列满 " + std::to_string(t_queue_full.load()) +
           " / 卸载时丢弃 " + std::to_string(t_dropped.load()) +
           " / 忙回滚 " + std::to_string(t_busy_requeue.load()) +
           " / 兜底还锁 " + std::to_string(t_leaked.load()) +
           " / 装卸 " + std::to_string(t_manage.load()));
}

} /* namespace mdpsr */
