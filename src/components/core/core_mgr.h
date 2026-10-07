#pragma once
/* ============================================================================
 *  core_Mgr —— 内核：插件资源的统一管理对象
 *
 *  导出的东西（命名规范）:
 *      core_Handle_Mgr          Handle   —— 无状态消息入口 + 后门
 *      core_Mgr                 Object   —— 真正干活的类
 *      core_State_ResourceDict  State    —— 已装载插件 -> 它们的资源键
 *
 *  它管的不是"业务"，而是"别的插件能不能被装进来、能不能被干净地撤掉"。
 *  所有需要跨消息保留的东西都放在 State (ResourceDict) 里；
 *  类实例本身只有"当前正在处理哪次装载/卸载"这种瞬态信息。
 * ==========================================================================*/

#include "mdpsr/abi.h"

#include <chrono>

namespace mdpsr {
namespace core {

/* ==========================================================================
 *  core_State_ResourceDict 的载荷 —— 已装载插件的资源索引
 *
 *  纯 POD，没有构造/析构，所以作为 State 载荷是安全的。
 * ==========================================================================*/
#define CORE_MAX_PLUGINS 64
#define CORE_MAX_KEYS    64

struct ResourceEntry {
    uint64_t plugin;                      /* 0 = 空槽 */
    uint32_t count;
    uint32_t pad;
    uint64_t keys[CORE_MAX_KEYS];         /* 这个插件在 MainMap 里占的资源键 */
};

struct ResourceDict {
    static constexpr uint32_t kMagic = 0x434F5245u;   /* 'CORE' */

    uint32_t      magic;
    uint32_t      count;                  /* 已用槽数 */
    uint64_t      reserved[2];            /* 以后要加东西就用这里, 保持布局稳定 */
    ResourceEntry entries[CORE_MAX_PLUGINS];
};

/* ==========================================================================
 *  core_Mgr —— 管理对象
 *
 *  线程: 它只被"本插件自己的 handle"使用, 而 dispatch 在调用期间持有
 *  该 handle 的锁, 所以同一时刻只有一个线程在跑这些方法 —— 不需要内部加锁。
 * ==========================================================================*/
class DllMgr {
public:
    DllMgr(const mdpsr_host* host, mdpsr_resource* pool) : _h(host), _pool(pool) {}
    ~DllMgr() = default;

    DllMgr(const DllMgr&) = delete;
    DllMgr& operator=(const DllMgr&) = delete;

    struct Cmd {
        int32_t     cmd;         /* 0 初始化 / 1 装载 / 2 卸载 / 3 看门狗 tick */
        uint64_t    plugin;      /* 卸载用 */
        const char* path;        /* 装载用 (可能为 nullptr) */
    };

    /* 返回 MDPSR_OK 或错误码。need_retry 为 true 时调用方应把原消息回滚给自己。 */
    int handle_cmd(const Cmd& c, mdpsr_map* state_table, bool* need_retry);

    bool still_working() const { return _busy; }

    /* 析构时要靠它把内存还回正确的池 */
    mdpsr_resource* pool() const { return _pool; }

    /* 卸载连续被推迟的次数 (超过上限就不再回滚, 免得消息永远在队列里转) */
    int  retries_of(uint64_t plugin) const;
    void bump_retry(uint64_t plugin);
    void clear_retry(uint64_t plugin);

    static constexpr int kMaxRetry = 40;

private:
    ResourceDict*  dict_of(mdpsr_map* state_table) const;
    ResourceEntry* entry_of(ResourceDict* d, uint64_t plugin) const;
    ResourceEntry* take_free(ResourceDict* d) const;

    int do_load(const char* manifest_rel, ResourceDict* d);
    int do_unload(uint64_t plugin, ResourceDict* d);

    const mdpsr_host* _h = nullptr;
    mdpsr_resource*   _pool = nullptr;

    /* 瞬态: 某次装载/卸载还在途中时, 我们靠看门狗消息 (cmd=3) 把它捡回来。
     * 这是"类内部缓存一个计时器"的用法 —— 它不跨进程、不跨 dll, 只是本对象
     * 在一次调用到下一次调用之间记个时间点, 所以不违反"handle 无状态"。 */
    bool _busy = false;
    std::chrono::steady_clock::time_point _watchdog_at{};

    /* 每个插件的重试计数 (只在 1 次卸载期间有意义, 上限 kMaxRetry) */
    static constexpr uint32_t kMaxTracked = 16;
    uint64_t _retry_key[kMaxTracked] = {};
    int      _retry_cnt[kMaxTracked] = {};
};

} /* namespace core */
} /* namespace mdpsr */
