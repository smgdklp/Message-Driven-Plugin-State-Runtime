/* ============================================================================
 *  mdpsr/scr/main/dispatcher.cpp
 * ==========================================================================*/
#include "dispatcher.h"

#include "runtime.h"

#include <algorithm>
#include <chrono>

namespace mdpsr {

/* ==========================================================================
 *  Pool_thread
 * ==========================================================================*/
ThreadPool::~ThreadPool() {
    join_all();
    std::lock_guard<std::mutex> lk(_mtx);
    for (Item* it : _items) delete it;
    _items.clear();
}

void* ThreadPool::spawn(const std::string& name, std::function<void()> fn) {
    auto* it = new Item{};
    it->name = name;
    it->th = std::thread(std::move(fn));
    {
        std::lock_guard<std::mutex> lk(_mtx);
        _items.push_back(it);
    }
    return it;
}

void ThreadPool::join(void* handle) {
    if (!handle) return;
    Item* it = static_cast<Item*>(handle);
    if (it->th.joinable()) it->th.join();
    {
        std::lock_guard<std::mutex> lk(_mtx);
        auto pos = std::find(_items.begin(), _items.end(), it);
        if (pos != _items.end()) _items.erase(pos);
    }
    delete it;
}

size_t ThreadPool::count() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _items.size();
}

void ThreadPool::join_all() {
    std::vector<Item*> snapshot;
    {
        std::lock_guard<std::mutex> lk(_mtx);
        snapshot = _items;
    }
    for (Item* it : snapshot) {
        if (it->th.joinable()) it->th.join();
    }
}

/* ==========================================================================
 *  Dispatcher
 * ==========================================================================*/
Dispatcher::Dispatcher(Runtime* rt, Queue* q, mdpsr_queue* info)
    : _rt(rt), _q(q), _info(info) {
}

Dispatcher::~Dispatcher() = default;

void Dispatcher::run() {
    _tid = std::this_thread::get_id();
    while (!_stop.load(std::memory_order_acquire)) {
        uint64_t       handle = 0;
        const uint8_t* body = nullptr;
        size_t         len = 0;
        if (!_q->pop(&handle, &body, &len)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (_stop.load(std::memory_order_acquire)) break;   /* 停止时把剩下的丢掉 */
        if (_rt) _rt->dispatch_message(this, handle, body, len);
    }
}

/* ==========================================================================
 *  Pool_dispatcher
 * ==========================================================================*/
DispatcherPool::~DispatcherPool() = default;

Dispatcher* DispatcherPool::create(Runtime* rt, Queue* q, mdpsr_queue* info) {
    auto d = std::make_unique<Dispatcher>(rt, q, info);
    Dispatcher* raw = d.get();
    std::lock_guard<std::mutex> lk(_mtx);
    _items.push_back(std::move(d));
    return raw;
}

void DispatcherPool::destroy(Dispatcher* d) {
    if (!d) return;
    std::lock_guard<std::mutex> lk(_mtx);
    auto pos = std::find_if(_items.begin(), _items.end(),
                            [d](const std::unique_ptr<Dispatcher>& p) { return p.get() == d; });
    if (pos != _items.end()) _items.erase(pos);
}

size_t DispatcherPool::count() const {
    std::lock_guard<std::mutex> lk(_mtx);
    return _items.size();
}

std::vector<Dispatcher*> DispatcherPool::all() const {
    std::lock_guard<std::mutex> lk(_mtx);
    std::vector<Dispatcher*> out;
    out.reserve(_items.size());
    for (const auto& p : _items) out.push_back(p.get());
    return out;
}

void DispatcherPool::stop_all() {
    /* 只负责全部置停; join 由 Runtime 用 Pool_thread 的句柄统一做 */
    for (Dispatcher* d : all()) d->request_stop();
}

} /* namespace mdpsr */
