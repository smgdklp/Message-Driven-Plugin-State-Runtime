/* ============================================================================
 *  mdpsr/runtime/registry.cpp
 *
 *  锁序总结 (整个运行时的死锁自由性都靠这三条):
 *
 *    A. _mtx (shared_mutex, 保护表结构) 绝不在等待 Entry::mtx 的时候持有。
 *       attach 里那个 try_lock 是不阻塞的, 所以也不构成"等待边"。
 *    B. Entry::mtx 的阻塞等待只发生在 acquire 里, 而 acquire 强制"按键升序"。
 *       于是任何线程持有 e1<e2<... 时只会去等一个更大的键 —— 等待图没有环。
 *    C. 宿主分发时先锁 handle (此时该线程手上一个 Entry 锁都没有), 所以
 *       "等 handle" 这条边也不参与任何环。
 *
 *  推论: 卸载路径的 hold_all 只用 try_lock, 永远不阻塞, 也就不可能被卷进环里;
 *  它最坏的结果是返回 BUSY 让调用方下轮再来, 而不会是死锁。
 * ==========================================================================*/
#include "registry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace mdpsr {

#ifdef MDPSR_DEBUG
void dbg_assert_fail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "\n[mdpsr][ASSERT] %s  (%s:%d)\n", expr, file, line);
    std::fflush(stderr);
    ::abort();
}
#endif

/* ==========================================================================
 *  线程本地台账
 * ==========================================================================*/
static thread_local Tls g_tls;
Tls& tls() { return g_tls; }

CallScope::CallScope(uint64_t plugin, uint64_t handle, const char* what) : _what(what) {
    Tls& t = tls();
    t.frames.push_back(t.held.size());
    t.prev_plugin.push_back(t.cur_plugin);
    t.prev_handle.push_back(t.cur_handle);
    t.cur_plugin = plugin;
    t.cur_handle = handle;
    t.depth++;
}

CallScope::~CallScope() {
    Tls& t = tls();
    if (t.frames.empty()) return;                 /* 不该发生 */
    const size_t base = t.frames.back();

    /* 兜底: 插件漏还的锁全部替它还掉。
     * 这是"漏锁不会把运行时卡死"的保证 —— 宁可日志里多一条 WARN。 */
    int leaked = 0;
    while (t.held.size() > base) {
        Entry* e = t.held.back().e;
        t.held.pop_back();
        if (e) e->mtx.unlock();
        ++leaked;
    }
    _leaked = leaked;

    t.frames.pop_back();
    if (t.depth > 0) --t.depth;
    if (!t.prev_plugin.empty()) { t.cur_plugin = t.prev_plugin.back(); t.prev_plugin.pop_back(); }
    if (!t.prev_handle.empty()) { t.cur_handle = t.prev_handle.back(); t.prev_handle.pop_back(); }
}

/* ==========================================================================
 *  Registry
 * ==========================================================================*/
Registry::Registry(std::pmr::memory_resource* arena) : _arena(arena) {}

Registry::~Registry() {
    /* Entry 永不回收, 这里只把池子交还。 */
    _all.clear();
    _tbl.clear();
}

Entry* Registry::find_locked(uint64_t key) {
    auto it = _tbl.find(key);
    return it == _tbl.end() ? nullptr : it->second;
}

Entry* Registry::intern_locked(uint64_t key) {
    Entry* e = find_locked(key);
    if (e) return e;

    void* mem = _arena->allocate(sizeof(Entry), alignof(Entry));
    if (!mem) return nullptr;
    e = new (mem) Entry();
    e->key = key;
    _tbl.emplace(key, e);
    _all.push_back(e);
    return e;
}

Entry* Registry::find(uint64_t key) {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    return find_locked(key);
}

int Registry::attach(uint64_t key, int kind, uint64_t plugin, uint32_t owner_gen,
                     const char* name, void* ptr, mdpsr_destroy_fn destroy,
                     const mdpsr_factory_ctx* fctx, bool published, Entry** out) {
    if (!key) return MDPSR_ERR_BAD_CONFIG;
    if (kind < 0 || kind >= MDPSR_KIND__COUNT) return MDPSR_ERR_TYPE_MISMATCH;

    Entry* e = nullptr;
    {
        std::unique_lock<std::shared_mutex> lk(_mtx);
        e = intern_locked(key);
        if (!e) return MDPSR_ERR_NO_SPACE;

        if (e->gen.load(std::memory_order_acquire) != 0) return MDPSR_ERR_ALREADY_EXISTS;

        /* 键存在但已失效 -> 复用这个 Entry (它永不消失, 正好当锚点)。
         * 必须确认"上一代的锁"已经全部归还: try_lock 拿不到就说明还有人握着,
         * 那就不能顶替 —— 直接 BUSY, 让调用方稍后再来。 */
        if (!e->mtx.try_lock()) return MDPSR_ERR_BUSY;
        e->mtx.unlock();

        e->kind        = kind;
        e->borrowable  = (kind == MDPSR_KIND_STATE || kind == MDPSR_KIND_OBJECT);
        e->plugin      = plugin;
        e->owner_gen   = owner_gen;
        e->destroy     = destroy;
        e->fctx        = fctx;
        e->acquires.store(0, std::memory_order_relaxed);
        e->busy.store(0, std::memory_order_relaxed);
        std::snprintf(e->name, sizeof(e->name), "%s", name ? name : "");
        e->ptr.store(ptr, std::memory_order_release);
        /* ★ gen 先立起来 (卸载要靠它表达"这一代"), published 才决定"能不能借"。
         * 顺序: ptr -> published -> gen。分发路径先读 gen 再读 ptr, 而"能不能借"
         * 只由 acquire 检查, 所以这里把 published 放在 gen 之前写。 */
        e->published.store(published, std::memory_order_release);
        e->gen.store(owner_gen ? owner_gen : 1u, std::memory_order_release);
    }
    if (out) *out = e;
    return MDPSR_OK;
}

int Registry::publish(const std::vector<uint64_t>& keys) {
    int n = 0;
    for (uint64_t k : keys) {
        Entry* e = find(k);
        if (!e) continue;
        e->published.store(true, std::memory_order_release);
        ++n;
    }
    return n;
}

int Registry::detach(uint64_t key) {
    std::unique_lock<std::shared_mutex> lk(_mtx);
    Entry* e = find_locked(key);
    if (!e) return MDPSR_ERR_KEY_MISSING;
    e->published.store(false, std::memory_order_release);   /* 先下线, 再失效 */
    e->gen.store(0, std::memory_order_release);
    e->ptr.store(nullptr, std::memory_order_release);
    e->kind = -1;
    e->borrowable = false;
    return MDPSR_OK;
}

int Registry::invalidate(uint64_t key) {
    Entry* e = find(key);
    if (!e) return MDPSR_ERR_KEY_MISSING;
    /* 只翻 gen。ptr 先留着 —— 卸载路径还要靠它去调析构。 */
    e->gen.store(0, std::memory_order_release);
    return MDPSR_OK;
}

int Registry::revalidate(uint64_t key, uint32_t gen) {
    Entry* e = find(key);
    if (!e) return MDPSR_ERR_KEY_MISSING;
    e->gen.store(gen ? gen : 1u, std::memory_order_release);
    return MDPSR_OK;
}

size_t Registry::size() {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    size_t n = 0;
    for (Entry* e : _all) if (e->gen.load(std::memory_order_acquire) != 0) ++n;
    return n;
}

uint32_t Registry::gen_of(uint64_t key) {
    Entry* e = find(key);
    if (!e) return 0u;
    /* 还没上线的条目对别人而言"不存在" —— 探活问的是"服务在不在",
     * 而半装好的东西不算服务。 */
    if (!e->published.load(std::memory_order_acquire)) return 0u;
    return e->gen.load(std::memory_order_acquire);
}

int Registry::kind_of(uint64_t key) {
    Entry* e = find(key);
    if (!e) return -1;
    if (!e->published.load(std::memory_order_acquire)) return -1;
    if (e->gen.load(std::memory_order_acquire) == 0) return -1;
    return e->kind;
}

int Registry::name_of(uint64_t key, char* out, uint32_t cap) {
    if (!out || cap == 0) return MDPSR_ERR_NULLPTR;
    out[0] = '\0';
    /* 名字是不可变的 (只在 attach 时写), 所以读一次快照就够 —— 不用二次上锁。 */
    Entry* e = find(key);
    if (!e) return MDPSR_ERR_KEY_MISSING;
    std::snprintf(out, cap, "%s", e->name);
    return MDPSR_OK;
}

bool Registry::name_taken(const char* name, uint64_t except_key) {
    if (!name || !*name) return false;
    std::shared_lock<std::shared_mutex> lk(_mtx);
    for (Entry* e : _all) {
        if (e->key == except_key) continue;
        if (e->gen.load(std::memory_order_acquire) == 0) continue;
        if (std::strcmp(e->name, name) == 0) return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 *  借 / 还
 * ------------------------------------------------------------------------*/
int Registry::lock_bounded(Entry* e) {
    if (!e) return MDPSR_ERR_KEY_MISSING;
    for (int i = 0; i < MDPSR_LOCK_TRIES; ++i) {
        if (e->mtx.try_lock()) return MDPSR_OK;
        /* 拿不到就先看 gen: 卸载方是"先置 0, 再锁", 所以它握着锁的时候
         * gen 一定是 0 —— 这种情况立刻失败返回, 绝不阻塞。 */
        if (e->gen.load(std::memory_order_acquire) == 0) return MDPSR_ERR_ENTRY_INVALID;
        if (e->mtx.try_lock_for(std::chrono::milliseconds(MDPSR_LOCK_WAIT_MS))) return MDPSR_OK;
        if (e->gen.load(std::memory_order_acquire) == 0) return MDPSR_ERR_ENTRY_INVALID;
    }
    e->busy.fetch_add(1, std::memory_order_relaxed);
    return MDPSR_ERR_BUSY;
}

int Registry::acquire(uint64_t key, uint32_t want_gen, void** out_ptr, uint32_t* out_gen) {
    if (!key) return MDPSR_ERR_KEY_MISSING;

    Tls& t = tls();

    /* 自己已经拿着了 -> 再锁一次就是自锁死。这在 v1 里是个隐形陷阱
     * ("handle 不许锁自己声明的 state"), v2 直接变成一条明确的错误码。 */
    if (t.find_held(key)) return MDPSR_ERR_BUSY;

    Entry* e = find(key);
    if (!e) return MDPSR_ERR_KEY_MISSING;
    if (!e->borrowable) return MDPSR_ERR_TYPE_MISMATCH;

    /* ★ 上线门闸: 条目挂着但插件还没 READY (= 装载中途) 时谁也借不到。
     * 这一条把"装载失败回滚要整池回收"变成安全的: 不可能有人手里握着
     * 一个半装好的载荷 —— 见 Entry::published。 */
    if (!e->published.load(std::memory_order_acquire)) return MDPSR_ERR_NOT_READY;

    /* Object 是插件私有的, State 才是公用的。
     * v1 声称 object 私有, 却把整张表交给所有人; v2 在这里真的拦一下。 */
    if (e->kind == MDPSR_KIND_OBJECT) {
        const uint64_t cp = t.cur_plugin;
        if (cp != 0 && e->plugin != cp) return MDPSR_ERR_NO_PERM;
    }

    uint32_t g = e->gen.load(std::memory_order_acquire);
    if (!g) return MDPSR_ERR_ENTRY_INVALID;

    /* 规则: 同一个线程里, 后借的键必须比手上所有键都大。
     * 违反就拒绝 —— 拒绝总比死锁好, 而且错误里能说清是谁跟谁。 */
    const uint64_t mx = t.max_held_key();
    if (mx != 0 && key < mx) {
        _order_bad.fetch_add(1, std::memory_order_relaxed);
        return MDPSR_ERR_LOCK_ORDER;
    }

    /* ★ 用有界等待拿锁, 而不是死等。
     *
     * 卸载方的顺序是固定的:
     *      置 gen = 0  ->  独占全部条目  ->  停队列 / join 队列线程  ->  卸 dll
     * 所以"有人正握着这把锁"通常意味着 gen 已经是 0, lock_bounded 会立刻返回
     * ENTRY_INVALID。但万一遇到"锁被握着而 gen 还没翻"的窗口, 无界等待就会
     * 卡在卸载方手里那把锁上 —— 而它正在 join 我们所在的队列线程, 直接死锁。
     * 加上界之后, 最坏也只是 BUSY, 调用方丢弃这条消息, 线程回到循环顶部。 */
    const int lr = lock_bounded(e);
    if (lr != MDPSR_OK) {
        if (lr == MDPSR_ERR_BUSY) _busy.fetch_add(1, std::memory_order_relaxed);
        return lr;
    }

    g = e->gen.load(std::memory_order_acquire);      /* 拿到锁后复查 */
    if (!g) { e->mtx.unlock(); return MDPSR_ERR_ENTRY_INVALID; }
    if (want_gen != 0 && want_gen != g) { e->mtx.unlock(); return MDPSR_ERR_STALE; }

    void* p = e->ptr.load(std::memory_order_acquire);
    if (!p) { e->mtx.unlock(); return MDPSR_ERR_ENTRY_INVALID; }

    t.held.push_back(Tls::Held{ key, e });
    e->acquires.fetch_add(1, std::memory_order_relaxed);
    _acquires.fetch_add(1, std::memory_order_relaxed);

    if (out_ptr) *out_ptr = p;
    if (out_gen) *out_gen = g;
    return MDPSR_OK;
}

int Registry::acquire_many(const uint64_t* keys, const uint32_t* gens, uint32_t n,
                           void** out_ptrs, uint32_t* out_gens) {
    if (!keys || n == 0) return MDPSR_ERR_NULLPTR;
    if (n > 64) return MDPSR_ERR_OUT_OF_RANGE;

    /* 按 (键, 原序号) 排序后再借, 保证升序; 全有或全无。 */
    uint32_t idx[64];
    for (uint32_t i = 0; i < n; ++i) idx[i] = i;
    std::sort(idx, idx + n, [&](uint32_t a, uint32_t b) {
        if (keys[a] != keys[b]) return keys[a] < keys[b];
        return a < b;
    });

    Tls& t = tls();
    const size_t mark = t.held.size();
    int rc = MDPSR_OK;

    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = idx[i];
        void* p = nullptr;
        uint32_t g = 0;
        rc = acquire(keys[k], gens ? gens[k] : 0u, &p, &g);
        if (rc != MDPSR_OK) break;
        if (out_ptrs) out_ptrs[k] = p;
        if (out_gens) out_gens[k] = g;
    }
    if (rc != MDPSR_OK) {
        while (t.held.size() > mark) {              /* 回滚这一批 */
            Entry* e = t.held.back().e;
            t.held.pop_back();
            if (e) e->mtx.unlock();
        }
    }
    return rc;
}

int Registry::release(uint64_t key) {
    Tls& t = tls();
    for (size_t i = t.held.size(); i-- > 0; ) {
        if (t.held[i].key != key) continue;
        Entry* e = t.held[i].e;
        t.held.erase(t.held.begin() + static_cast<ptrdiff_t>(i));
        if (e) e->mtx.unlock();
        return MDPSR_OK;
    }
    return MDPSR_ERR_ENTRY_INVALID;                 /* 没借过 / 已经还了 */
}

void Registry::dump_held(std::string* out) {
    if (!out) return;
    static const char* kinds[] = { "object", "state ", "handle", "queue " };
    std::shared_lock<std::shared_mutex> lk(_mtx);
    int held = 0;
    for (Entry* e : _all) {
        /* ★ 注意: gen==0 的条目也要看 —— 卸载中途"已经把 gen 置 0, 但锁还没放"
         * 正是最需要被看见的状态, 漏掉它等于把这个工具最大的盲区留给自己。 */
        const bool free_now = e->mtx.try_lock();
        if (free_now) e->mtx.unlock();
        if (free_now) continue;
        ++held;
        const char* k = (e->kind >= 0 && e->kind < 4) ? kinds[e->kind] : "??????";
        *out += "      HELD  ";
        *out += k;
        *out += " '";
        *out += e->name;
        *out += "' (gen ";
        *out += std::to_string(e->gen.load(std::memory_order_acquire));
        *out += ")\n";
    }
    if (!held) *out += "      (没有任何条目被握着)\n";
}

void Registry::release_all() {
    Tls& t = tls();
    const size_t base = t.base();
    while (t.held.size() > base) {
        Entry* e = t.held.back().e;
        t.held.pop_back();
        if (e) e->mtx.unlock();
    }
}

int Registry::held_count() {
    return static_cast<int>(tls().held.size());
}

/* --------------------------------------------------------------------------
 *  卸载路径的独占
 * ------------------------------------------------------------------------*/
void Registry::Hold::release() {
    for (auto it = _held.rbegin(); it != _held.rend(); ++it) {
        if (*it) (*it)->mtx.unlock();
    }
    _held.clear();
    _ok = false;
}

int Registry::hold_all(const std::vector<uint64_t>& keys, Hold* out) {
    return hold_filtered(keys, -1, true, out);
}
int Registry::hold_kind(const std::vector<uint64_t>& keys, int kind, Hold* out) {
    return hold_filtered(keys, kind, true, out);
}
int Registry::hold_except_kind(const std::vector<uint64_t>& keys, int kind, Hold* out) {
    return hold_filtered(keys, kind, false, out);
}

int Registry::hold_filtered(const std::vector<uint64_t>& keys, int kind, bool want, Hold* out) {
    out->_held.clear();
    out->_ok = false;

    std::vector<Entry*> es;
    es.reserve(keys.size());
    for (uint64_t k : keys) {
        Entry* e = find(k);
        if (!e) continue;
        if (kind >= 0 && ((e->kind == kind) != want)) continue;
        es.push_back(e);
    }
    std::sort(es.begin(), es.end(), [](Entry* a, Entry* b) { return a->key < b->key; });
    es.erase(std::unique(es.begin(), es.end()), es.end());

    for (Entry* e : es) {
        if (!e->mtx.try_lock()) {                   /* 不阻塞 —— 所以不可能参与死锁环 */
            out->release();
            _busy.fetch_add(1, std::memory_order_relaxed);
            return MDPSR_ERR_BUSY;
        }
        out->_held.push_back(e);
    }
    out->_ok = true;
    return MDPSR_OK;
}

/* --------------------------------------------------------------------------
 *  池
 * ------------------------------------------------------------------------*/
Pool* Registry::pool_new(uint64_t owner) {
    void* mem = _arena->allocate(sizeof(Pool), alignof(Pool));
    if (!mem) return nullptr;
    Pool* p = new (mem) Pool(owner);
    return p;
}

void Registry::pool_delete(Pool* p) {
    if (!p) return;
    /* 整池回收: 这个插件分配过的一切内存在这里一次性还给系统。
     * 这是"卸载 = 真回收"的地方 (v1 在这里什么都没做)。 */
    p->res.release();
    p->~Pool();
    /* Pool 结构本身留在 arena 里 (arena 只增不减), 免得留下悬垂的 mdpsr_pool*。 */
}

void* Registry::pool_alloc(Pool* p, size_t bytes, size_t align) {
    if (!p || bytes == 0) return nullptr;
    void* q = p->res.allocate(bytes, align);
    if (q) {
        p->allocs.fetch_add(1, std::memory_order_relaxed);
        p->live.fetch_add(1, std::memory_order_relaxed);
    }
    return q;
}

void Registry::pool_free(Pool* p, void* q, size_t bytes, size_t align) {
    if (!p || !q) return;
    p->res.deallocate(q, bytes, align);
    p->frees.fetch_add(1, std::memory_order_relaxed);
    p->live.fetch_sub(1, std::memory_order_relaxed);
}

} /* namespace mdpsr */
