#pragma once
/* ============================================================================
 *  mdpsr/runtime/registry.h
 *  注册表 + 每插件内存池
 *
 *  三条不变量 (整个运行时的正确性都建在这上面):
 *
 *   1) 【条目控制块永不释放】
 *      Entry 从宿主自己的 arena 里分配, 一旦创建就活到进程结束。热插拔会反复
 *      用同一个键 (名字没变, 哈希就没变), 所以"卸载后旧指针悬垂"这件事在结构
 *      上不可能发生 —— 旧指针指向的还是那个 Entry, 只是 gen 变成 0 了。
 *      插件根本拿不到 Entry*, 但有借有还的协议需要一个永不消失的锚点, 就是它。
 *
 *   2) 【gen 表达"这是第几代"】
 *      每次装载 +1, 失效置 0。acquire 可以指定想要哪一代:
 *          想用"当前这一代的服务" -> want_gen = 0
 *          手里存着上一次拿到的 gen  -> 传它, 换过代就返回 STALE
 *      这就是 v1 遗留的"Ptr 代际问题"的答案。
 *
 *   3) 【借还协议 = 一把非递归锁 + 一本台账】
 *      acquire: 读 gen -> 锁上 -> 复查 gen -> 记账 -> 交出指针
 *      release: 从台账里找到 -> 解锁
 *      台账 (journal) 是 per-thread 的, 并且宿主在每个"调用插件代码"的入口都
 *      开一个作用域; 作用域退出时台账没清空 = 插件漏了 release -> 宿主兜底解锁
 *      并记一条 WARN。★ 所以"插件漏锁把整个运行时卡死"是不可能的。
 * ==========================================================================*/

#include "mdpsr/abi.h"

#include <atomic>
#include <memory_resource>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mdpsr {

/* ==========================================================================
 *  每插件一个的池。mdpsr_pool 在 C 侧是不透明类型, 真身就是它。
 * ==========================================================================*/
struct Pool {
    uint64_t               owner = 0;      /* 插件键 */
    std::pmr::synchronized_pool_resource res;
    std::atomic<uint64_t>  allocs{ 0 };
    std::atomic<uint64_t>  frees{ 0 };
    std::atomic<uint64_t>  live{ 0 };      /* 还活着的分配次数 (估算泄漏用) */

    Pool() = default;
    explicit Pool(uint64_t o) : owner(o) {}
};

/* ==========================================================================
 *  条目的控制块
 * ==========================================================================*/
struct Entry {
    uint64_t              key = 0;
    std::atomic<uint32_t> gen{ 0 };            /* 0 = 空/失效 */
    std::atomic<void*>    ptr{ nullptr };
    int                   kind = -1;
    bool                  borrowable = false;  /* 只有 STATE / OBJECT 能被借 */
    /* ★ published = "这个条目已经上线, 可以被别人借了"。
     * 装载过程中先 attach(ptr 有了, gen 也有了) 但 published=false: 于是任何别的
     * 插件都借不到"半装好的东西", gen_of 也看不到它。装载全部成功之后由
     * Registry::publish() 一次翻成 true。
     *
     * 为什么必须这样: 装载失败的回滚要 pool_delete(整池还回系统), 而在它之前
     * 我们不一定抢得齐所有条目锁。如果半装好的 State 已经能被借走, 那个借阅方
     * 手里的载荷就会在池回收时变成悬垂。上线门闸把这条路径从"抢不到锁就危险"
     * 变成"根本不可能有人拿着"。 */
    std::atomic<bool>     published{ false };
    uint64_t              plugin = 0;          /* 拥有者 */
    uint32_t              owner_gen = 0;
    char                  name[MDPSR_NAME_MAX] = { 0 };

    /* 卸载时调用的析构 (可空), 以及它需要的上下文。
     * ctx 里的字符串指向 PluginRecord 里的稳定副本, 所以这里只存指针。 */
    mdpsr_destroy_fn      destroy = nullptr;
    const mdpsr_factory_ctx* fctx = nullptr;

    std::timed_mutex       mtx;                /* 借用门闸 (有界等待, 见 lock_bounded) */
    std::atomic<uint64_t> acquires{ 0 };
    std::atomic<uint64_t> busy{ 0 };
};

/* 借用时最多等多久。★ 这个数字是"稳定性"的关键: 任何线程都不允许在条目锁上
 * 无限期等待 —— 因为卸载方要"先独占条目, 再去 join 队列线程", 只要有一个队列
 * 线程卡在某把锁上不回头, join 就永远不返回。给等待加一个上界, 那个线程就一定会
 * 回到队列循环顶部, 看到 _stop, 然后退出。 */
#define MDPSR_LOCK_WAIT_MS 50
#define MDPSR_LOCK_TRIES   8

/* ==========================================================================
 *  线程本地的调用台账
 *
 *  frames 是一叠"作用域起点"。为什么要嵌套: plugin_install(SYNC_INIT) 会在
 *  调用者线程上跑新插件的 cmd=0, 那是一次嵌套的插件调用 —— 它有自己的台账,
 *  退出时也只清理自己那一层。
 * ==========================================================================*/
struct Tls {
    struct Held { uint64_t key; Entry* e; };

    std::vector<Held>    held;      /* 所有层合起来的台账 (栈式) */
    std::vector<size_t>  frames;    /* 每层作用域在 held 里的起始下标 */
    std::vector<uint64_t> prev_plugin;
    std::vector<uint64_t> prev_handle;

    uint64_t cur_plugin = 0;        /* 当前调用属于哪个插件 (0 = 宿主) */
    uint64_t cur_handle = 0;
    int      depth = 0;
    int      in_host_call = 0;      /* 正在宿主 API 内部 (latch, 防重入) */

    size_t base() const { return frames.empty() ? 0 : frames.back(); }
    uint64_t max_held_key() const {
        uint64_t m = 0;
        for (const Held& h : held) if (h.key > m) m = h.key;
        return m;
    }
    bool holds(uint64_t key) const {
        for (const Held& h : held) if (h.key == key) return true;
        return false;
    }
    Entry* find_held(uint64_t key) const {
        for (const Held& h : held) if (h.key == key) return h.e;
        return nullptr;
    }
};

Tls& tls();

/* ==========================================================================
 *  一次"调用插件代码"的作用域 —— 所有入口都必须套它
 * ==========================================================================*/
class CallScope {
public:
    CallScope(uint64_t plugin, uint64_t handle, const char* what);
    ~CallScope();
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;

    /* 这次调用里插件漏还了几个锁 (留给人看) */
    int leaked() const { return _leaked; }
    const char* what() const { return _what; }

private:
    const char* _what = "";
    int         _leaked = 0;
};

/* ==========================================================================
 *  Registry
 * ==========================================================================*/
class Registry {
public:
    explicit Registry(std::pmr::memory_resource* arena);
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /* ---- 表结构 (短临界区, 绝不在这里调插件代码) ---- */

    Entry* find(uint64_t key);                        /* 不加锁快照, 可能已失效 */

    /* 挂一个条目。键已存在且还有效 -> ALREADY_EXISTS;
     * 键存在但已失效 -> 复用那个 Entry (try_lock 拿不到就 BUSY)。
     * out 拿到 Entry* (生命期 = 进程)。
     * published = false 时条目"挂着但还不能被借"(装载中途), 全部装好之后调
     * publish() 一次性上线 —— 见 Entry::published 的说明。 */
    int attach(uint64_t key, int kind, uint64_t plugin, uint32_t owner_gen,
               const char* name, void* ptr, mdpsr_destroy_fn destroy,
               const mdpsr_factory_ctx* fctx, bool published, Entry** out);

    /* 上线: 把这些键翻成"可借"。装载成功 (READY) 的那一刻调用一次。
     * 必须成批调用 —— 上线是原子的语义: 要么都上, 要么都还没上。 */
    int publish(const std::vector<uint64_t>& keys);

    /* 摘表 (置空 + gen=0)。不要求持锁: 调用方要么是卸载路径 (已独占),
     * 要么是失败回滚。 */
    int detach(uint64_t key);
    int invalidate(uint64_t key);                     /* gen = 0, 表里还在 */
    int revalidate(uint64_t key, uint32_t gen);       /* 撤销上面那一步 */

    size_t   size();
    uint32_t gen_of(uint64_t key);
    int      kind_of(uint64_t key);
    int      name_of(uint64_t key, char* out, uint32_t cap);
    /* 名字有没有被别的条目占着 (except_key 例外)。装载时查重名用。 */
    bool     name_taken(const char* name, uint64_t except_key);

    /* ---- 借还协议 ---- */
    int  acquire(uint64_t key, uint32_t want_gen, void** out_ptr, uint32_t* out_gen);    int  acquire_many(const uint64_t* keys, const uint32_t* gens, uint32_t n,
                      void** out_ptrs, uint32_t* out_gens);
    int  release(uint64_t key);
    void release_all();                                /* 只清当前作用域 */
    int  held_count();

    /* 有界地拿一把条目锁。这是"任何线程都不会无限期卡住"的执行点:
     *      OK            = 拿到了 (调用方负责 un/lock_entry 配对)
     *      ENTRY_INVALID = 等的过程里资源被置无效了 (卸载中)
     *      BUSY          = 等满了上界还是没拿到 —— 调用方应该放弃这次操作 */
    int  lock_bounded(Entry* e);

    /* ---- 卸载用: 一次独占一批 ----
     * 按键升序 try_lock, 全有或全无。永不阻塞, 所以不可能参与死锁环。 */
    class Hold {
    public:
        ~Hold() { release(); }
        void release();
        bool ok() const { return _ok; }
        size_t count() const { return _held.size(); }
    private:
        friend class Registry;
        std::vector<Entry*> _held;
        bool _ok = false;
    };
    /* 一次独占一批条目。
     *   hold_all          : 全部
     *   hold_kind         : 只要这一种 (卸载时先拿 handle, 用它)
     *   hold_except_kind  : 除了这一种 (拿完 handle 再拿其余, 用它)
     * 都是按键升序 try_lock, 全有或全无, 永不阻塞。 */
    int hold_all(const std::vector<uint64_t>& keys, Hold* out);
    int hold_kind(const std::vector<uint64_t>& keys, int kind, Hold* out);
    int hold_except_kind(const std::vector<uint64_t>& keys, int kind, Hold* out);

    /* ---- 内存 ---- */
    Pool* pool_new(uint64_t owner);
    void  pool_delete(Pool* p);
    void* pool_alloc(Pool* p, size_t bytes, size_t align);
    void  pool_free(Pool* p, void* q, size_t bytes, size_t align);

    /* 诊断 */
    uint64_t total_acquires() const { return _acquires.load(std::memory_order_relaxed); }
    uint64_t total_busy() const { return _busy.load(std::memory_order_relaxed); }
    uint64_t total_leaks() const { return _leaks.load(std::memory_order_relaxed); }
    uint64_t total_order_violations() const { return _order_bad.load(std::memory_order_relaxed); }

    /* 把"现在每一条资源的锁是不是被谁握着"写成文本。
     * 运行卡住的时候这是唯一能一眼看出"谁握着什么"的办法 —— 见 --watchdog。 */
    void dump_held(std::string* out);

private:
    Entry*  intern_locked(uint64_t key);
    Entry*  find_locked(uint64_t key);
    int     hold_filtered(const std::vector<uint64_t>& keys, int kind, bool want, Hold* out);

    std::pmr::memory_resource*       _arena;
    mutable std::shared_mutex        _mtx;      /* 只保护 _tbl 的形状 */
    std::unordered_map<uint64_t, Entry*> _tbl;
    std::vector<Entry*>              _all;     /* 永不回收 */

    std::atomic<uint64_t> _acquires{ 0 };
    std::atomic<uint64_t> _busy{ 0 };
    std::atomic<uint64_t> _leaks{ 0 };
    std::atomic<uint64_t> _order_bad{ 0 };
};

/* 内部断言: 只在 debug 构建里生效 */
#ifdef MDPSR_DEBUG
void dbg_assert_fail(const char* expr, const char* file, int line);
#  define MDPSR_ASSERT(e) do { if (!(e)) ::mdpsr::dbg_assert_fail(#e, __FILE__, __LINE__); } while (0)
#else
#  define MDPSR_ASSERT(e) ((void)0)
#endif

} /* namespace mdpsr */
