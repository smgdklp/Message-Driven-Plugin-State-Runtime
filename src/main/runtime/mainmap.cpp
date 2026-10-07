/* ============================================================================
 *  mdpsr/runtime/mainmap.cpp
 *  MainMap —— 一张大表 + 四个二级字典
 *
 *  线程模型:
 *      · _mtx (shared_mutex) 保护"表的形状"(查找 / 插入 / 摘除)。
 *        查表可以并发 (共享锁), 插入/摘除互斥 (独占锁)。
 *        它不保护 Ptr 里的内容 —— 那是 Ptr.r/w_count/valid 和 Ptr.mtx 的事。
 *      · 条目一旦 attach 成功就绝不搬动, 所以 mdpsr_ptr* 可以安全地在
 *        Context 里裸传 (生命周期 = 从 attach 到 detach, 而 detach 要求
 *        valid == false 且读写在途为 0)。
 * ==========================================================================*/
#include "runtime.h"

#include <algorithm>`r`n#include <cstring>

namespace mdpsr {

MainMap::MainMap(std::pmr::memory_resource* res)
    : _res(res)
    , _tbl()
    , _names()
    , _object()
    , _state()
    , _handle()
    , _queue() {
}

MainMap::~MainMap() {
    for (mdpsr_ptr* p : _all) free_ptr(p);
    _all.clear();
}

mdpsr_ptr* MainMap::alloc_ptr() {
    void* m = _res->allocate(sizeof(mdpsr_ptr), alignof(mdpsr_ptr));
    if (!m) return nullptr;
    mdpsr_ptr* p = new (m) mdpsr_ptr{};
    /* 原子 + 锁必须显式构造 —— Ptr 会被放进池子里复用 */
    p->ptr = nullptr;
    p->valid.store(0, std::memory_order_relaxed);
    new (p->_mtx_storage) std::mutex();
    return p;
}

void MainMap::free_ptr(mdpsr_ptr* p) {
    if (!p) return;
    ptr_mtx(p)->~mutex();
    p->~mdpsr_ptr();
    _res->deallocate(p, sizeof(mdpsr_ptr), alignof(mdpsr_ptr));
}

void MainMap::sort_keys(uint64_t* keys, size_t n) {
    /* 简单的插入排序: n 通常 <= 8 (handle + 声明的 state/object), 够用且无分配 */
    for (size_t i = 1; i < n; ++i) {
        const uint64_t v = keys[i];
        size_t j = i;
        while (j > 0 && keys[j - 1] > v) { keys[j] = keys[j - 1]; --j; }
        keys[j] = v;
    }
}

HashMap* MainMap::table_of(int kind) {
    switch (kind) {
    case MDPSR_KIND_OBJECT: return &_object;
    case MDPSR_KIND_STATE:  return &_state;
    case MDPSR_KIND_HANDLE: return &_handle;
    case MDPSR_KIND_QUEUE:  return &_queue;
    default:                return nullptr;
    }
}

int MainMap::kind_of(uint64_t key) {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    auto it = _kind.find(key);
    return it == _kind.end() ? -1 : it->second;
}

mdpsr_ptr* MainMap::get(uint64_t key) {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    void** v = _tbl.find(key);
    if (!v || !*v) return nullptr;
    mdpsr_ptr* p = static_cast<mdpsr_ptr*>(*v);
    /* valid == false 一律当作 nullptr —— 这是规格里最硬的一条 */
    if (p->valid.load(std::memory_order_acquire) == 0) return nullptr;
    return p;
}

mdpsr_ptr* MainMap::peek(uint64_t key) {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    void** v = _tbl.find(key);
    return (v && *v) ? static_cast<mdpsr_ptr*>(*v) : nullptr;
}

const char* MainMap::name_of(uint64_t key) {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    void** v = _names.find(key);
    return (v && *v) ? static_cast<const char*>(*v) : "";
}

int MainMap::attach(int kind, uint64_t key, const char* name, void* ptr, mdpsr_ptr** out) {
    if (key == 0) return MDPSR_ERR_BAD_CONFIG;
    HashMap* sub = table_of(kind);
    if (!sub) return MDPSR_ERR_TYPE_MISMATCH;

    std::unique_lock<std::shared_mutex> lk(_mtx);

    void** existing = _tbl.find(key);
    if (existing && *existing) {
        mdpsr_ptr* old = static_cast<mdpsr_ptr*>(*existing);
        if (old->valid.load(std::memory_order_acquire) != 0) {
            return MDPSR_ERR_ALREADY_EXISTS;      /* 还活着, 不许顶替 */
        }
        /* 旧条目已经被置无效了。但"置无效"只在持锁时发生, 而我们没有那把锁 ——
         * 所以这里再试一次锁: 拿不到说明真的还有人握着它, 不能顶替。 */
        if (!try_lock(old)) return MDPSR_ERR_BUSY;
        unlock(old);
        detach_locked(key);
    }

    mdpsr_ptr* p = alloc_ptr();
    if (!p) return MDPSR_ERR_MAP_NO_SPACE;
    p->ptr = ptr;
    p->valid.store(1, std::memory_order_release);

    /* 名字副本进 Pool_map, 生命周期与条目一致 (只给日志/诊断看) */
    if (name && *name) {
        const size_t n = std::strlen(name) + 1;
        void* nb = _res->allocate(n, alignof(char));
        if (nb) {
            std::memcpy(nb, name, n);
            _names.set(key, nb);
        }
    }

    _tbl.set(key, p);
    _kind[key] = kind;
    sub->set(key, p);
    _all.push_back(p);

    if (out) *out = p;
    return MDPSR_OK;
}

int MainMap::detach_locked(uint64_t key) {
    void** v = _tbl.find(key);
    if (!v || !*v) return MDPSR_ERR_MAP_KEY_MISSING;
    mdpsr_ptr* p = static_cast<mdpsr_ptr*>(*v);

    auto it = _kind.find(key);
    if (it != _kind.end()) {
        HashMap* sub = table_of(it->second);
        if (sub) sub->set(key, nullptr);   /* 只置空, 不缩容 (指针稳定性优先) */
        _kind.erase(it);
    }

    void** nv = _names.find(key);
    if (nv && *nv) {
        const char* np = static_cast<const char*>(*nv);
        _res->deallocate(const_cast<char*>(np), std::strlen(np) + 1, alignof(char));
        *nv = nullptr;
    }

    p->valid.store(0, std::memory_order_release);
    p->ptr = nullptr;
    *v = nullptr;                          /* 表里保留 tombstone */
    return MDPSR_OK;
}

int MainMap::detach(uint64_t key) {
    std::unique_lock<std::shared_mutex> lk(_mtx);
    void** v = _tbl.find(key);
    if (!v || !*v) return MDPSR_ERR_MAP_KEY_MISSING;
    return detach_locked(key);
}

int MainMap::invalidate(uint64_t key) {
    mdpsr_ptr* p = nullptr;
    {
        std::shared_lock<std::shared_mutex> lk(_mtx);
        void** v = _tbl.find(key);
        if (!v || !*v) return MDPSR_ERR_MAP_KEY_MISSING;
        p = static_cast<mdpsr_ptr*>(*v);
    }
    /* 只翻一个原子。约定: 调用方必须已经持有这个条目的锁 ——
     * 这样"置无效"就不会和"正在使用"擦肩而过。 */
    p->valid.store(0, std::memory_order_release);
    return MDPSR_OK;
}

int MainMap::is_valid(mdpsr_ptr* p) {
    return (p && p->valid.load(std::memory_order_acquire) != 0) ? 1 : 0;
}

int MainMap::lock(mdpsr_ptr* p) {
    if (!p) return MDPSR_ERR_NULLPTR;
    ptr_mtx(p)->lock();
    return MDPSR_OK;
}

int MainMap::try_lock(mdpsr_ptr* p) {
    if (!p) return MDPSR_ERR_NULLPTR;
    return ptr_mtx(p)->try_lock() ? MDPSR_OK : MDPSR_ERR_BUSY;
}

void MainMap::unlock(mdpsr_ptr* p) {
    if (!p) return;
    ptr_mtx(p)->unlock();
}

int MainMap::enum_keys(uint64_t* out, uint32_t cap, uint32_t* out_count) {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    uint32_t n = 0;
    _tbl.each([&](uint64_t key, void* val) {
        if (!val) return;                       /* 忽略 tombstone */
        if (out && n < cap) out[n] = key;
        ++n;
    });
    if (out_count) *out_count = n;
    return MDPSR_OK;
}

size_t MainMap::size() {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    size_t n = 0;
    _tbl.each([&](uint64_t, void* val) { if (val) ++n; });
    return n;
}

void MainMap::gather(const uint64_t* keys, size_t n, HashMap* out) {
    std::shared_lock<std::shared_mutex> lk(_mtx);
    for (size_t i = 0; i < n; ++i) {
        void** v = _tbl.find(keys[i]);
        if (!v || !*v) continue;
        mdpsr_ptr* p = static_cast<mdpsr_ptr*>(*v);
        if (p->valid.load(std::memory_order_acquire) == 0) continue;
        out->set(keys[i], p);
    }
}

/* ==========================================================================
 *  MultiLock
 *
 *  取锁顺序是**全进程唯一的**: 按键升序。dispatch (拿 handle + 声明的
 *  state/object) 和 unload (拿整个插件的资源) 都走这个顺序, 所以两边
 *  永远不会互相咬住。
 * ==========================================================================*/
int MultiLock::acquire(MainMap& m, const uint64_t* keys, size_t n) {
    release();
    _m = &m;

    std::vector<Item> items;
    for (size_t i = 0; i < n; ++i) {
        if (keys[i] == 0) continue;
        bool dup = false;
        for (const Item& e : items) if (e.key == keys[i]) { dup = true; break; }
        if (dup) continue;

        mdpsr_ptr* p = m.get(keys[i]);
        if (!p) continue;                 /* 这个资源根本没挂上, 跳过 */
        items.push_back(Item{ keys[i], p });
    }

    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.key < b.key; });

    for (const Item& e : items) {
        if (m.try_lock(e.p) != MDPSR_OK) { release(); return MDPSR_ERR_BUSY; }
        /* 拿到锁之后必须复查 valid: 我们可能在等锁的时候被置无效了。
         * 这一步让"卸载"和"正在取锁的调用方"不会擦肩而过。 */
        if (!m.is_valid(e.p)) {
            m.unlock(e.p);
            release();
            return MDPSR_ERR_MAP_INVALID;
        }
        _held.push_back(e.p);
    }
    return MDPSR_OK;
}

int MultiLock::acquire_raw(MainMap& m, std::vector<Item> items) {
    release();
    _m = &m;

    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.key < b.key; });

    for (const Item& e : items) {
        if (!e.p) continue;
        if (m.try_lock(e.p) != MDPSR_OK) { release(); return MDPSR_ERR_BUSY; }
        _held.push_back(e.p);
    }
    _order = std::move(items);
    return MDPSR_OK;
}

void MultiLock::release() {
    if (_m) {
        for (auto it = _held.rbegin(); it != _held.rend(); ++it) _m->unlock(*it);
    }
    _held.clear();
    _order.clear();
    _m = nullptr;
}

bool MultiLock::held(uint64_t key) const {
    for (const Item& e : _order) if (e.key == key) return true;
    return false;
}

} /* namespace mdpsr */
