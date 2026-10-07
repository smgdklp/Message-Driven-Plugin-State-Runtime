/* ============================================================================
 *  mdpsr/runtime/queue.cpp
 *  Queue —— 环形字节缓冲 + 自己的线程 + 限速计时器
 *
 *  帧布局 (16 字节头):
 *      uint64_t _handle
 *      int32_t  _cmd
 *      int32_t  _len      本条消息总字节长 = 16 + body
 *      uint8_t  body[]
 *
 *  线程模型:
 *      · _mtx 一把非递归锁, 覆盖 push / pop / 扩容 / 回绕。多生产者 + 单消费者
 *        真线程安全, 代价是每帧一次拷贝。
 *      · 限速计时器在 pop 成功之后作用: 若这条消息"需要限速", 线程在处理它
 *        之前先睡到满足 pace_ms。这不是节流生产者, 而是保证"一个把消息回滚
 *        给自己的循环"不会把整条队列的 CPU 吃干 —— 规格里那句
 *        "防止有天才拿来当 while" 就是指这个。
 * ==========================================================================*/
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace mdpsr {

Queue::Queue(std::pmr::memory_resource* pool, std::string name, uint64_t key, const Desc& d)
    : _buf(pool)
    , _scratch(pool)
    , _name(std::move(name))
    , _key(key)
    , _pace_ms(d.pace_ms) {
    size_t cap = d.capacity ? d.capacity : 4096;
    if (cap < MDPSR_MSG_HEADER * 2) cap = MDPSR_MSG_HEADER * 2;
    _buf.resize(cap);
}

Queue::~Queue() {
    request_stop();
    join();
    _cv.notify_all();
}

/* --------------------------------------------------------------------------
 *  环形缓冲
 * ------------------------------------------------------------------------*/
uint8_t Queue::peek_at(size_t offset) const {
    return _buf[(_head + offset) % _buf.size()];
}

void Queue::ensure(size_t need) {
    if (_buf.size() >= need) return;

    size_t newcap = _buf.empty() ? 4096 : _buf.size();
    /* 显式溢出保护: need 极端大时 newcap 会绕回 0 -> 死循环 */
    while (newcap < need) {
        if (newcap > (std::numeric_limits<size_t>::max)() / 2) {
            newcap = need;          /* 直接顶上去, 让 resize 去抛/失败 */
            break;
        }
        newcap *= 2;
    }

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

void Queue::push(uint64_t handle, int32_t cmd, const void* body, size_t len) {
    /* 空指针 + 超长在门口就挡掉, 绝不让它进 memcpy / 长度计算 */
    if (len != 0 && !body) return;
    if (len > 64u * 1024u * 1024u) return;

    {
        std::lock_guard<std::mutex> lk(_mtx);
        const size_t total = MDPSR_MSG_HEADER + len;
        if (total > static_cast<size_t>(0x7FFFFFFF)) return;   /* _len 是 int32 */

        ensure(_size + total);

        uint8_t hdr[MDPSR_MSG_HEADER];
        const int32_t flen = static_cast<int32_t>(total);
        std::memcpy(hdr, &handle, 8);
        std::memcpy(hdr + 8, &cmd, 4);
        std::memcpy(hdr + 12, &flen, 4);
        write_bytes(hdr, MDPSR_MSG_HEADER);
        if (len) write_bytes(static_cast<const uint8_t*>(body), len);
        _pushed++;
    }
    _cv.notify_one();
}

bool Queue::pop(uint64_t* handle, int32_t* cmd, const uint8_t** body, size_t* len) {
    std::lock_guard<std::mutex> lk(_mtx);
    if (_size < MDPSR_MSG_HEADER) return false;

    uint8_t hdr[MDPSR_MSG_HEADER];
    for (size_t i = 0; i < MDPSR_MSG_HEADER; ++i) hdr[i] = peek_at(i);

    uint64_t h = 0;
    int32_t  c = 0, total = 0;
    std::memcpy(&h, hdr, 8);
    std::memcpy(&c, hdr + 8, 4);
    std::memcpy(&total, hdr + 12, 4);

    if (total < static_cast<int32_t>(MDPSR_MSG_HEADER)) {
        /* 帧头损坏: 只丢掉这一帧的头, 不清空整条队列 (以前是清空, 会连累所有人) */
        skip_bytes(MDPSR_MSG_HEADER);
        return false;
    }
    if (_size < static_cast<size_t>(total)) return false;   /* 还没攒够一条 */

    skip_bytes(MDPSR_MSG_HEADER);
    const size_t blen = static_cast<size_t>(total) - MDPSR_MSG_HEADER;
    _scratch.resize(blen ? blen : 1);
    if (blen) read_bytes(_scratch.data(), blen);

    if (handle) *handle = h;
    if (cmd)    *cmd = c;
    if (body)   *body = _scratch.data();
    if (len)    *len = blen;
    _popped++;
    return true;
}

void Queue::drain() {
    uint64_t h = 0; int32_t c = 0; const uint8_t* b = nullptr; size_t l = 0;
    while (pop(&h, &c, &b, &l)) { /* 丢掉 */ }
}

size_t Queue::readable() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _size;
}

uint64_t Queue::pushed() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _pushed;
}

uint64_t Queue::popped() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _popped;
}

/* --------------------------------------------------------------------------
 *  自己那条线程
 * ------------------------------------------------------------------------*/
void Queue::start(Runtime* rt, bool main_thread) {
    _main_thread = main_thread;
    _main_tid = rt ? rt->main_thread_id() : 0;
    _stop.store(false, std::memory_order_release);
    if (main_thread) {
        /* 挂主线程的队列不起线程, 由宿主主循环 pump_once 驱动 */
        _running.store(true, std::memory_order_release);
        return;
    }
    _running.store(true, std::memory_order_release);
    _th = std::thread([this, rt] { run(rt); });
}

void Queue::run(Runtime* rt) {
    _tid = std::this_thread::get_id();

    const auto gap = std::chrono::milliseconds(_pace_ms ? _pace_ms : 0);
    auto next_allowed = std::chrono::steady_clock::now();

    for (;;) {
        if (_stop.load(std::memory_order_acquire)) break;

        uint64_t       handle = 0;
        int32_t        cmd = 0;
        const uint8_t* body = nullptr;
        size_t         len = 0;

        if (!pop(&handle, &cmd, &body, &len)) {
            /* 空队列: 睡到条件变量上, 由 push 叫醒, 绝不空转 */
            std::unique_lock<std::mutex> lk(_mtx);
            if (_size < MDPSR_MSG_HEADER) {
                _cv.wait_for(lk, std::chrono::milliseconds(16),
                             [this] {
                                 return _size >= MDPSR_MSG_HEADER ||
                                        _stop.load(std::memory_order_acquire);
                             });
            }
            continue;
        }

        /* 限速: 两条消息的处理开始时间之间至少隔 pace_ms。
         * 睡在这里是安全的 —— 生产者的 push 只需要那一把锁, 而睡觉时锁已经放开。
         * 这也天然给了队列背压: 一个把消息回滚给自己的循环最多跑到 1/pace 次每秒。 */
        if (gap.count() > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now < next_allowed) {
                _total_paced.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(next_allowed - now);
                if (_stop.load(std::memory_order_acquire)) break;
            }
            next_allowed = std::chrono::steady_clock::now() + gap;
        }

        if (rt) {
            if (rt->dispatch_one(this, handle, cmd, body, len) != MDPSR_OK) {
                _errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    _running.store(false, std::memory_order_release);
}

int Queue::pump_once(Runtime* rt, uint32_t wait_ms) {
    if (_stop.load(std::memory_order_acquire)) return 0;

    /* 等一条完整消息 (最多 wait_ms)。主循环每轮都调, 所以不能空转。 */
    {
        std::unique_lock<std::mutex> lk(_mtx);
        if (_size < MDPSR_MSG_HEADER) {
            _cv.wait_for(lk, std::chrono::milliseconds(wait_ms),
                         [this] {
                             return _size >= MDPSR_MSG_HEADER ||
                                    _stop.load(std::memory_order_acquire);
                         });
        }
        if (_stop.load(std::memory_order_acquire)) return 0;
        if (_size < MDPSR_MSG_HEADER) return 0;
    }

    /* 挂主线程的队列也限速, 但在这里【绝不能 sleep】—— 一睡就把 Win32 消息泵堵死。
     * 做法: 到点之前直接返回 0, 让主循环下一轮再来 (主循环自带 1ms 睡眠)。
     * 消息留在队列里不动, 所以顺序不会被破坏, 也不存在头阻塞:
     * 反正这一轮本来也不该处理它。 */
    if (_pace_ms) {
        const auto now = std::chrono::steady_clock::now();
        if (now < _next_allowed) {
            _total_paced.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }

    uint64_t       handle = 0;
    int32_t        cmd = 0;
    const uint8_t* body = nullptr;
    size_t         len = 0;
    if (!pop(&handle, &cmd, &body, &len)) return 0;

    if (rt) rt->dispatch_one(this, handle, cmd, body, len);
    if (_pace_ms) {
        _next_allowed = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(_pace_ms);
    }
    return 1;
}

void Queue::request_stop() {
    _stop.store(true, std::memory_order_release);
    _cv.notify_all();
}

void Queue::join() {
    if (_th.joinable()) _th.join();
}

} /* namespace mdpsr */
