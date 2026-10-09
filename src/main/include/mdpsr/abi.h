/* ============================================================================
 *  mdpsr —— Message-Driven Plugin State Runtime
 *  abi.h  ★ 宿主与插件之间唯一的二进制契约 (ABI v2)
 *
 *  这一版与 archive/old_v1 的 ABI v1 不兼容, 是重写而不是兼容升级。
 *
 *  与 v1 的三个根本区别 (都是为了让"热插拔"真的稳定):
 *
 *   1) 【契约里没有 C++ 了】
 *      v1 把 HashMap / std::atomic / std::mutex / pmr 放进了头文件, 于是
 *      "ABI" 实际是"同版本 MSVC 才能互通"。v2 是纯 C99 头文件, 只有 POD
 *      结构体和函数指针表, 换个编译器/换个语言照样能写插件。
 *
 *   2) 【插件拿不到表内部, 只能按键借资源】
 *      v1 把四张二级字典的裸哈希表指针交给插件, 插件可以无锁遍历 —— 宿主
 *      一边 rehash 一边被读, 那是 v1 最致命的数据竞争。
 *      v2 只有 acquire/release: 按键借资源, 借到就持锁, 用完全部归还。
 *      资源控制块永不释放, 所以永远拿不到悬垂指针; 代际 (gen) 负责告诉你
 *      "手里这个还是不是你当初要的那一个"。
 *
 *   3) 【每个插件一个内存池, 卸载时整池回收】
 *      v1 的 State 载荷永远留在全局池里, 卸载等于不回收。
 *      v2 的工厂从 ctx->pool 分配 —— 那是本插件自己的池, 卸载时整池销毁。
 *
 *  ============================ 四条基本约定 ============================
 *
 *  1) 消息是字节流。帧头 24 字节:
 *         uint64_t dst   目标 handle 的键 (hash)
 *         uint64_t src   发送者 handle 的键, 0 = 宿主
 *         int32_t  cmd   命令号
 *         int32_t  len   本条消息总字节长 (= 24 + body)
 *     ★ 判命令只读 msg->cmd; body 里的参数"长度够才读", 绝不能先要求整条结构体。
 *
 *  2) 名称一律哈希成 uint64_t 当标识 (mdpsr_hash64)。条目名全局唯一 ——
 *     装载时真的检查重名, 撞了就装不上, 而不是静默顶替。
 *
 *  3) 只有托管资源 (State / Object / Handle / Queue) 进注册表, 用 (key, gen)
 *     标识。借用规约:
 *           acquire(key, gen)  ->  持锁, 拿到指针
 *           release(key)       ->  归还   (或让宿主在调用结束时兜底归还)
 *     拿不到锁不是错, 是"忙", 回滚消息下轮再试即可。
 *
 *  4) 【不要跨插件同步调用函数】。插件之间只通过 emit 投消息。这是"宿主可以
 *     安全 FreeLibrary"的前提: 任何时刻正在执行的插件代码, 必然是被宿主在
 *     分发路径上持着锁调起来的, 而卸载会先拿到全部锁再卸。
 * ==========================================================================*/
#ifndef MDPSR_ABI_H
#define MDPSR_ABI_H

#include <stdint.h>
#include <stddef.h>

/* 这个头文件是被 C 和 C++ 双向包含的: 契约本身是纯 C 的 (只有 POD 结构体和
 * 函数指针), 所以加上 extern "C" 只是为了让 C++ 侧的类型/函数不带上名字修饰。 */
#ifdef __cplusplus
extern "C" {
#endif

#define MDPSR_ABI_VERSION 3u

#define MDPSR_NAME_MAX 64u
#define MDPSR_PATH_MAX 512u
#define MDPSR_MSG_HEADER 24u

/* 插件在工厂里能拿到的自定义字符串数组的数量上限 */
#define MDPSR_LIST_MAX 16u

/* ==========================================================================
 *  错误码
 * ==========================================================================*/
#define MDPSR_OK                    0

/* 通用 */
#define MDPSR_ERR_UNKNOWN_CMD       11001
#define MDPSR_ERR_BAD_CONFIG        11002
#define MDPSR_ERR_NULLPTR           11003
#define MDPSR_ERR_TYPE_MISMATCH     11004
#define MDPSR_ERR_CREATE_FAILED     11005
#define MDPSR_ERR_TIMEOUT           11006
#define MDPSR_ERR_BAD_STATE         11007
#define MDPSR_ERR_OUT_OF_RANGE      11008
#define MDPSR_ERR_NOT_FOUND         11009
#define MDPSR_ERR_ALREADY_EXISTS    11010
#define MDPSR_ERR_BUSY              11011
#define MDPSR_ERR_BAD_MESSAGE       11012
#define MDPSR_ERR_NAME_TOO_LONG     11013
/* v2 新增 */
#define MDPSR_ERR_LOCK_ORDER        11014   /* 违反"按键升序借用"规则 */
#define MDPSR_ERR_STALE             11015   /* 代际对不上: 资源换过一代了 */
#define MDPSR_ERR_NO_PERM           11016   /* 没有 kernel 能力 */
#define MDPSR_ERR_REENTRANT         11017   /* 在插件调用里又要求装卸 */
#define MDPSR_ERR_POOL_MISMATCH     11018   /* 拿了别的插件的池 */
#define MDPSR_ERR_NOT_READY         11019   /* 插件还没装好 / 正在卸 */

/* 注册表 */
#define MDPSR_ERR_KEY_MISSING       12001
#define MDPSR_ERR_ENTRY_INVALID     12002
#define MDPSR_ERR_NO_SPACE          12003

/* 插件 */
#define MDPSR_ERR_PLUGIN_NOT_FOUND  13001
#define MDPSR_ERR_PLUGIN_LOAD       13002
#define MDPSR_ERR_PLUGIN_ABI        13003
#define MDPSR_ERR_PLUGIN_EXPORT     13004
#define MDPSR_ERR_PLUGIN_BUSY       13005
#define MDPSR_ERR_PLUGIN_INIT       13006

/* 队列 */
#define MDPSR_ERR_QUEUE_NOT_FOUND   14001
#define MDPSR_ERR_QUEUE_FULL        14002

/* ==========================================================================
 *  保留命令号。插件自己的命令从 MDPSR_CMD_USER_BASE 开始。
 *  cmd = 0 恒为"初始化", 约等于别的框架里的 Main。
 * ==========================================================================*/
#define MDPSR_CMD_INIT      0
#define MDPSR_CMD_REPLY     1     /* 通用成功回复   body = mdpsr_reply */
#define MDPSR_CMD_FAIL      2     /* 通用失败回复   body = mdpsr_fail  */
#define MDPSR_CMD_PING      3     /* body = uint32 seq, 收到请回 PONG */
#define MDPSR_CMD_PONG      4
#define MDPSR_CMD_STOP      5     /* 请对方停下自转循环 */
#define MDPSR_CMD_USER_BASE 16

/* 两条固定队列。
 * ★ v3 起宿主不再拥有"挂主线程"的队列: 主线程只做 init/boot 然后等退出,
 *   GUI 这类需要线程亲和的东西由插件自己开线程实现 (见 components/winmsg)。 */
#define MDPSR_QUEUE_SYS_NAME     "Queue_sys"       /* 内核/管理: 装卸都在它上面跑 */
#define MDPSR_QUEUE_DEFAULT_NAME "Queue_default"   /* 默认队列 */

/* ==========================================================================
 *  条目种类 / 插件状态
 * ==========================================================================*/
enum {
    MDPSR_KIND_OBJECT = 0,   /* 类实例, 私有 */
    MDPSR_KIND_STATE  = 1,   /* 公用数据对象 */
    MDPSR_KIND_HANDLE = 2,   /* 无状态消息入口 */
    MDPSR_KIND_QUEUE  = 3,   /* 队列本体 */
    MDPSR_KIND__COUNT = 4
};

enum {
    MDPSR_PLUGIN_EMPTY     = 0,   /* 没装 */
    MDPSR_PLUGIN_LOADING   = 1,
    MDPSR_PLUGIN_READY     = 2,
    MDPSR_PLUGIN_UNLOADING = 3,
    MDPSR_PLUGIN_FAILED    = 4
};

/* plugin_install / plugin_reload 的开关 */
#define MDPSR_INSTALL_SYNC_INIT 0x1u   /* 在调用者线程上同步跑一次 Init handle 的 cmd=0 */
#define MDPSR_INSTALL_NO_INIT   0x2u   /* 不跑 Init */

/* plugin_uninstall 的开关 */
#define MDPSR_UNINSTALL_TRY_ONLY 0x1u  /* 拿不到锁立刻返回 BUSY, 不在内部重试 */

/* ==========================================================================
 *  不透明类型
 * ==========================================================================*/
typedef struct mdpsr_host mdpsr_host;
typedef struct mdpsr_pool mdpsr_pool;

/* ==========================================================================
 *  消息
 * ==========================================================================*/
typedef struct mdpsr_msg {
    uint64_t dst;
    uint64_t src;
    int32_t  cmd;
    int32_t  len;         /* 整帧字节数 = MDPSR_MSG_HEADER + body 长度 */
} mdpsr_msg;

/* 回复载荷 (cmd = MDPSR_CMD_REPLY) */
typedef struct mdpsr_reply {
    int32_t  cmd;         /* 回复的是哪条命令 */
    int32_t  code;        /* 0 = 成功 */
    uint64_t src;         /* 回复者的 handle 键 */
    uint32_t seq;         /* 请求里带的序号, 原样回传 */
    uint32_t value;       /* 附带的一个数值, 语义由协议定 */
} mdpsr_reply;

/* 失败载荷 (cmd = MDPSR_CMD_FAIL) */
typedef struct mdpsr_fail {
    int32_t  code;        /* 错误码 */
    int32_t  reason;      /* MDPSR_FAIL_* */
    uint64_t dst;         /* 本来是投给谁的 */
    uint32_t seq;
    uint32_t reserved;
} mdpsr_fail;

#define MDPSR_FAIL_DEAD_HANDLE 1   /* 目标 handle 不存在或已失效 */
#define MDPSR_FAIL_QUEUE_FULL  2   /* 队列满了, 消息没进队 */
#define MDPSR_FAIL_REJECTED    3   /* 收件人明确拒绝 */
#define MDPSR_FAIL_INTERNAL    4

/* ==========================================================================
 *  队列描述 / 统计
 * ==========================================================================*/
typedef struct mdpsr_queue_desc {
    uint32_t struct_size;
    uint32_t capacity;     /* 环形缓冲字节上限 (硬上限: 满了 push 直接失败) */
    uint32_t pace_ms;      /* 两条消息的处理开始时间至少隔多少毫秒, 0 = 不限速 */
    uint32_t flags;        /* 保留 */
} mdpsr_queue_desc;

typedef struct mdpsr_queue_stats {
    uint32_t struct_size;
    uint32_t pace_ms;
    uint32_t capacity;
    uint32_t used;         /* 当前占用的字节 */
    uint32_t high_water;   /* 历史最高水位 */
    uint32_t paused;       /* 被限速挡下的累计次数 (v3 起用它替掉 on_main_thread) */
    uint64_t pushed;
    uint64_t popped;
    uint64_t full;         /* 因为满而拒绝的次数 */
    uint64_t paced;        /* 被限速挡下的次数 */
    uint64_t errors;
} mdpsr_queue_stats;

/* ==========================================================================
 *  插件信息
 * ==========================================================================*/
typedef struct mdpsr_plugin_info {
    uint32_t struct_size;
    uint32_t status;       /* MDPSR_PLUGIN_* */
    uint64_t key;
    uint32_t gen;          /* 每次成功装载 +1 */
    uint32_t entry_count;
    uint32_t module_count;
    uint32_t install_count;
    uint32_t fail_count;
    uint32_t reserved;
    char     name[MDPSR_NAME_MAX];
    char     manifest[MDPSR_PATH_MAX];
} mdpsr_plugin_info;

typedef struct mdpsr_install_opts {
    uint32_t struct_size;
    uint32_t flags;        /* MDPSR_INSTALL_* */
} mdpsr_install_opts;

/* ==========================================================================
 *  静态配置 —— 清单里的 "Static_State" 字段
 *
 *  清单顶层可以写:
 *      "Static_State": {
 *          "my.cfg": {                       <- 初级名称 (宿主拿它登记一个条目)
 *              "greeting": "你好, mdpsr",     <- 次级名称 -> 字符串
 *              "scale":    "2"
 *          }
 *      }
 *
 *  ★ 这是【宿主装载插件时顺便做的事】, 不是插件工厂的活:
 *      宿主解析清单 -> 为每个【初级名称】登记一个 STATE 条目,
 *          条目键   = mdpsr_hash64(初级名称)
 *          条目载荷 = mdpsr_static_dict (见下)
 *      条目和本插件的其它条目一起上线 (publish 门闸), 卸载时一起摘掉。
 *    所以插件不需要任何新工厂、也不需要 JSON 解析器 —— 直接 acquire:
 *
 *      void* p = NULL; uint32_t g = 0;
 *      if (mdpsr_acquire(host, mdpsr_hash64("my.cfg"), 0, &p, &g) == MDPSR_OK) {
 *          const mdpsr_static_dict* d = (const mdpsr_static_dict*)p;
 *          const mdpsr_static_item* it = mdpsr_static_find(d, mdpsr_hash64("scale"));
 *          ...
 *      }
 *
 *  ★ 因为它是注册表里的 STATE 条目, 所以【可以在运行时改】: 借到手就能写
 *    (内核只管锁和生命周期, 不管内容)。改的时候注意 len 要跟着变。
 *
 *  ★ text 是 UTF-8 字节流, 【不保证 NUL 结尾】—— 一律按 len 读, 别当 C 字符串用。
 *    配置里写中文等多字节内容时, 存的就是它的 UTF-8 编码; 要当字符串用就按
 *    len 转成 std::string(本身即 UTF-8), 不要假定单字节。
 *    text 恒非空 (空值指向一个空字节), 所以判空只看 len, 不必先判 text。
 *
 *  ★ 值必须是【字符串】。写成数字/对象/数组会在装载时直接报 BAD_CONFIG ——
 *    宁可报错也不替你猜 ("2" 和 2 到底想表达什么, 只有插件自己知道)。
 * ==========================================================================*/
typedef struct mdpsr_static_item {
    uint64_t       key;      /* mdpsr_hash64(次级名称) */
    const uint8_t* text;     /* UTF-8 字节流, 不保证 NUL 结尾, 恒非空 */
    uint32_t       len;      /* 字节数 (不是字符数!) */
    uint32_t       reserved;
} mdpsr_static_item;

/* 登记进注册表的静态配置条目载荷 —— acquire(初级名称) 拿到的就是它。
 * 用 magic 自证一下拿到的确实是字典而不是别的 STATE (借错东西时不至于乱读)。 */
#define MDPSR_STATIC_MAGIC 0x4D535444u   /* 'MSTD' */

typedef struct mdpsr_static_dict {
    uint32_t                 magic;   /* MDPSR_STATIC_MAGIC */
    uint32_t                 count;   /* items 的项数 */
    const mdpsr_static_item* items;   /* count == 0 时为 NULL */
    const char*              name;    /* 初级名称 (NUL 结尾, 只为日志方便) */
} mdpsr_static_dict;

/* 按 次级名称哈希 查一项; 找不到返回 NULL。 */
static inline const mdpsr_static_item* mdpsr_static_find(const mdpsr_static_dict* d,
                                                         uint64_t key) {
    if (!d || d->magic != MDPSR_STATIC_MAGIC || !d->items) return NULL;
    for (uint32_t i = 0; i < d->count; ++i) {
        if (d->items[i].key == key) return &d->items[i];
    }
    return NULL;
}

/* ==========================================================================
 *  工厂上下文 —— 清单里声明的东西原样交给工厂
 * ==========================================================================*/
typedef struct mdpsr_factory_ctx {
    uint32_t    struct_size;
    uint32_t    list_count;
    const char* name;          /* 本条目名 */
    const char* plugin_name;   /* 插件名 (清单的 "name") */
    const char* manifest_path; /* 本清单相对运行时根目录的路径 */
    const char* manifest_dir;  /* 本清单所在目录 */
    uint64_t    plugin;        /* 本插件的键 */
    uint32_t    plugin_gen;
    uint32_t    reserved;
    const char* const* list;   /* "list" 字段 (可选) */
    const mdpsr_host* host;
    mdpsr_pool* pool;          /* ★ 本插件专属池: 卸载时整池回收 */
} mdpsr_factory_ctx;

/* ==========================================================================
 *  调用上下文 —— 一次 handle 调用能看到的东西
 * ==========================================================================*/
typedef struct mdpsr_ctx {
    uint64_t handle;       /* 我自己 (消息的 dst) */
    uint64_t plugin;       /* 我属于哪个插件 */
    uint32_t plugin_gen;
    uint32_t reserved;
    const mdpsr_host* host;
} mdpsr_ctx;

/* ==========================================================================
 *  宿主服务接口
 *
 *  所有函数第一个参数都是 host->self (原样回传)。头文件末尾有一层 inline
 *  包装, 插件里直接写 mdpsr_acquire(host, ...) 就行, 不用碰 self。
 * ==========================================================================*/
struct mdpsr_host {
    uint32_t abi_version;
    uint32_t struct_size;
    void*    self;

    /* ---- 基础 ---- */
    uint64_t    (*hash)(const char* name);
    void        (*log)(void* self, int level, const char* msg);
    const char* (*root_dir)(void* self);
    uint64_t    (*millis)(void* self);            /* 单调时钟, 毫秒 */
    uint32_t    (*thread_id)(void* self);
    uint32_t    (*main_thread_id)(void* self);
    void        (*sleep_ms)(void* self, uint32_t ms);

    /* ---- 注册表: 借 / 还 ----
     * acquire : 按键借资源。want_gen 为 0 表示"哪一代都行"。
     *           成功 => 持有该条目的锁, *out_ptr 有效直到 release。
     *           失败 => BUSY(有人正在用) / STALE / KEY_MISSING / ENTRY_INVALID
     * acquire_many : 一次借多个, 宿主内部按键升序, 全有或全无 (推荐)
     * release : 归还一个; 没借过的键返回 ENTRY_INVALID
     * release_all : 归还本次调用里借到的全部 (宿主在调用返回时也兜底做一次)
     * held_count : 当前借了几个
     * gen_of  : 无锁快照, 0 = 不存在或已失效 ("服务在不在"就该用它)
     * kind_of : 条目种类, -1 = 没有
     * name_of : 条目名副本, 给日志用
     * ★ 同一个线程里多次 acquire, 键必须严格递增, 否则返回 LOCK_ORDER。
     *   这把"插件之间互相咬锁"的死锁在结构上消灭掉了 —— 见 doc/架构.md。 */
    int      (*acquire)(void* self, uint64_t key, uint32_t want_gen,
                        void** out_ptr, uint32_t* out_gen);
    int      (*acquire_many)(void* self, const uint64_t* keys, const uint32_t* gens,
                             uint32_t n, void** out_ptrs, uint32_t* out_gens);
    void     (*release)(void* self, uint64_t key);
    void     (*release_all)(void* self);
    int      (*held_count)(void* self);
    uint32_t (*gen_of)(void* self, uint64_t key);
    int      (*kind_of)(void* self, uint64_t key);
    int      (*name_of)(void* self, uint64_t key, char* out, uint32_t cap);

    /* ---- 内存 ----
     * pool_alloc / pool_free 只作用于"当前调用所属插件自己的池"; 传别人的池
     * 会拿到 POOL_MISMATCH。池在卸载时整池销毁, 所以"忘了 free"最坏也只是
     * 在这个插件的一生里占着, 不会泄漏到别人头上。 */
    void* (*pool_alloc)(void* self, mdpsr_pool* pool, size_t bytes, size_t align);
    void  (*pool_free)(void* self, mdpsr_pool* pool, void* p, size_t bytes, size_t align);

    /* ---- 消息 ----
     * emit      : 投给 dst 所在队列, src = 调用上下文里的我自己
     * emit_from : 显式指定 src
     * emit_to   : 显式指定投到哪条队列
     * reply     : 给 req->src 回一条 (src 为 0 就只记日志, 不投) */
    int (*emit)(void* self, uint64_t dst, int32_t cmd, const void* body, uint32_t len);
    int (*emit_from)(void* self, uint64_t dst, uint64_t src, int32_t cmd,
                     const void* body, uint32_t len);
    int (*emit_to)(void* self, uint64_t queue_key, uint64_t dst, uint64_t src,
                   int32_t cmd, const void* body, uint32_t len);
    int (*reply)(void* self, const mdpsr_msg* req, int32_t cmd,
                 const void* body, uint32_t len);

    /* ---- 队列 ---- */
    int      (*queue_create)(void* self, const char* name, const mdpsr_queue_desc* desc,
                             uint64_t* out_key);
    int      (*queue_bind)(void* self, uint64_t handle_key, uint64_t queue_key);
    uint64_t (*queue_of)(void* self, uint64_t handle_key);
    int      (*queue_stats)(void* self, uint64_t queue_key, mdpsr_queue_stats* out);
    int      (*queue_list)(void* self, uint64_t* out, uint32_t cap, uint32_t* out_count);

    /* ---- 插件装卸 (需要 kernel 能力) ----
     * 只有清单里写了 "kernel": true 的插件能调; 宿主自己是 0 号特权调用者。
     * opts 可以传 NULL (等于 SYNC_INIT)。 */
    int      (*plugin_install)(void* self, const char* manifest_rel,
                               const mdpsr_install_opts* opts, uint64_t* out_plugin);
    int      (*plugin_uninstall)(void* self, uint64_t plugin, uint32_t flags);
    int      (*plugin_reload)(void* self, uint64_t plugin, const mdpsr_install_opts* opts);
    int      (*plugin_find)(void* self, const char* name, uint64_t* out_plugin,
                            uint32_t* out_gen);
    int      (*plugin_info)(void* self, uint64_t plugin, mdpsr_plugin_info* out);
    int      (*plugin_list)(void* self, uint64_t* out, uint32_t cap, uint32_t* out_count);
    uint64_t (*current_plugin)(void* self);   /* 当前调用属于哪个插件 (0 = 宿主) */
};

/* ==========================================================================
 *  插件导出的东西
 * ==========================================================================*/
typedef void* (*mdpsr_state_fn)(const mdpsr_factory_ctx* ctx);   /* State 工厂 */
typedef void* (*mdpsr_object_fn)(const mdpsr_factory_ctx* ctx);  /* Object 工厂 */
/* 析构 (导出名 = 工厂名 + "_destroy"), State / Object 都能用。
 * 只做析构, 不做 free —— 内存由池整池回收; 想提前还就 mdpsr_fctx_free。 */
typedef void  (*mdpsr_destroy_fn)(void* instance, const mdpsr_factory_ctx* ctx);
typedef int   (*mdpsr_queue_fn)(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out);
typedef int   (*mdpsr_handle_fn)(const mdpsr_msg* msg, const uint8_t* body,
                                 uint32_t body_len, const mdpsr_ctx* ctx);
typedef int   (*mdpsr_module_init_fn)(const mdpsr_factory_ctx* ctx);
typedef void  (*mdpsr_module_fini_fn)(void);

/* ==========================================================================
 *  共享工具 (宿主与插件必须逐字节一致)
 * ==========================================================================*/

/* FNV-1a 变体; 空指针返回 0, 结果 0 映射为 1。
 * 只吃 ASCII/UTF-8 字节, 与时区/码页无关。 */
static inline uint64_t mdpsr_hash64(const char* s) {
    uint64_t h = 1469598103934665603ull;
    if (!s) return 0;
    while (*s) {
        h ^= (uint64_t)(uint8_t)(*s++);
        h *= 1099511628211ull;
    }
    return h ? h : 1ull;
}

/* 插件键: "plugin:<名字>" 再哈希一次, 和普通条目共用键空间也不会撞 */
static inline uint64_t mdpsr_plugin_key(const char* name) {
    char buf[MDPSR_NAME_MAX + 8];
    size_t n = 0;
    const char* p = "plugin:";
    while (*p && n < sizeof(buf) - 1) buf[n++] = *p++;
    if (name) { while (*name && n < sizeof(buf) - 1) buf[n++] = *name++; }
    buf[n] = '\0';
    return mdpsr_hash64(buf);
}

/* 从消息体安全地拷一段 POD; 不够长返回 0 */
static inline int mdpsr_read(const uint8_t* body, uint32_t len, void* out, uint32_t n) {
    uint8_t* q = (uint8_t*)out;
    uint32_t i;
    if (!body || !out || len < n) return 0;
    for (i = 0; i < n; ++i) q[i] = body[i];
    return 1;
}

/* 读消息体里偏移 off 处的 NUL 结尾字符串, 越界返回 NULL */
static inline const char* mdpsr_read_str(const uint8_t* body, uint32_t len, uint32_t off) {
    const char* s;
    uint32_t i;
    if (!body || off >= len) return NULL;
    s = (const char*)(body + off);
    for (i = off; i < len; ++i) if (s[i - off] == '\0') return s;
    return NULL;
}

/* 只读 body 前 4 字节的 _cmd 副本 (长度不够就给 fallback)。
 * 真正的判分请用 msg->cmd —— 这个只是给"想自己再看一眼"的人用的。 */
static inline int32_t mdpsr_body_cmd(const uint8_t* body, uint32_t len, int32_t fallback) {
    int32_t c = fallback;
    if (body && len >= 4) {
        c = (int32_t)((uint32_t)body[0] | ((uint32_t)body[1] << 8) |
                      ((uint32_t)body[2] << 16) | ((uint32_t)body[3] << 24));
    }
    return c;
}

/* ---- host 接口的顺手包装 (插件里用这些, 不用碰 self) ---- */
static inline uint64_t mdpsr_hash(const mdpsr_host* h, const char* n) { return h->hash(n); }
static inline void mdpsr_log(const mdpsr_host* h, int lv, const char* m) { h->log(h->self, lv, m); }
static inline uint64_t mdpsr_millis(const mdpsr_host* h) { return h->millis(h->self); }

static inline int mdpsr_acquire(const mdpsr_host* h, uint64_t key, uint32_t gen,
                                void** ptr, uint32_t* out_gen) {
    return h->acquire(h->self, key, gen, ptr, out_gen);
}
static inline int mdpsr_acquire_many(const mdpsr_host* h, const uint64_t* keys,
                                     const uint32_t* gens, uint32_t n,
                                     void** ptrs, uint32_t* out_gens) {
    return h->acquire_many(h->self, keys, gens, n, ptrs, out_gens);
}
static inline void mdpsr_release(const mdpsr_host* h, uint64_t key) { h->release(h->self, key); }
static inline void mdpsr_release_all(const mdpsr_host* h) { h->release_all(h->self); }
static inline int  mdpsr_held_count(const mdpsr_host* h) { return h->held_count(h->self); }
static inline uint32_t mdpsr_gen_of(const mdpsr_host* h, uint64_t key) { return h->gen_of(h->self, key); }
static inline int mdpsr_kind_of(const mdpsr_host* h, uint64_t key) { return h->kind_of(h->self, key); }

static inline void* mdpsr_pool_alloc(const mdpsr_host* h, mdpsr_pool* p, size_t n, size_t a) {
    return h->pool_alloc(h->self, p, n, a);
}
static inline void mdpsr_pool_free(const mdpsr_host* h, mdpsr_pool* p, void* q, size_t n, size_t a) {
    h->pool_free(h->self, p, q, n, a);
}
static inline void* mdpsr_fctx_alloc(const mdpsr_factory_ctx* c, size_t n, size_t a) {
    return mdpsr_pool_alloc(c->host, c->pool, n, a);
}
static inline void mdpsr_fctx_free(const mdpsr_factory_ctx* c, void* p, size_t n, size_t a) {
    mdpsr_pool_free(c->host, c->pool, p, n, a);
}

static inline int mdpsr_emit(const mdpsr_host* h, uint64_t dst, int32_t cmd,
                             const void* body, uint32_t len) {
    return h->emit(h->self, dst, cmd, body, len);
}
static inline int mdpsr_emit_from(const mdpsr_host* h, uint64_t dst, uint64_t src,
                                  int32_t cmd, const void* body, uint32_t len) {
    return h->emit_from(h->self, dst, src, cmd, body, len);
}
static inline int mdpsr_emit_to(const mdpsr_host* h, uint64_t q, uint64_t dst, uint64_t src,
                                int32_t cmd, const void* body, uint32_t len) {
    return h->emit_to(h->self, q, dst, src, cmd, body, len);
}
static inline int mdpsr_send_reply(const mdpsr_host* h, const mdpsr_msg* req, int32_t cmd,
                                   const void* body, uint32_t len) {
    return h->reply(h->self, req, cmd, body, len);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

/* 插件导出宏 */
#ifdef _WIN32
#  ifdef __cplusplus
#    define MDPSR_EXPORT extern "C" __declspec(dllexport)
#  else
#    define MDPSR_EXPORT __declspec(dllexport)
#  endif
#else
#  ifdef __cplusplus
#    define MDPSR_EXPORT extern "C" __attribute__((visibility("default")))
#  else
#    define MDPSR_EXPORT __attribute__((visibility("default")))
#  endif
#endif

/* 每个插件都要导出它, 用宏省掉抄写错误 */
#define MDPSR_DECL_ABI_VERSION() \
    MDPSR_EXPORT uint32_t mdpsr_abi_version(void) { return MDPSR_ABI_VERSION; }

#endif /* MDPSR_ABI_H */
