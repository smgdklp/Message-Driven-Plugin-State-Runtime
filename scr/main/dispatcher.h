#pragma once
/* ============================================================================
 *  mdpsr/scr/main/dispatcher.h
 *
 *  多线程分流:
 *
 *    Pool_thread      —— 挂载 dispatcher 的线程池子
 *    Dispatcher       —— 一个 dispatcher 与一个 Queue 生命周期绑定,
 *                        只从自己那一个 queue 取消息
 *    Pool_dispatcher  —— 存放 dispatcher 对象
 *
 *  分流本质: 每个 dispatcher 做的事情完全一样 (取消息 → 查表 → 调 handle),
 *  区别只在"吃哪个队列"。组件申请到自己的 queue 之后把 handle 绑上去,
 *  它的消息就在那条专属线程上执行; 组件卸载时队列与线程一起消失。
 * ==========================================================================*/

#include "queue_map.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mdpsr {

class Runtime;

/* ==========================================================================
 *  Pool_thread —— 挂载 dispatcher 的线程池子
 * ==========================================================================*/
class ThreadPool {
public:
    ThreadPool() = default;
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /* 起一条常驻线程, 返回不透明句柄 */
    void*  spawn(const std::string& name, std::function<void()> fn);
    /* join 并释放句柄 (线程退出的条件由调用方负责置位) */
    void   join(void* handle);
    size_t count() const;
    /* 只 join 全部, 不销毁句柄 */
    void   join_all();

private:
    struct Item {
        std::string name;
        std::thread th;
    };
    mutable std::mutex  _mtx;
    std::vector<Item*>  _items;
};

/* ==========================================================================
 *  Dispatcher —— 一个 queue 一条命
 * ==========================================================================*/
class Dispatcher {
public:
    Dispatcher(Runtime* rt, Queue* q, mdpsr_queue* info);
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    Queue*       queue() const { return _q; }
    mdpsr_queue* info()  const { return _info; }
    uint64_t     key()   const { return _info ? _info->name : 0; }

    void run();                                     /* 线程主体 */
    void request_stop() { _stop.store(true, std::memory_order_release); }
    bool stopping() const { return _stop.load(std::memory_order_acquire); }

    void  set_thread_handle(void* h) { _thread_handle = h; }
    void* thread_handle() const { return _thread_handle; }
    void  set_thread_id(std::thread::id id) { _tid = id; }
    std::thread::id thread_id() const { return _tid; }

private:
    Runtime*          _rt;
    Queue*            _q;
    mdpsr_queue*      _info;
    std::atomic<bool> _stop{ false };
    void*             _thread_handle = nullptr;
    std::thread::id   _tid{};
};

/* ==========================================================================
 *  Pool_dispatcher —— 存放 dispatcher 对象
 * ==========================================================================*/
class DispatcherPool {
public:
    DispatcherPool() = default;
    ~DispatcherPool();
    DispatcherPool(const DispatcherPool&) = delete;
    DispatcherPool& operator=(const DispatcherPool&) = delete;

    Dispatcher* create(Runtime* rt, Queue* q, mdpsr_queue* info);
    void        destroy(Dispatcher* d);
    size_t      count() const;
    /* 只请求停 + join, 不销毁对象 */
    void        stop_all();
    std::vector<Dispatcher*> all() const;

private:
    mutable std::mutex                       _mtx;
    std::vector<std::unique_ptr<Dispatcher>> _items;
};

} /* namespace mdpsr */
