#pragma once
/* ============================================================================
 *  mdpsr —— Message-Driven Plugin State Runtime
 *  宿主与插件的唯一二进制契约 (ABI v1)
 *
 *  这一版按"收敛后的规格"写, 与 archive/ 里的旧 ABI (v4) 不兼容。
 *
 *  ============================ 五条基本约定 ============================
 *
 *  1) 【消息是字节流】
 *     队列里一条消息序列化为连续字节, 帧头 16 字节:
 *         uint64_t _handle   目标 handle 的哈希
 *         int32_t  _cmd      命令号
 *         int32_t  _len      本条消息总字节长 (= 16 + body 长度)
 *         uint8_t  body[]    载荷, 由收件组件自己解释
 *     宿主只按 _handle 查表, 把「上下文 + body」原样交给 handle。
 *
 *  2) 【名称一律哈希成 uint64_t 当标识】
 *     mdpsr_hash64(名称) 是全进程唯一的物体标识, 也是 MainMap 的键。
 *     组件之间不需要 include 对方的头文件, 只要名字对上就能互相找到。
 *
 *  3) 【只有托管资源才用 Ptr】
 *         ptr      资源指针 (State* / Object* / Handle* / Queue*)
 *         valid    有效性 (原子)。valid == false 一律当作 nullptr,
 *                  任何读写都不允许进入。
 *         mtx      一把互斥锁 —— 它是"正在使用"的**唯一**声明方式。
 *
 *     ★ 没有读写计数。"谁在用谁持锁":
 *         · 宿主分发一条消息时, 对目标 handle (以及它声明用到的 state/object)
 *           取锁 -> 调用 -> 返回即释放;
 *         · 卸载时反过来: 先置 valid = false (挡住新的), 再取锁。
 *             拿到锁 == 没人在用 == 可以直接删;
 *             拿不到 == 有人正在用 == 这一轮先不删, 回滚到下轮再来。
 *
 *  4) 【Handle 是无状态接口函数】
 *     handle 只通过 (msgData/msgLen, ctx) 工作, 不持有任何可变全局状态。
 *     要跨消息保留的东西放 State, 由 MainMap 记录和跟踪生命周期。
 *
 *  5) 【dll 内的类必须自己做好"找不到资源"的防御】
 *     资源随时可能因为卸载而变无效。取到的指针是 nullptr 就直接返回错误码,
 *     绝不解引用 —— 这是 dll 作者的责任, 宿主不会替你兜。
 *
 *  ============================ 命名规范 ============================
 *
 *     Queue 类对象        Queue_<功能名称>          Queue_default
 *     Handle 导出函数      mdpsr_handle_<条目名>     <条目名> 形如 Mgr
 *     State 导出函数       mdpsr_state_<条目名>
 *     Object 导出函数      mdpsr_object_<条目名>  (+ _destroy)
 *     Queue 工厂           mdpsr_queue_<条目名>
 *
 *  条目名里的非 [A-Za-z0-9_] 字符在符号里统一变成 '_'
 *  (例如条目名 "Ticker.Handle.Tick" -> 导出 mdpsr_handle_Ticker_Handle_Tick)。
 *  宿主在 MainMap 里注册的键 = mdpsr_hash64(条目名原文), 所以两边永远一致。
 * ==========================================================================*/

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#  include <atomic>
#  include <cstdint>
#  include <memory_resource>
#  include <mutex>
#  include <new>
#  include <vector>
#endif

#define MDPSR_ABI_VERSION 1u

/* 名称哈希的字符串长度上限 */
#define MDPSR_NAME_MAX 64u
#define MDPSR_PATH_MAX 512u

/* 帧头长度: uint64 _handle + int32 _cmd + int32 _len */
#define MDPSR_MSG_HEADER 16u

/* ==========================================================================
 *  错误码
 * ==========================================================================*/
#define MDPSR_OK                   0
#define MDPSR_ERR_UNKNOWN_CMD      11001
#define MDPSR_ERR_BAD_CONFIG       11002
#define MDPSR_ERR_NULLPTR          11003
#define MDPSR_ERR_TYPE_MISMATCH    11004
#define MDPSR_ERR_CREATE_FAILED    11005
#define MDPSR_ERR_TIMEOUT          11006
#define MDPSR_ERR_BAD_STATE        11007
#define MDPSR_ERR_OUT_OF_RANGE     11008
#define MDPSR_ERR_NOT_FOUND        11009
#define MDPSR_ERR_ALREADY_EXISTS   11010
#define MDPSR_ERR_BUSY             11011
#define MDPSR_ERR_BAD_MESSAGE      11012
#define MDPSR_ERR_NAME_TOO_LONG    11013

#define MDPSR_ERR_MAP_KEY_MISSING  12001
#define MDPSR_ERR_MAP_INVALID      12002
#define MDPSR_ERR_MAP_NO_SPACE     12003

#define MDPSR_ERR_PLUGIN_NOT_FOUND 13001
#define MDPSR_ERR_PLUGIN_LOAD      13002
#define MDPSR_ERR_PLUGIN_ABI       13003
#define MDPSR_ERR_PLUGIN_EXPORT    13004
#define MDPSR_ERR_PLUGIN_BUSY      13005

#define MDPSR_ERR_QUEUE_NOT_FOUND  14001

/* cmd = 0 恒为"初始化", 约等于别的框架里的 Main */
#define MDPSR_CMD_INIT 0

/* ==========================================================================
 *  三条固定队列的名称 (都由宿主在初始化阶段建好)
 * ==========================================================================*/
#define MDPSR_QUEUE_PLUGINMGR_NAME  "Queue_pluginmgr"   /* 核心组件的线程 */
#define MDPSR_QUEUE_DEFAULT_NAME    "Queue_default"     /* 给所有组件的默认队列 */
#define MDPSR_QUEUE_WINDOWSGUI_NAME "Queue_windowsgui"  /* 挂在宿主主线程上 */

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  一、不透明类型的前置声明
 *
 *  mdpsr_ptr / mdpsr_map / mdpsr_factory_ctx 的完整定义在文件末尾的 C++
 *  段里 (它们本来就只有 C++ 能用)。C 段只需要"有这么个类型"。
 * ==========================================================================*/

typedef struct mdpsr_host  mdpsr_host;
typedef struct mdpsr_state mdpsr_state;

/* 队列描述: 队列工厂用它回答"这条队列要什么参数"。
 * host 接口里 queue_create 要用到它, 所以定义必须排在最前面。 */
typedef struct mdpsr_queue_desc {
    uint32_t struct_size;
    uint32_t capacity;      /* 初始环形缓冲字节, 0 = 默认 4096 */
    uint32_t pace_ms;       /* 两条消息之间至少间隔多少毫秒, 0 = 不限速 */
    uint32_t reserved;
} mdpsr_queue_desc;

struct mdpsr_ptr;
struct mdpsr_factory_ctx;
class  HashMap;
typedef HashMap mdpsr_map;

/* MainMap 里"一个条目属于哪一类"。条目按类型分表, 所以类型混淆在结构上
 * 不可能 —— 想拿 handle 却拿到了 queue, 得先伪造哈希, 那是另一回事。 */
enum mdpsr_kind {
    MDPSR_KIND_OBJECT = 0,
    MDPSR_KIND_STATE  = 1,
    MDPSR_KIND_HANDLE = 2,
    MDPSR_KIND_QUEUE  = 3
};

/* ==========================================================================
 *  二、宿主服务接口
 *
 *  所有函数第一个参数 host 都是 host->host (宿主实例, 原样回传)。
 * ==========================================================================*/
struct mdpsr_host {
    uint32_t abi_version;
    uint32_t struct_size;
    void*    host;

    /* ---- 基础 ---- */
    uint64_t    (*hash)(const char* name);
    void        (*log)(void* host, int level, const char* msg);
    const char* (*root_dir)(void* host);

    /* ---- MainMap ----
     * get        : 按键查一个**有效**条目 (valid == false 或不存在 -> NULL)
     * is_valid   : 原子读一下 valid (不持锁, 只是"快速看一眼")
     * lock/try_lock/unlock : Ptr.mtx, "我在用这个资源"的唯一声明方式
     * ptr_of     : 取 Ptr 里的资源指针
     * attach     : 把资源挂进 MainMap (键 -> ptr), 并拿到 Ptr
     * detach     : 摘表 (要在持有该条目锁的前提下调用)
     * invalidate : 置 valid = false (卸载第一步)
     * enum_keys  : 枚举全部键的快照
     *
     * 没有读写计数: "正在使用"完全由 Ptr.mtx 表达。
     * 所以调用方要么"看一眼 valid 就走", 要么"持锁用完再放", 没有第三种。 */
    mdpsr_ptr* (*map_get)(void* host, uint64_t key);
    int        (*map_is_valid)(void* host, mdpsr_ptr* p);
    int        (*map_lock)(void* host, mdpsr_ptr* p);
    int        (*map_try_lock)(void* host, mdpsr_ptr* p);
    void       (*map_unlock)(void* host, mdpsr_ptr* p);
    void*      (*map_ptr_of)(void* host, mdpsr_ptr* p);
    int        (*map_attach)(void* host, int kind, uint64_t key, const char* name,
                             void* ptr, mdpsr_ptr** out);
    int        (*map_detach)(void* host, uint64_t key);
    int        (*map_invalidate)(void* host, uint64_t key);
    int        (*map_enum_keys)(void* host, uint64_t* out, uint32_t cap, uint32_t* out_count);

    /* 二级字典的查询 (tbl 就是 Context 里的 object/state/handle/queue)。
     * 插件**只能**通过这两个函数碰二级字典 —— 它的真身在宿主那边, 换实现
     * 不应该影响插件。 */
    mdpsr_ptr* (*map_find)(mdpsr_map* tbl, uint64_t key);
    uint32_t   (*map_count)(mdpsr_map* tbl);

    /* ---- 插件装载 ---- */
    int (*plugin_load)(void* host, const char* manifest_rel, uint64_t* out_plugin);
    int (*plugin_unload)(void* host, uint64_t plugin);
    int (*plugin_keys)(void* host, uint64_t plugin, uint64_t* out,
                       uint32_t cap, uint32_t* out_count);
    /* 插件在清单里声明的 Init handle 名字 (没有就填空串)。
     * 装载者用它给新装上的插件点火。 */
    int (*init_handle_of)(void* host, uint64_t plugin, char* out, uint32_t cap);

    /* ---- 消息 ---- */
    int (*emit)(void* host, uint64_t handle_key, const void* body, size_t len);
    int (*queue_emit)(void* host, uint64_t queue_key, uint64_t handle_key,
                      const void* body, size_t len);

    /* ---- 队列 / 主线程 ----
     * queue_create / queue_bind 是"后门级"接口: 只有核心组件该用。
     * queue_create 幂等 (重名返回已有的)。 */
    int      (*queue_create)(void* host, const char* name, const mdpsr_queue_desc* desc,
                             uint64_t plugin, mdpsr_ptr** out);
    int      (*queue_bind)(void* host, uint64_t handle_key, uint64_t queue_key);
    uint64_t (*queue_key_for_plugin)(void* host, uint64_t plugin);
    mdpsr_ptr* (*queue_get)(void* host, uint64_t queue_key);
    int      (*queue_list)(void* host, uint64_t* out, uint32_t cap, uint32_t* out_count);
    uint32_t (*main_thread_id)(void* host);
};

/* ==========================================================================
 *  三、Context —— 分发上下文
 *
 *  所有指针只在本次 handle 调用期间有效; 需要的东西没了就是 NULL。
 *  "全部队列的哈希对队列指针"由 queue 字典提供 —— 查 Queue_default 就是
 *  在里面按哈希查一次; 查不到说明这个组件没遵循规范。
 * ==========================================================================*/
typedef struct mdpsr_context {
    mdpsr_map* object;   /* {名称哈希 : mdpsr_ptr*} 本插件自己的 Object */
    mdpsr_map* state;    /* {名称哈希 : mdpsr_ptr*} 全部 State */
    mdpsr_map* handle;   /* {名称哈希 : mdpsr_ptr*} 全部 Handle */
    mdpsr_map* queue;    /* {名称哈希 : mdpsr_ptr*} 全部 Queue  */

    struct mdpsr_host* host;   /* 宿主服务接口 (不要改它, 往下一层传就行) */
} mdpsr_context;

/* ==========================================================================
 *  四、标准消息形状
 * ==========================================================================*/
typedef struct mdpsr_cmd_head {
    int32_t _cmd;
    /* 该命令自己的参数紧随其后 */
} mdpsr_cmd_head;

typedef struct mdpsr_init_cmd {
    int32_t _cmd;      /* = MDPSR_CMD_INIT */
    int32_t _pad;
} mdpsr_init_cmd;

/* ==========================================================================
 *  五、函数签名
 * ==========================================================================*/

/* Handle: 无状态消息入口 */
typedef int (*mdpsr_handle_fn)(const uint8_t* msg_data, size_t msg_len,
                               const mdpsr_context* ctx);

typedef int (*mdpsr_queue_fn)(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out);

/* 后门: 宿主读完 config.json 后直接调用 (不经过消息队列) */
typedef int (*mdpsr_backdoor_fn)(const struct mdpsr_host* host);

/* ==========================================================================
 *  六、插件必需导出
 *
 *      uint32_t mdpsr_abi_version(void);
 *      int      mdpsr_module_init(const mdpsr_host* host, uint64_t plugin);
 *      void     mdpsr_module_fini(uint64_t plugin);      // 可选
 *
 *  以及清单里声明的那批工厂 (见文件头的命名规范)。
 * ==========================================================================*/

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ==========================================================================
 *  七、C++ 专属: Ptr / 二级字典 / State / 工厂上下文
 *
 *  为什么要放在 C 链接块外面:
 *    · HashMap 里有个模板成员函数, 有 C 链接就编不过;
 *    · 它们本来就是"宿主和插件用同一套工具链"才能共享的类型。
 *  纯 C 的插件看不到这一段, 也不该看到。
 * ==========================================================================*/
#ifdef __cplusplus

/* ---- 内存资源 ---- */
typedef std::pmr::memory_resource mdpsr_resource;

/* ---- 二级字典 (哈希 -> mdpsr_ptr*) ----
 *
 *  实现: 开放寻址 + 线性探测, 永不真正删除 (要删就留 tombstone)。
 *  迭代顺序不稳定, 这是故意的 —— 它不承担任何顺序语义。
 *
 *  ★ 插件不要直接调它的成员 (宿主可能换实现), 用 host->map_find。 */
class HashMap {
public:
    HashMap() = default;
    explicit HashMap(size_t reserve_hint) { reserve(reserve_hint); }

    void reserve(size_t n) {
        if (n <= _cap) return;
        size_t cap = 8;
        while (cap < n * 2) cap <<= 1;
        rehash(cap);
    }

    void clear() {
        for (auto& s : _slots) { s.used = 0; s.key = 0; s.val = nullptr; }
        _count = 0;
    }

    size_t size() const { return _count; }
    size_t capacity() const { return _cap; }

    /* 返回"值的地址"; 键不存在返回 nullptr。
     * 这个指针在下次插入之前一直有效 (从不 erase、从不收缩) ——
     * 这正是 Context 里的裸指针能安全裸传的前提。 */
    void** find(uint64_t key) {
        if (_cap == 0) return nullptr;
        const size_t m = _cap - 1;
        size_t i = static_cast<size_t>(key) & m;
        for (size_t probe = 0; probe < _cap; ++probe) {
            Slot& s = _slots[i];
            if (!s.used) return nullptr;
            if (s.used == 1 && s.key == key) return &s.val;
            i = (i + 1) & m;
        }
        return nullptr;
    }

    void* get(uint64_t key) const {
        auto* self = const_cast<HashMap*>(this);
        void** v = self->find(key);
        return v ? *v : nullptr;
    }

    bool has(uint64_t key) const { return const_cast<HashMap*>(this)->find(key) != nullptr; }

    void** set(uint64_t key, void* val) {
        if (_cap == 0) rehash(16);
        if ((_count + 1) * 4 >= _cap * 3) rehash(_cap * 2);
        return set_no_grow(key, val);
    }

    void** insert(uint64_t key, void* val) {
        if (_cap == 0) rehash(16);
        if ((_count + 1) * 4 >= _cap * 3) rehash(_cap * 2);
        if (find(key)) return nullptr;
        return set_no_grow(key, val);
    }

    template <typename Fn>
    void each(Fn&& fn) const {
        for (const auto& s : _slots) {
            if (s.used == 1) fn(s.key, s.val);
        }
    }

private:
    struct Slot {
        uint64_t key = 0;
        void*    val = nullptr;
        uint8_t  used = 0;      /* 0 空 / 1 占用 / 2 tombstone */
    };

    void** set_no_grow(uint64_t key, void* val) {
        const size_t m = _cap - 1;
        size_t i = static_cast<size_t>(key) & m;
        size_t tomb = static_cast<size_t>(-1);
        for (;;) {
            Slot& s = _slots[i];
            if (s.used == 0) {
                if (tomb != static_cast<size_t>(-1)) {
                    Slot& t = _slots[tomb];
                    t.key = key; t.val = val; t.used = 1;
                    ++_count;
                    return &t.val;
                }
                s.key = key; s.val = val; s.used = 1;
                ++_count;
                return &s.val;
            }
            if (s.used == 1 && s.key == key) { s.val = val; return &s.val; }
            if (s.used == 2 && tomb == static_cast<size_t>(-1)) tomb = i;
            i = (i + 1) & m;
        }
    }

    void rehash(size_t newcap) {
        std::vector<Slot> old;
        old.swap(_slots);
        const size_t oldcap = _cap;
        _slots.assign(newcap, Slot{});
        _cap = newcap;
        _count = 0;
        for (size_t k = 0; k < oldcap; ++k) {
            if (old[k].used == 1) set_no_grow(old[k].key, old[k].val);
        }
    }

    std::vector<Slot> _slots;
    size_t _cap = 0;
    size_t _count = 0;
};

typedef HashMap mdpsr_map;

/* ---- Ptr: MainMap 里唯一的条目形状 ---- */
typedef struct mdpsr_ptr {
    void* ptr;      /* State* / Object* / Handle* / Queue* */

    /* 有效性。0 一律当作 nullptr。规则: 只有"持有下面那把锁的人"才能把它
     * 改成 0 (卸载路径就是这么干的)。读它不需要持锁 —— 想快速看一眼就读它。 */
    std::atomic<int32_t> valid;

    /* 真正的门闸: 谁在用谁持有。契约不公开它的类型, 只用 host api 操作。
     * 留这块定长存储是为了让 Ptr 能被放进池子; 下面的 static_assert 保证
     * 它一定装得下 (装不下编译期就报错, 不会静默踩内存)。 */
    union {
        unsigned char _mtx_storage[128];
        void*         _mtx_align;
    };
} mdpsr_ptr;

static_assert(sizeof(std::mutex) <= 128, "Ptr 的 mtx 存储区太小");
static_assert(alignof(std::mutex) <= alignof(void*), "Ptr 的 mtx 对齐不够");
static_assert(sizeof(mdpsr_ptr) == 144, "mdpsr_ptr 布局变了, ABI 必须跟着升版本");

/* ---- State: 公用数据对象 ----
 * 载荷是任意字节 (可变长), 由创建它的 State 工厂自行解释。 */
struct mdpsr_state {
    uint64_t    name;      /* Hash(条目名), 也是 MainMap 的键。宿主会覆盖 */
    uint64_t    plugin;    /* 所属插件键。宿主会覆盖 */
    uint32_t    kind;      /* 插件自定义类型标签, 方便接手的插件辨认载荷 */
    uint32_t    flags;
    size_t      size;      /* payload 字节数 */
    void*       payload;   /* 公用数据本体 */
    mdpsr_resource* res;   /* 分配 payload 用的池 */
    void (*destroy)(mdpsr_state*);   /* 可选析构 (载荷里放了 C++ 对象就必须设) */
};

/* ---- 工厂上下文: 清单里声明的东西原样传给工厂 ---- */
typedef struct mdpsr_factory_ctx {
    const char*        name;          /* 条目名 */
    const char*        plugin_name;   /* 插件名 (清单的 "name") */
    const char*        manifest;      /* 本清单相对运行时根目录的路径 */
    const char*        manifest_dir;  /* 本清单所在目录 */
    const char* const* list;          /* 清单里的字符串数组 (没有就是 NULL) */
    size_t             list_count;
    uint64_t           plugin;        /* 本插件在 MainMap 里的键 */
    struct mdpsr_host* host;
    /* ★ 该用哪个池 —— State 工厂拿到 Pool_state, Object 工厂拿到 Pool_object,
     *   Queue 工厂拿到 Pool_msg。用错池 = 卸载时收不回来, 所以宿主明确给。 */
    mdpsr_resource*    pool;
} mdpsr_factory_ctx;

/* State 工厂: 只能产出"数值对象 / 公开数据对象", 从 ctx->pool (Pool_state) 分配 */
typedef mdpsr_state* (*mdpsr_state_fn)(const mdpsr_factory_ctx* ctx);
/* Object 工厂: 类对象, 从 ctx->pool (Pool_object) 分配 */
typedef void* (*mdpsr_object_fn)(const mdpsr_factory_ctx* ctx);
typedef void  (*mdpsr_object_destroy_fn)(void* instance);

/* ==========================================================================
 *  八、共享工具 (宿主与插件必须完全一致)
 * ==========================================================================*/

/* FNV-1a 变体; 空指针返回 0, 结果 0 映射为 1。
 * 只吃 ASCII/UTF-8 字节, 与时区/码页无关。 */
static inline uint64_t mdpsr_hash64(const char* s) {
    if (!s) return 0;
    uint64_t h = 1469598103934665603ull;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 1099511628211ull;
    }
    return h ? h : 1ull;
}

/* 从消息字节流安全地拷一段 POD; 长度不够返回 false */
template <typename T>
static inline bool mdpsr_read_pod(const uint8_t* data, size_t len, T* out) {
    if (!data || !out || len < sizeof(T)) return false;
    uint8_t* q = reinterpret_cast<uint8_t*>(out);
    for (size_t i = 0; i < sizeof(T); ++i) q[i] = data[i];
    return true;
}

/* 读 NUL 结尾字符串 (在 [data+offset, data+len) 内), 越界返回 nullptr */
static inline const char* mdpsr_read_str(const uint8_t* data, size_t len, size_t offset) {
    if (!data || offset >= len) return nullptr;
    const char* s = reinterpret_cast<const char*>(data + offset);
    for (size_t i = offset; i < len; ++i) {
        if (s[i - offset] == '\0') return s;
    }
    return nullptr;
}

/* 相对路径拼接 + 消解 "." / ".."; 结果始终是"相对运行时根目录"的形式 */
static inline void mdpsr_path_join(char* out, size_t cap,
                                   const char* base, const char* rel) {
    if (!out || cap == 0) return;
    out[0] = '\0';

    auto slen = [](const char* s) { size_t n = 0; if (s) while (s[n]) ++n; return n; };

    auto copy_plain = [&](const char* s) {
        size_t i = 0;
        if (s) { for (; s[i] && i + 1 < cap; ++i) out[i] = s[i]; }
        out[i] = '\0';
    };
    if (!rel || !*rel) { copy_plain(base); return; }

    /* 绝对路径直接返回 (先确认长度, 避免单字符 rel 越界读) */
    {
        const size_t rl = slen(rel);
        if (rel[0] == '/' || rel[0] == '\\' || (rl >= 2 && rel[1] == ':')) {
            copy_plain(rel);
            return;
        }
    }

    char tmp[MDPSR_PATH_MAX * 2];
    size_t n = 0;
    auto append = [&](const char* s, size_t len) {
        for (size_t i = 0; i < len && n + 1 < sizeof(tmp); ++i) tmp[n++] = s[i];
    };
    if (base && *base) {
        append(base, slen(base));
        if (n && tmp[n - 1] != '/') tmp[n++] = '/';
    }
    append(rel, slen(rel));
    tmp[n] = '\0';

    const char* seg[128];
    size_t seglen[128];
    size_t cnt = 0, i = 0;
    while (i < n && cnt < 128) {
        while (i < n && (tmp[i] == '/' || tmp[i] == '\\')) ++i;
        const size_t s = i;
        while (i < n && tmp[i] != '/' && tmp[i] != '\\') ++i;
        const size_t l = i - s;
        if (l == 0) continue;
        if (l == 1 && tmp[s] == '.') continue;
        if (l == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
            if (cnt > 0) --cnt;
            continue;
        }
        seg[cnt] = tmp + s;
        seglen[cnt] = l;
        ++cnt;
    }

    size_t w = 0;
    for (size_t k = 0; k < cnt; ++k) {
        if (k) { if (w + 1 < cap) out[w++] = '/'; }
        for (size_t j = 0; j < seglen[k] && w + 1 < cap; ++j) out[w++] = seg[k][j];
    }
    out[w] = '\0';
}

/* 组一条 core 消息: { int32 _cmd; int32 _pad; uint64 _plugin; char _path[] } */
typedef struct mdpsr_core_cmd {
    int32_t  _cmd;
    int32_t  _pad;
    uint64_t _plugin;
    /* char _path[]; 紧跟其后, NUL 结尾 */
} mdpsr_core_cmd;

static inline size_t mdpsr_core_pack(uint8_t* out, size_t cap,
                                     int32_t cmd, uint64_t plugin, const char* path) {
    size_t plen = 0;
    if (path) { while (path[plen]) ++plen; }
    if (!out || cap < sizeof(mdpsr_core_cmd) + plen + 1) return 0;
    mdpsr_core_cmd h{};
    h._cmd = cmd;
    h._pad = 0;
    h._plugin = plugin;
    size_t n = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&h);
    for (size_t i = 0; i < sizeof(h); ++i) out[n++] = p[i];
    for (size_t i = 0; i < plen; ++i) out[n++] = static_cast<uint8_t>(path[i]);
    out[n++] = 0;
    return n;
}

#endif /* __cplusplus */

/* 插件导出宏 */
#ifdef _WIN32
#  define MDPSR_EXPORT extern "C" __declspec(dllexport)
#else
#  define MDPSR_EXPORT extern "C" __attribute__((visibility("default")))
#endif
