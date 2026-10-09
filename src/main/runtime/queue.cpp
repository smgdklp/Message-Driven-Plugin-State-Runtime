/* ============================================================================
 *  mdpsr/runtime/queue.cpp
 * ==========================================================================*/
#include "queue.h"
#include "registry.h"
#include "runtime.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace mdpsr {

Queue::Queue(std::pmr::memory_resource* pool, std::string name, uint64_t key, const Desc& d)
    : _buf(pool)
    , _scratch(pool)
    , _name(std::move(name))
    , _key(key)
    , _pace_ms(d.pace_ms) {
    uint32_t cap = d.capacity ? d.capacity : 65536;
    if (cap < MDPSR_MSG_HEADER * 4) cap = MDPSR_MSG_HEADER * 4;
    _capacity = cap;
    _buf.resize(cap);
    _scratch.resize(256);
}

Queue::~Queue() {
    request_stop();
    join();
}

uint8_t Queue::peek_locked(size_t off) const {
    return _buf[(_head + off) % _buf.size()];
}

void Queue::write_locked(const uint8_t* p, size_t n) {
    if (n == 0) return;
    const size_t cap = _buf.size();
    const size_t pos = (_head + _size) % cap;
    const size_t first = (std::min)(n, cap - pos);
    std::memcpy(_buf.data() + pos, p, first);
    if (n > first) std::memcpy(_buf.data(), p + first, n - first);
    _size += n;
}

int Queue::push(uint64_t dst, uint64_t src, int32_t cmd, const void* body, uint32_t len) {
    /* 门口就挡掉非法输入, 绝不让它进长度计算 */
    if (len != 0 && !body) return MDPSR_ERR_BAD_MESSAGE;
    const size_t total = MDPSR_MSG_HEADER + static_cast<size_t>(len);
    if (total > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return MDPSR_ERR_BAD_MESSAGE;
    }

    {
        std::lock_guard<std::mutex> lk(_mtx);
        if (_size + total > _buf.size()) {
            /* ★ 有界队列: 满了就是满了, 明确告诉调用方。
             * 不偷偷丢 (调用方会以为投成功了), 也不无限涨 (那是吃光内存)。 */
            _full.fetch_add(1, std::memory_order_relaxed);
            return MDPSR_ERR_QUEUE_FULL;
        }
        uint8_t hdr[MDPSR_MSG_HEADER];
        const int32_t flen = static_cast<int32_t>(total);
        std::memcpy(hdr, &dst, 8);
        std::memcpy(hdr + 8, &src, 8);
        std::memcpy(hdr + 16, &cmd, 4);
        std::memcpy(hdr + 20, &flen, 4);
        write_locked(hdr, MDPSR_MSG_HEADER);
        if (len) write_locked(static_cast<const uint8_t*>(body), len);

        _pushed.fetch_add(1, std::memory_order_relaxed);
        const uint64_t used = static_cast<uint64_t>(_size);
        uint64_t hw = _high.load(std::memory_order_relaxed);
        while (used > hw && !_high.compare_exchange_weak(hw, used, std::memory_order_relaxed)) {}
    }
    _cv.notify_one();
    return MDPSR_OK;
}

bool Queue::pop_locked(Frame* out) {
    if (_size < MDPSR_MSG_HEADER) return false;

    uint8_t hdr[MDPSR_MSG_HEADER];
    for (size_t i = 0; i < MDPSR_MSG_HEADER; ++i) hdr[i] = peek_locked(i);

    uint64_t dst = 0, src = 0;
    int32_t  cmd = 0, total = 0;
    std::memcpy(&dst, hdr, 8);
    std::memcpy(&src, hdr + 8, 8);
    std::memcpy(&cmd, hdr + 16, 4);
    std::memcpy(&total, hdr + 20, 4);

    if (total < static_cast<int32_t>(MDPSR_MSG_HEADER)) {
        /* 帧头坏了: 只丢这一个头, 不清空整条队列 (别连累排在后面的人) */
        _head = (_head + MDPSR_MSG_HEADER) % _buf.size();
        _size -= MDPSR_MSG_HEADER;
        _errors.fetch_add(1, std::memory_order_relaxed);
        _popped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (_size < static_cast<size_t>(total)) return false;      /* 还没攒够一整条 */

    const size_t blen = static_cast<size_t>(total) - MDPSR_MSG_HEADER;
    _head = (_head + MDPSR_MSG_HEADER) % _buf.size();
    _size -= MDPSR_MSG_HEADER;

    if (blen) {
        if (_scratch.size() < blen) _scratch.resize(blen);
        const size_t cap = _buf.size();
        const size_t first = (std::min)(blen, cap - _head);
        std::memcpy(_scratch.data(), _buf.data() + _head, first);
        if (blen > first) std::memcpy(_scratch.data() + first, _buf.data(), blen - first);
        _head = (_head + blen) % cap;
        _size -= blen;
    }

    out->dst = dst;
    out->src = src;
    out->cmd = cmd;
    out->body = blen ? _scratch.data() : nullptr;
    out->body_len = static_cast<uint32_t>(blen);
    _popped.fetch_add(1, std::memory_order_relaxed);
    return true;
}

size_t Queue::drain_pending() {
    std::lock_guard<std::mutex> lk(_mtx);
    size_t n = 0;
    Frame f;
    while (pop_locked(&f)) ++n;
    if (n) _dropped.fetch_add(n, std::memory_order_relaxed);
    return n;
}

void Queue::stats(mdpsr_queue_stats* out) const {
    if (!out) return;
    std::lock_guard<std::mutex> lk(_mtx);
    std::memset(out, 0, sizeof(*out));
    out->struct_size    = sizeof(*out);
    out->pace_ms        = _pace_ms;
    out->capacity       = _capacity;
    out->used           = static_cast<uint32_t>(_size);
    out->high_water     = static_cast<uint32_t>(_high.load(std::memory_order_relaxed));
    out->paused         = static_cast<uint32_t>(_paced.load(std::memory_order_relaxed));
    out->pushed         = _pushed.load(std::memory_order_relaxed);
    out->popped         = _popped.load(std::memory_order_relaxed);
    out->full           = _full.load(std::memory_order_relaxed);
    out->paced          = _paced.load(std::memory_order_relaxed);
    out->errors         = _errors.load(std::memory_order_relaxed);
}

/* --------------------------------------------------------------------------
 *  自己那条线程
 * ------------------------------------------------------------------------*/
void Queue::start(Runtime* rt) {
    _rt = rt;
    _stop.store(false, std::memory_order_release);
    _running.store(true, std::memory_order_release);
    _th = std::thread([this] { run(); });
}

void Queue::run() {
    /* ★ 先把线程号写上: 卸载路径靠它判断"我是不是正在自己的队列线程上",
     * 从而避免 join 自己。写在最前面, 因为此后任何插件代码都还没跑。 */
    _tid.store(Runtime::current_thread_id(), std::memory_order_release);
    _where.store("run:top", std::memory_order_release);

    for (;;) {
        if (_stop.load(std::memory_order_acquire)) break;

        Frame f;
        {
            std::unique_lock<std::mutex> lk(_mtx);
            if (_size < MDPSR_MSG_HEADER) {
                _cv.wait_for(lk, std::chrono::milliseconds(16), [this] {
                    return _size >= MDPSR_MSG_HEADER || _stop.load(std::memory_order_acquire);
                });
            }
            if (_stop.load(std::memory_order_acquire)) break;
            if (_size < MDPSR_MSG_HEADER) continue;

            /* 限速: 距离上一次"开始处理"不足 pace_ms 就等到点。
             * 睡的时候把锁放开 —— 生产者 push 只需要那把锁。 */
            if (_pace_ms) {
                const auto now = std::chrono::steady_clock::now();                if (now < _next_allowed) {
                    _paced.fetch_add(1, std::memory_order_relaxed);
                    _cv.wait_until(lk, _next_allowed, [this] {
                        return _stop.load(std::memory_order_acquire);
                    });
                    if (_stop.load(std::memory_order_acquire)) break;
                }
            }
            _where.store("run:pop", std::memory_order_release);
            if (!pop_locked(&f)) continue;
        }

        _where.store("run:dispatch", std::memory_order_release);
        const auto start = std::chrono::steady_clock::now();
        if (_rt) {
            /* BUSY 不是错: 那是"消息被回滚重投了", 不该记进错误计数 */
            const int drc = _rt->dispatch_frame(*this, f);
            if (drc != MDPSR_OK && drc != MDPSR_ERR_BUSY) {
                _errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (_pace_ms) {
            /* 按"开始时间"推进: 处理耗时不会被累加进去 */
            _next_allowed = start + std::chrono::milliseconds(_pace_ms);
        }
        _where.store("run:top", std::memory_order_release);
    }
    _where.store("run:exited", std::memory_order_release);
    _running.store(false, std::memory_order_release);
}

void Queue::request_stop() {
    _stop.store(true, std::memory_order_release);
    _cv.notify_all();
}

void Queue::join() {
    if (_th.joinable()) _th.join();
}

} /* namespace mdpsr */
