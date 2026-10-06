#pragma once
/* ============================================================================
 *  mdpsr/scr/main/queue_map.h
 *  状态机的三块地基: Pool / Queue / Map
 *
 *  Pool_msg   synchronized_pool_resource        消息字节流
 *  Pool_map   synchronized_pool_resource        Map 节点 + 计数指针 + 队列描述符
 *  Pool_state synchronized_pool_resource        公用 State 与可变长 cache
 *  Pool_dll   unordered_map<uint64_t, unique_ptr<synchronized_pool_resource>>
 *                                               每个 DLL 一个独立池, 卸载整池释放
 *
 *  注意: 适配多线程分流之后 Pool_state 从 unsynchronized 改成了 synchronized ——
 *  现在多个 dispatcher 线程会并发地从它里面分配 State 载荷,
 *  跨线程用 unsynchronized_pool_resource 是未定义行为。
 * ==========================================================================*/

#include <mdpsr/abi.h>

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mdpsr {

/* ==========================================================================
 *  池
 * ==========================================================================*/
class Pools {
public:
    Pools();
    ~Pools();

    std::pmr::memory_resource* msg()   const { return _msg.get(); }
    std::pmr::memory_resource* map()   const { return _map.get(); }
    std::pmr::memory_resource* state() const { return _state.get(); }

    std::pmr::memory_resource* dll_pool(uint64_t dll);
    std::pmr::memory_resource* dll_pool_find(uint64_t dll) const;
    bool                       dll_pool_drop(uint64_t dll);

private:
    std::unique_ptr<std::pmr::synchronized_pool_resource> _msg;
    std::unique_ptr<std::pmr::synchronized_pool_resource> _map;
    std::unique_ptr<std::pmr::synchronized_pool_resource> _state;   /* 多线程: 必须同步 */

    mutable std::mutex _dll_mtx;
    std::unordered_map<uint64_t, std::unique_ptr<std::pmr::synchronized_pool_resource>> _dll;
};

/* ==========================================================================
 *  Queue —— 可拓展环形字节缓冲队列
 *
 *  一条消息在队列里的布局 (序列化后的连续字节):
 *      int32_t  _len      本条消息的总字节长 (= 12 + body)
 *      uint64_t _handle   目标 handle 的哈希值
 *      uint8_t  _buffer[] 附带信息, body 长度 = _len - 12
 *
 *  可读字节数 >= _len 时, 认为积累出一条完整消息。
 *  队列本身不对 body 做任何解释。
 * ==========================================================================*/
class Queue {
public:
    explicit Queue(std::pmr::memory_resource* res, size_t initial = 4096);

    /* 组帧入队 */
    void push(uint64_t handle, const void* body, size_t len);

    /* 直接塞一帧已序列化好的字节流; 校验帧头, 成功返回 true */
    bool push_frame(const uint8_t* frame, size_t n);

    /* 取一条完整消息; body 指向内部缓冲, 仅在下次 pop 前有效 */
    bool pop(uint64_t* handle, const uint8_t** body, size_t* len);

    size_t   readable() const;
    uint64_t pushed() const { return _pushed; }
    uint64_t popped() const { return _popped; }

private:
    void    ensure(size_t need);
    void    write_bytes(const uint8_t* p, size_t n);
    void    read_bytes(uint8_t* p, size_t n);
    void    skip_bytes(size_t n);
    uint8_t peek_at(size_t offset) const;

    mutable std::mutex        _mtx;
    std::pmr::vector<uint8_t> _buf;      /* 环形缓冲, 容量 = _buf.size() */
    size_t                    _head = 0;
    size_t                    _size = 0;
    uint64_t                  _pushed = 0;
    uint64_t                  _popped = 0;

    std::pmr::vector<uint8_t> _scratch;  /* pop 的落地缓冲 (Pool_msg) */
};

/* ==========================================================================
 *  Map —— uint64_t -> Countptr
 * ==========================================================================*/
struct Countptr {
    void*    ptr;          /* State* / Classptr* / Handle* / Queue* */
    int32_t  r_count;
    int32_t  w_count;
    int32_t  valid;
    int32_t  type;         /* mdpsr_entry_type */
    uint64_t dll;
    uint64_t bind_object;
    uint64_t bind_queue;   /* handle 专用: 绑定的分流队列键, 0 = queue_core */
    uint32_t bind_state_count;
    uint32_t pad;
    uint64_t bind_states[MDPSR_MAX_BIND_STATES];
    char     name[MDPSR_NAME_MAX];
};

class Map {
public:
    explicit Map(std::pmr::memory_resource* res);
    ~Map();

    int    reg(const mdpsr_entry& e);
    int    set_bind_queue(uint64_t key, uint64_t queue_key);
    int    acquire(uint64_t key, bool write, Countptr** out);
    int    release(uint64_t key, bool write);
    int    invalidate(uint64_t key);
    int    erase(uint64_t key);
    int    wait_idle(uint64_t key, uint32_t timeout_ms);
    int    dll_idle(uint64_t dll, uint32_t timeout_ms);
    int    snapshot(uint64_t dll, mdpsr_entry_info* out, uint32_t cap, uint32_t* out_count);
    size_t collect_classptrs(std::vector<mdpsr_classptr*>& out);
    size_t keys_of_dll(uint64_t dll, uint64_t* out, uint32_t cap, uint32_t* out_count);
    size_t live_count() const;

private:
    std::pmr::memory_resource* _res;
    mutable std::mutex         _mtx;
    std::condition_variable    _cv;
    std::pmr::unordered_map<uint64_t, Countptr*> _table;
};

} /* namespace mdpsr */

// touch 639269210643932546

// touch 639269210885353085

// touch 639269211452447189
