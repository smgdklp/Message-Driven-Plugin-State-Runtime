#pragma once
/* ============================================================================
 *  mdpsr/runtime/queue.h
 *  Queue —— 有界环形字节缓冲 + 自己一条线程 + 限速计时器
 *
 *  与 v1 的三点不同:
 *
 *   1) 【有界】capacity 是硬上限, 满了 push 直接失败并返回 QUEUE_FULL。
 *      v1 的队列无限增长, 于是"生产者比消费者快"最终表现为吃光内存, 而不是
 *      一个可处理的错误码。有界 = 可预测 = 稳定。
 *
 *   2) 【限速是按"开始时间"算的】next_allowed = start_of_last + pace_ms。
 *      v1 用的是"睡完再算", 处理耗时会被累加进去, 于是声明 20ms 实际越跑越慢。
 *
 *   3) 【统计是真的】pushed/popped/full/paced/errors/high_water 全都在这里,
 *      而且宿主真的把它们报出来 (v1 的"限速挡下"计数器压根没人加过)。
 *
 *  帧布局 (24 字节头):
 *      uint64_t dst
 *      uint64_t src
 *      int32_t  cmd
 *      int32_t  len      整帧字节数 = 24 + body
 *      uint8_t  body[]
 * ==========================================================================*/

#include "mdpsr/abi.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mdpsr {

class Runtime;

/* 一条已经解好的消息。body 指向队列内部的 scratch, 只在下次 pop 之前有效。 */
struct Frame {
    uint64_t       dst = 0;
    uint64_t       src = 0;
    int32_t        cmd = 0;
    const uint8_t* body = nullptr;
    uint32_t       body_len = 0;
};

class Queue {
public:
    struct Desc {
        uint32_t capacity = 65536;
        uint32_t pace_ms  = 0;
    };

    Queue(std::pmr::memory_resource* pool, std::string name, uint64_t key, const Desc& d);
    ~Queue();
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    uint64_t           key() const { return _key; }
    const std::string& name() const { return _name; }
    uint32_t           pace_ms() const { return _pace_ms; }
    bool               running() const { return _running.load(std::memory_order_acquire); }
    bool               stopping() const { return _stop.load(std::memory_order_acquire); }
    uint32_t           thread_id() const { return _tid.load(std::memory_order_acquire); }

    /* 组一帧入队。多生产者安全。
     * 返回 MDPSR_OK / MDPSR_ERR_QUEUE_FULL / MDPSR_ERR_BAD_MESSAGE */
    int push(uint64_t dst, uint64_t src, int32_t cmd, const void* body, uint32_t len);

    /* 自己那条线程。★ v3 起队列只有这一种跑法: 每条队列一条自己的线程。
     * (以前还有一种"挂在宿主主线程上、由主循环泵"的模式, 已经删掉 —— GUI
     *  这类线程亲和的活现在由插件自己开线程, 见 components/winmsg) */
    void start(Runtime* rt);
    void request_stop();
    void join();

    /* 停止之后把没来得及处理的丢掉, 返回丢掉的条数 (会写进 dropped 统计) */
    size_t drain_pending();

    void stats(mdpsr_queue_stats* out) const;

    /* 诊断: 这条队列的线程"现在正在干什么" (卡住时唯一能看出它在哪的办法,
     * 因为这台机器上没有调试器)。run() 每一步都会更新它。 */
    const char* where() const { return _where.load(std::memory_order_acquire); }
    /* 给分发路径用: 让"这条队列的线程现在在哪一步"能被外面看见 */
    void mark(const char* w) { _where.store(w, std::memory_order_release); }

private:
    void   run();
    bool   pop_locked(Frame* out);
    void   write_locked(const uint8_t* p, size_t n);
    uint8_t peek_locked(size_t off) const;

    mutable std::mutex      _mtx;
    std::condition_variable _cv;
    std::pmr::vector<uint8_t> _buf;
    std::pmr::vector<uint8_t> _scratch;
    size_t                  _head = 0;
    size_t                  _size = 0;

    std::string _name;
    uint64_t    _key = 0;
    uint32_t    _pace_ms = 0;
    uint32_t    _capacity = 0;

    Runtime*  _rt = nullptr;
    std::thread _th;
    std::atomic<bool>     _stop{ false };
    std::atomic<bool>     _running{ false };
    std::atomic<uint32_t> _tid{ 0 };
    std::chrono::steady_clock::time_point _next_allowed{};

    /* 统计 (全原子, 读的时候不用抢锁) */
    std::atomic<uint64_t> _pushed{ 0 }, _popped{ 0 }, _full{ 0 }, _paced{ 0 },
                          _errors{ 0 }, _dropped{ 0 }, _high{ 0 };
    std::atomic<const char*> _where{ "created" };
};

} /* namespace mdpsr */
