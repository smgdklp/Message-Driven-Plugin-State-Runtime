/* ============================================================================
 *  mdpsr/scr/main/queue_map.cpp —— Pool / Queue / Map 实现
 * ==========================================================================*/
#include "queue_map.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace mdpsr {

/* ==========================================================================
 *  Pools
 * ==========================================================================*/
Pools::Pools()
    : _msg(new std::pmr::synchronized_pool_resource())
    , _map(new std::pmr::synchronized_pool_resource())
    , _state(new std::pmr::synchronized_pool_resource()) {
}

Pools::~Pools() = default;

std::pmr::memory_resource* Pools::dll_pool(uint64_t dll) {
    std::lock_guard<std::mutex> lk(_dll_mtx);
    auto it = _dll.find(dll);
    if (it != _dll.end()) return it->second.get();
    auto res = std::make_unique<std::pmr::synchronized_pool_resource>();
    std::pmr::memory_resource* raw = res.get();
    _dll.emplace(dll, std::move(res));
    return raw;
}

std::pmr::memory_resource* Pools::dll_pool_find(uint64_t dll) const {
    std::lock_guard<std::mutex> lk(_dll_mtx);
    auto it = _dll.find(dll);
    return it == _dll.end() ? nullptr : it->second.get();
}

bool Pools::dll_pool_drop(uint64_t dll) {
    std::unique_ptr<std::pmr::synchronized_pool_resource> victim;
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        auto it = _dll.find(dll);
        if (it == _dll.end()) return false;
        victim = std::move(it->second);
        _dll.erase(it);
    }
    victim.reset();     /* 整池释放 */
    return true;
}

/* ==========================================================================
 *  Queue
 * ==========================================================================*/
Queue::Queue(std::pmr::memory_resource* res, size_t initial)
    : _buf(res)
    , _scratch(res) {
    if (initial < 64) initial = 64;
    _buf.resize(initial);
}

uint8_t Queue::peek_at(size_t offset) const {
    return _buf[(_head + offset) % _buf.size()];
}

void Queue::ensure(size_t need) {
    if (_buf.size() >= need) return;
    size_t newcap = _buf.empty() ? 4096 : _buf.size();
    while (newcap < need) newcap *= 2;
    std::pmr::vector<uint8_t> nb(_buf.get_allocator().resource());
    nb.resize(newcap);
    for (size_t i = 0; i < _size; ++i) nb[i] = peek_at(i);
    _buf.swap(nb);
    _head = 0;
}

void Queue::write_bytes(const uint8_t* p, size_t n) {
    if (n == 0) return;
    const size_t cap = _buf.size();
    const size_t pos = (_head + _size) % cap;
    const size_t first = (std::min)(n, cap - pos);
    std::memcpy(_buf.data() + pos, p, first);
    if (n > first) std::memcpy(_buf.data(), p + first, n - first);
    _size += n;
}

void Queue::read_bytes(uint8_t* p, size_t n) {
    if (n == 0) return;
    const size_t cap = _buf.size();
    const size_t first = (std::min)(n, cap - _head);
    if (p) std::memcpy(p, _buf.data() + _head, first);
    if (n > first && p) std::memcpy(p + first, _buf.data(), n - first);
    _head = (_head + n) % cap;
    _size -= n;
}

void Queue::skip_bytes(size_t n) {
    if (n == 0) return;
    const size_t cap = _buf.size();
    _head = (_head + n) % cap;
    _size -= n;
}

void Queue::push(uint64_t handle, const void* body, size_t len) {
    std::lock_guard<std::mutex> lk(_mtx);
    const int32_t total = static_cast<int32_t>(MDPSR_MSG_HEADER + len);
    ensure(_size + static_cast<size_t>(total));
    uint8_t hdr[MDPSR_MSG_HEADER];
    std::memcpy(hdr, &total, 4);
    std::memcpy(hdr + 4, &handle, 8);
    write_bytes(hdr, MDPSR_MSG_HEADER);
    if (len) write_bytes(static_cast<const uint8_t*>(body), len);
    _pushed++;
}

bool Queue::push_frame(const uint8_t* frame, size_t n) {
    if (!frame || n < MDPSR_MSG_HEADER) return false;
    int32_t len = 0;
    uint64_t handle = 0;
    std::memcpy(&len, frame, 4);
    std::memcpy(&handle, frame + 4, 8);
    if (len < static_cast<int32_t>(MDPSR_MSG_HEADER)) return false;
    if (static_cast<size_t>(len) != n) return false;    /* 十六进制列表必须给完整一帧 */
    push(handle, frame + MDPSR_MSG_HEADER, n - MDPSR_MSG_HEADER);
    return true;
}

bool Queue::pop(uint64_t* handle, const uint8_t** body, size_t* len) {
    std::lock_guard<std::mutex> lk(_mtx);
    if (_size < MDPSR_MSG_HEADER) return false;

    uint8_t hdr[MDPSR_MSG_HEADER];
    for (size_t i = 0; i < MDPSR_MSG_HEADER; ++i) hdr[i] = peek_at(i);

    int32_t total = 0;
    std::memcpy(&total, hdr, 4);
    if (total < static_cast<int32_t>(MDPSR_MSG_HEADER) || total > (64 << 20)) {
        /* 帧头损坏: 丢弃整个队列内容, 避免死循环 */
        _head = 0;
        _size = 0;
        return false;
    }
    if (_size < static_cast<size_t>(total)) return false;   /* 还没积累出完整消息 */

    uint64_t h = 0;
    std::memcpy(&h, hdr + 4, 8);

    skip_bytes(MDPSR_MSG_HEADER);
    const size_t blen = static_cast<size_t>(total) - MDPSR_MSG_HEADER;
    _scratch.resize(blen ? blen : 1);
    if (blen) read_bytes(_scratch.data(), blen);

    if (handle) *handle = h;
    if (body)   *body = _scratch.data();
    if (len)    *len = blen;
    _popped++;
    return true;
}

size_t Queue::readable() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _size;
}

/* ==========================================================================
 *  Map
 * ==========================================================================*/
Map::Map(std::pmr::memory_resource* res)
    : _res(res)
    , _table(res) {
}

Map::~Map() {
    for (auto& kv : _table) {
        if (kv.second) {
            kv.second->~Countptr();
            _res->deallocate(kv.second, sizeof(Countptr), alignof(Countptr));
        }
    }
}

int Map::reg(const mdpsr_entry& e) {
    std::lock_guard<std::mutex> lk(_mtx);
    if (_table.find(e.key) != _table.end()) return MDPSR_ERR_ALREADY_EXISTS;

    void* mem = _res->allocate(sizeof(Countptr), alignof(Countptr));
    Countptr* cp = new (mem) Countptr{};
    cp->ptr         = e.ptr;
    cp->valid       = 1;
    cp->type        = e.type;
    cp->dll         = e.dll;
    cp->bind_object = e.bind_object;
    cp->bind_queue  = e.bind_queue;
    uint32_t bc = e.bind_state_count;
    if (bc > MDPSR_MAX_BIND_STATES) bc = MDPSR_MAX_BIND_STATES;
    cp->bind_state_count = 0;
    for (uint32_t i = 0; i < bc; ++i) {
        if (!e.bind_states[i]) continue;
        cp->bind_states[cp->bind_state_count++] = e.bind_states[i];
    }
    std::snprintf(cp->name, sizeof(cp->name), "%s", e.name ? e.name : "");
    _table.emplace(e.key, cp);
    return MDPSR_OK;
}

int Map::set_bind_queue(uint64_t key, uint64_t queue_key) {
    std::lock_guard<std::mutex> lk(_mtx);
    auto it = _table.find(key);
    if (it == _table.end()) return MDPSR_ERR_MAP_KEY_MISSING;
    it->second->bind_queue = queue_key;
    return MDPSR_OK;
}

int Map::acquire(uint64_t key, bool write, Countptr** out) {
    std::lock_guard<std::mutex> lk(_mtx);
    auto it = _table.find(key);
    if (it == _table.end()) return MDPSR_ERR_MAP_KEY_MISSING;
    Countptr* cp = it->second;
    if (!cp->valid) return MDPSR_ERR_MAP_INVALID;
    if (write) cp->w_count++;
    else       cp->r_count++;
    if (out) *out = cp;
    return MDPSR_OK;
}

int Map::release(uint64_t key, bool write) {
    std::lock_guard<std::mutex> lk(_mtx);
    auto it = _table.find(key);
    if (it == _table.end()) return MDPSR_ERR_MAP_KEY_MISSING;
    Countptr* cp = it->second;
    if (write) cp->w_count = (cp->w_count > 0) ? cp->w_count - 1 : 0;
    else       cp->r_count = (cp->r_count > 0) ? cp->r_count - 1 : 0;
    _cv.notify_all();
    return MDPSR_OK;
}

int Map::invalidate(uint64_t key) {
    std::lock_guard<std::mutex> lk(_mtx);
    auto it = _table.find(key);
    if (it == _table.end()) return MDPSR_ERR_MAP_KEY_MISSING;
    it->second->valid = 0;
    _cv.notify_all();
    return MDPSR_OK;
}

int Map::erase(uint64_t key) {
    std::lock_guard<std::mutex> lk(_mtx);
    auto it = _table.find(key);
    if (it == _table.end()) return MDPSR_ERR_MAP_KEY_MISSING;
    Countptr* cp = it->second;
    cp->valid = 0;
    _table.erase(it);
    cp->~Countptr();
    _res->deallocate(cp, sizeof(Countptr), alignof(Countptr));
    _cv.notify_all();
    return MDPSR_OK;
}

int Map::wait_idle(uint64_t key, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lk(_mtx);
    const bool ok = _cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        auto it = _table.find(key);
        if (it == _table.end()) return true;
        return it->second->r_count == 0 && it->second->w_count == 0;
    });
    return ok ? MDPSR_OK : MDPSR_ERR_TIMEOUT;
}

int Map::dll_idle(uint64_t dll, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lk(_mtx);
    const bool ok = _cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
        for (auto& kv : _table) {
            if (kv.second->dll != dll) continue;
            if (kv.second->r_count != 0 || kv.second->w_count != 0) return false;
        }
        return true;
    });
    return ok ? MDPSR_OK : MDPSR_ERR_TIMEOUT;
}

int Map::snapshot(uint64_t dll, mdpsr_entry_info* out, uint32_t cap, uint32_t* out_count) {
    std::lock_guard<std::mutex> lk(_mtx);
    uint32_t n = 0;
    for (auto& kv : _table) {
        Countptr* cp = kv.second;
        if (dll != 0 && cp->dll != dll) continue;
        if (out && n < cap) {
            mdpsr_entry_info& info = out[n];
            info.key     = kv.first;
            info.dll     = cp->dll;
            info.type    = cp->type;
            info.r_count = cp->r_count;
            info.w_count = cp->w_count;
            info.valid   = cp->valid;
            std::snprintf(info.name, sizeof(info.name), "%s", cp->name);
        }
        n++;
    }
    if (out_count) *out_count = n;
    return MDPSR_OK;
}

size_t Map::collect_classptrs(std::vector<mdpsr_classptr*>& out) {
    std::lock_guard<std::mutex> lk(_mtx);
    out.clear();
    for (auto& kv : _table) {
        Countptr* cp = kv.second;
        if (cp->type == MDPSR_ENTRY_OBJECT && cp->valid && cp->ptr) {
            out.push_back(static_cast<mdpsr_classptr*>(cp->ptr));
        }
    }
    return out.size();
}

size_t Map::keys_of_dll(uint64_t dll, uint64_t* out, uint32_t cap, uint32_t* out_count) {
    std::lock_guard<std::mutex> lk(_mtx);
    size_t n = 0;
    for (auto& kv : _table) {
        if (kv.second->dll != dll) continue;
        if (out && n < cap) out[n] = kv.first;
        ++n;
    }
    if (out_count) *out_count = static_cast<uint32_t>(n);
    return n;
}

size_t Map::live_count() const {
    std::lock_guard<std::mutex> lk(_mtx);
    size_t n = 0;
    for (auto& kv : _table) if (kv.second->valid) n++;
    return n;
}

} /* namespace mdpsr */
