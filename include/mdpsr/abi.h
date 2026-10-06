#pragma once
/* ============================================================================
 *  mdpsr - Message-Driven Plugin State Runtime
 *  插件二进制接口 (ABI)  version 3
 *
 *  本头文件是宿主 (runtime) 与所有插件 DLL 的唯一契约。
 *  插件编译时必须包含它, 且必须导出:
 *
 *      MDPSR_EXPORT uint32_t mdpsr_abi_version(void);
 *      MDPSR_EXPORT int      mdpsr_module_init(const mdpsr_host* host, uint64_t dll);
 *      MDPSR_EXPORT void     mdpsr_module_fini(uint64_t dll);   // 可选
 *
 *  三条基本约定:
 *
 *  1) 【消息只是字节流】
 *     队列里的一条消息序列化为连续字节:
 *          int32_t  _len      本条消息的总字节长 (= 12 + body)
 *          uint64_t _handle   目标 handle 的哈希值
 *          uint8_t  _buffer[] 附带信息, 长度 = _len - 12
 *     Dispatcher 只做一件事: 按 _handle 查表, 把「上下文 + 后面的字节」原样交给 handle。
 *     字节部分怎么解析, 由各组件自己约束, 宿主不做任何解释。
 *
 *  2) 【State 是公用配置 / 公用数据对象】
 *     State 不只是可变长 cache —— 它是 DLL 对外暴露的「公用配置 / 公用数据对象」。
 *     凡是需要跨组件公用、需要防止别人直接索引自己的类而造成依赖打结的东西,
 *     都必须挂成 State, 由 Map 托管, 而不是让别的插件去摸自己的类实例。
 *     State 载荷长度可变只是它的能力之一。
 *
 *  3) 【Handle 是解耦接口, 必须无状态】
 *     Handle 一般充当接口层; 按约定, 非 cache 的状态一律放 State,
 *     handle 只通过 msgData / ctxData 工作, 不持有任何可变全局状态。
 *
 *  4) 【分流: 一个 Dispatcher 只吃一个 Queue】
 *     Map 里除了 State* / Classptr* / Handle*, 还并列挂着 Queue*。
 *     每个 Queue 与一个 Dispatcher 生命周期绑定, 一个 dispatcher 只从自己那一个
 *     queue 取消息。组件可以在 plugin.json 的 "queue" 段申请自己的队列和名字,
 *     把 handle 绑上去, 于是它的消息就在那条专属线程上执行; 组件卸载时,
 *     队列与线程一起卸载。投喂失败一律回退到默认的 queue_core。
 *     所有组件都能从上下文的 Content::queue 拿到发起本次分发的 Map.Queue*。
 * ==========================================================================*/

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#  include <memory_resource>
#  include <new>
extern "C" {
#endif

#define MDPSR_ABI_VERSION 3u

#define MDPSR_NAME_MAX 64
#define MDPSR_PATH_MAX 512
#define MDPSR_TEXT_MAX 240

/* 消息帧头长度: int32 _len + uint64 _handle */
#define MDPSR_MSG_HEADER 12u

/* --------------------------------------------------------------------------
 *  错误码
 *  11xxx 通用 / 12xxx Map / 13xxx 插件加载
 * ------------------------------------------------------------------------*/
#define MDPSR_OK                    0
#define MDPSR_ERR_UNKNOWN_CMD       11001
#define MDPSR_ERR_BAD_CONFIG        11002
#define MDPSR_ERR_NULLPTR           11003
#define MDPSR_ERR_TYPE_MISMATCH     11004
#define MDPSR_ERR_CREATE_FAILED     11005
#define MDPSR_ERR_RELEASE_FAILED    11006
#define MDPSR_ERR_THREAD_FAILED     11007
#define MDPSR_ERR_TIMEOUT           11008
#define MDPSR_ERR_BAD_STATE         11009
#define MDPSR_ERR_OUT_OF_RANGE      11010
#define MDPSR_ERR_INVALID_HANDLE    11011
#define MDPSR_ERR_NOT_FOUND         11012
#define MDPSR_ERR_ALREADY_EXISTS    11013
#define MDPSR_ERR_BUSY              11014
#define MDPSR_ERR_BAD_MESSAGE       11015

#define MDPSR_ERR_MAP_KEY_MISSING   12001
#define MDPSR_ERR_MAP_INVALID       12002
#define MDPSR_ERR_MAP_POOL          12003

#define MDPSR_ERR_PLUGIN_NOT_FOUND  13001
#define MDPSR_ERR_PLUGIN_LOAD       13002
#define MDPSR_ERR_PLUGIN_ABI        13003
#define MDPSR_ERR_PLUGIN_EXPORT     13004
#define MDPSR_ERR_PLUGIN_BUSY       13005

#define MDPSR_ERR_QUEUE_NOT_FOUND   14001
#define MDPSR_ERR_QUEUE_FULL        14002
#define MDPSR_ERR_QUEUE_SELF        14003   /* 不能在自己的分发线程上销毁自己的队列 */

/* 核心分流队列的固定名称 —— 后门 DLL_init 会把它挂到 Map.Queue* 上 */
#define MDPSR_QUEUE_CORE_NAME "queue_core"

/* --------------------------------------------------------------------------
 *  内存资源
 *  宿主与插件同工具链编译, 直接共享 std::pmr::memory_resource。
 * ------------------------------------------------------------------------*/
#ifdef __cplusplus
typedef std::pmr::memory_resource mdpsr_resource;
#else
typedef void mdpsr_resource;
#endif

/* ==========================================================================
 *  一、基础对象
 * ==========================================================================*/

/* ---- State: DLL 的公用配置 / 公用数据对象 ----
 * 载荷是任意字节 (可变长), 由创建它的 state 工厂自行解释。
 * 需要跨组件公用、或需要防止依赖打结的配置, 一律挂成 State。
 */
typedef struct mdpsr_state mdpsr_state;
typedef void (*mdpsr_state_destroy_fn)(mdpsr_state* st);

struct mdpsr_state {
    uint64_t    name;     /* Hash(state 名称), 也是 Map 的键 */
    uint64_t    dll;      /* 所属 dll 键 */
    uint32_t    kind;     /* 插件自定义类型标签, 便于接手的插件辨认载荷 */
    uint32_t    flags;
    size_t      size;     /* payload 字节数 */
    void*       payload;  /* 公用数据本体, 由 state->res 分配 */
    mdpsr_resource* res;
    mdpsr_state_destroy_fn destroy;   /* 可选: 释放 payload 内部资源 */
};

/* ---- Classptr: 所有 dll 类的索引 ----
 * 注意: 类实例只允许它的宿主 DLL 自己碰; 别的组件要数据请走 State。
 */
typedef struct mdpsr_classptr {
    uint64_t    name;
    uint64_t    dll;
    uint32_t    kind;
    uint32_t    flags;
    void*       instance;   /* dll 类实例指针 */
    void*       destroy;    /* 可选析构 mdpsr_object_destroy_fn */
    void*       itable;     /* 可选接口表 */
} mdpsr_classptr;

/* 全部 Classptr 的快照 (只在 handle_core 调用期间有效) */
typedef struct mdpsr_classptr_table {
    mdpsr_classptr** items;
    size_t           count;
} mdpsr_classptr_table;

/* ---- Handle / Handle_core ---- */
typedef struct mdpsr_handle mdpsr_handle;
typedef int (*mdpsr_handle_fn)(const uint8_t* msg_data, size_t msg_len,
                               const uint8_t* ctx_data, size_t ctx_len);

struct mdpsr_handle {
    uint64_t        id;      /* Hash(handle 名称) */
    uint64_t        dll;
    uint32_t        kind;    /* 0 = Handle, 1 = Handle_core */
    uint32_t        flags;
    mdpsr_handle_fn fn;      /* C 风格无状态入口 */
    char            name[MDPSR_NAME_MAX];
};

typedef struct mdpsr_handle_core {
    mdpsr_handle    base;
    uint32_t        is_core;
    uint32_t        pad;
    void*           user;    /* 内核私有数据 */
} mdpsr_handle_core;

/* ---- DLLptr: 每个 dll 一个专属池 ---- */
typedef struct mdpsr_dllptr {
    uint64_t        dll;
    uint32_t        flags;
    uint32_t        pad;
    mdpsr_resource* pool;
    char            name[MDPSR_PATH_MAX];
} mdpsr_dllptr;

/* ---- Queue: Map 里与 State* / Classptr* / Handle* 并列的第四类条目 ----
 *
 *  每个 Queue 与**一个** Dispatcher 生命周期绑定:
 *      · 一个 dispatcher 只从它绑定的那一个 queue 取消息;
 *      · 组件的 handle 可以绑定到某个 queue, 于是它的消息就在
 *        那个 queue 的分发线程上执行 —— 这就是"分流";
 *      · 组件卸载时, 属于它的 queue + 线程一起卸载。
 *
 *  约定:
 *      · 叫 "queue_core" 的那个是默认分流队列, 由后门 DLL_init 建立;
 *      · 投喂时如果目标队列不存在或投不进去, 一律回退到 queue_core;
 *      · 每个组件都可以从自身上下文拿到 Map.Queue* (Content::queue)。
 */
typedef struct mdpsr_queue {
    uint64_t        name;      /* Hash(队列名), 也是 Map 的键 */
    uint64_t        dll;       /* 所属 dll 键; 0 = 核心 queue_core */
    uint32_t        flags;
    uint32_t        capacity;  /* 初始环形缓冲字节数 */
    void*           impl;      /* 宿主内部 Queue* 的不透明句柄 */
    char            label[MDPSR_NAME_MAX];
} mdpsr_queue;

/* 全部分流队列的快照 (只在 handle_core 调用期间有效) */
typedef struct mdpsr_queue_table {
    mdpsr_queue** items;
    size_t        count;
} mdpsr_queue_table;

/* ==========================================================================
 *  二、分发上下文
 *
 *  一个 handle 可以在清单里绑定多个 State (见 5.3 的 "states")。
 *  states/state_count 指向一张只在本次调用期间有效的槽位表;
 *  state 字段等价于 states[0], 方便只绑一个的插件直接用。
 *
 *  queue 字段 = 本次分发所在的那个分流队列 (即 Map.Queue* 的索引)。
 *  所有组件都能从上下文里拿到它, 往它里面投消息就等于"把活儿留在自己的线程上"。
 * ==========================================================================*/
typedef struct mdpsr_state_slot {
    uint64_t     key;
    mdpsr_state* state;
} mdpsr_state_slot;

typedef struct mdpsr_content {
    mdpsr_state*             state;
    mdpsr_classptr*          classptr;
    struct mdpsr_host*       host;
    mdpsr_state_slot*        states;
    size_t                   state_count;
    mdpsr_queue*             queue;
} mdpsr_content;

typedef struct mdpsr_core_content {
    mdpsr_state*             state;
    mdpsr_classptr*          classptr;
    mdpsr_handle*            handle;
    mdpsr_handle_core*       handle_core;
    mdpsr_classptr_table*    classptr_all;
    mdpsr_dllptr*            dllptr;
    struct mdpsr_host*       host;
    mdpsr_state_slot*        states;
    size_t                   state_count;
    mdpsr_queue*             queue;
    mdpsr_queue_table*       queue_all;
} mdpsr_core_content;

/* ==========================================================================
 *  三、通用控制消息
 *  宿主测试脚本 / 插件之间互发消息的默认载荷。插件可以自行定义别的 POD 载荷。
 * ==========================================================================*/
typedef struct mdpsr_ctrl_msg {
    int32_t  cmd;
    int32_t  flags;
    uint64_t arg;                  /* 常见: HWND / 计数 / 序号 */
    char     text[MDPSR_TEXT_MAX]; /* NUL 结尾文本参数 */
} mdpsr_ctrl_msg;

/* ==========================================================================
 *  四、DLLMgr (内核组件) 的消息载荷
 *
 *  字节流强类型化为:
 *      int32_t  _cmd   0 初始化 / 1 加载 / 2 卸载 / 3 清空
 *      uint32_t _pad
 *      uint64_t _dll   dll 键 (卸载用)
 *      之后跟 NUL 结尾的 UTF-8 路径字符串 (可为空)
 * ==========================================================================*/
typedef struct mdpsr_dllmgr_cmd {
    int32_t  _cmd;
    uint32_t _pad;
    uint64_t _dll;
    /* char _path[];  紧跟其后, NUL 结尾 */
} mdpsr_dllmgr_cmd;

#define MDPSR_DLLMGR_CMD_INIT    0
#define MDPSR_DLLMGR_CMD_LOAD    1
#define MDPSR_DLLMGR_CMD_UNLOAD  2
#define MDPSR_DLLMGR_CMD_CLEAR   3

/* ==========================================================================
 *  五、清单条目与 Map 注册
 * ==========================================================================*/
enum mdpsr_entry_type {
    MDPSR_ENTRY_STATE       = 0,
    MDPSR_ENTRY_OBJECT      = 1,
    MDPSR_ENTRY_HANDLE      = 2,
    MDPSR_ENTRY_HANDLE_CORE = 3,
    MDPSR_ENTRY_QUEUE       = 4     /* Map.Queue*, 与上面三类并列 */
};

/* 一个 handle 最多能绑定多少个 State */
#define MDPSR_MAX_BIND_STATES 4

typedef struct mdpsr_entry {
    uint64_t    key;
    uint64_t    dll;
    int32_t     type;
    int32_t     flags;
    const char* name;         /* 宿主会拷贝 */
    void*       ptr;          /* mdpsr_state* / mdpsr_classptr* / mdpsr_handle* / mdpsr_queue* */
    uint64_t    bind_object;  /* handle 专用: 绑定的 object 键, 0 = 无 */
    uint64_t    bind_queue;   /* handle 专用: 绑定的分流队列键, 0 = queue_core */
    uint32_t    bind_state_count;
    uint32_t    pad;
    uint64_t    bind_states[MDPSR_MAX_BIND_STATES];  /* handle 专用: 绑定的 state 键 */
} mdpsr_entry;

typedef struct mdpsr_entry_info {
    uint64_t key;
    uint64_t dll;
    int32_t  type;
    int32_t  r_count;
    int32_t  w_count;
    int32_t  valid;
    char     name[MDPSR_NAME_MAX];
} mdpsr_entry_info;

/* 清单里的条目 (宿主解析后的快照) */
typedef struct mdpsr_manifest_entry {
    char     name[MDPSR_NAME_MAX];
    char     symbol[MDPSR_NAME_MAX];
    char     type[16];                 /* handle | handle_core */
    uint32_t param_count;              /* param 是数组时元素个数, 字符串时为 0 */
    uint32_t type_id;                  /* mdpsr_entry_type */
    char     param[MDPSR_PATH_MAX];    /* param 是字符串时 */
    char     bind_object[MDPSR_NAME_MAX];
    char     bind_queue[MDPSR_NAME_MAX];   /* handle 专用: 绑定的分流队列名, 空 = queue_core */
    uint32_t bind_state_count;
    uint32_t pad2;
    char     bind_states[MDPSR_MAX_BIND_STATES][MDPSR_NAME_MAX];  /* 绑定的 state 名称 */
} mdpsr_manifest_entry;

/* ==========================================================================
 *  六、工厂上下文
 *  清单里 "param" 允许是字符串, 也允许是字符串数组:
 *      字符串 -> ctx->param 有值, list_count == 0
 *      数组   -> ctx->list/list_count 有值, param == NULL
 *  列表里的相对路径请用 mdpsr_path_join(ctx->manifest_dir, entry) 拼成
 *  「相对运行时根目录」的路径, 宿主的所有路径接口都用这个基准。
 * ==========================================================================*/
typedef struct mdpsr_state_ctx {
    const char*              name;          /* state 名称 (json 的 key) */
    const char*              param;         /* param 是字符串时 */
    const char* const*       list;          /* param 是数组时 */
    size_t                   list_count;
    const char*              manifest;      /* 本清单相对根目录的路径 */
    const char*              manifest_dir;  /* 本清单所在目录 (相对根目录) */
    uint64_t                 dll;
    mdpsr_resource*          pool;          /* Pool_state 全局公共池 */
    const struct mdpsr_host* host;
    void*                    reserved;
} mdpsr_state_ctx;

typedef struct mdpsr_object_ctx {
    const char*              name;
    const char*              param;
    const char* const*       list;
    size_t                   list_count;
    const char*              manifest;
    const char*              manifest_dir;
    uint64_t                 dll;
    mdpsr_resource*          pool;          /* Pool_dll 中该 dll 的专属池 */
    const struct mdpsr_host* host;
    void*                    reserved;
} mdpsr_object_ctx;

typedef mdpsr_state* (*mdpsr_state_fn)(const mdpsr_state_ctx* ctx);
typedef void*        (*mdpsr_object_fn)(const mdpsr_object_ctx* ctx);
typedef void         (*mdpsr_object_destroy_fn)(void* instance);

/* --------------------------------------------------------------------------
 *  六之二、分流队列工厂
 *
 *  组件在 plugin.json 里声明 "queue" 段申请自己的分流队列:
 *
 *      "queue": {
 *          "Capture.Thread": { "symbol": "mdpsr_queue_capture", "capacity": 8192 }
 *      }
 *      或者简写 (键是 symbol, 值是队列名):
 *      "queue": { "mdpsr_queue_capture": "Capture.Thread" }
 *
 *  DLLMgr/宿主 会为该条目建一个 Queue + 一个 Dispatcher + 一条常驻线程。
 *  工厂不是必须的: 条目值直接给字符串(只声明名字)就用默认参数。
 * ------------------------------------------------------------------------*/
typedef struct mdpsr_queue_ctx {
    const char*              name;          /* 队列名 (json 的 key) */
    const char*              param;         /* param 是字符串时 */
    const char* const*       list;          /* param 是数组时 */
    size_t                   list_count;
    const char*              manifest;
    const char*              manifest_dir;
    uint64_t                 dll;
    mdpsr_resource*          pool;          /* Pool_msg */
    const struct mdpsr_host* host;
    void*                    reserved;
} mdpsr_queue_ctx;

typedef struct mdpsr_queue_desc {
    uint32_t struct_size;   /* 填 sizeof(mdpsr_queue_desc) */
    uint32_t flags;         /* 保留, 填 0 */
    uint32_t capacity;      /* 初始环形缓冲字节数, 0 = 用默认值 */
    uint32_t reserved;
} mdpsr_queue_desc;

/* 返回 MDPSR_OK 表示允许建立这个分流队列; 返回别的值宿主就跳过它 */
typedef int (*mdpsr_queue_fn)(const mdpsr_queue_ctx* ctx, mdpsr_queue_desc* out);

/* --------------------------------------------------------------------------
 *  六之三、后门 (backdoor)
 *
 *  宿主加载完 plugin 列表之后, 会按 config.json 里的 "init_cmd" 标签
 *  在所有已加载模块里 GetProcAddress 找同名导出并**直接调用** —— 不经过消息队列。
 *  内核组件用它来点火: 建立 queue_core, 挂上 Map.Queue*, 再投出第一条消息。
 * ------------------------------------------------------------------------*/
typedef int (*mdpsr_backdoor_fn)(const struct mdpsr_host* host);

/* ==========================================================================
 *  七、宿主服务接口
 * ==========================================================================*/
typedef struct mdpsr_host {
    uint32_t    abi_version;
    uint32_t    struct_size;
    void*       host;                 /* 宿主实例, 原样回传 */

    /* --- 基础 --- */
    uint64_t (*hash)(const char* name);
    void     (*log)(void* host, int level, const char* msg);
    const char* (*root_dir)(void* host);          /* 运行时根目录 (exe 同目录) */

    /* --- 池 --- */
    mdpsr_resource* (*pool_state)(void* host);
    mdpsr_resource* (*pool_map)(void* host);
    mdpsr_resource* (*pool_msg)(void* host);

    /* --- Map --- */
    int  (*map_register)(void* host, const mdpsr_entry* e);
    int  (*map_lookup)(void* host, uint64_t key, void** out_ptr, int write);
    int  (*map_release)(void* host, uint64_t key, int write);
    int  (*map_invalidate)(void* host, uint64_t key);
    int  (*map_erase)(void* host, uint64_t key);
    int  (*map_wait_idle)(void* host, uint64_t key, uint32_t timeout_ms);
    int  (*map_dll_idle)(void* host, uint64_t dll, uint32_t timeout_ms);
    int  (*map_snapshot)(void* host, uint64_t dll, mdpsr_entry_info* out,
                         uint32_t cap, uint32_t* out_count);

    /* --- DLL 池 --- */
    int  (*dll_pool_create)(void* host, uint64_t dll, const char* name, mdpsr_dllptr* out);
    int  (*dll_pool_get)(void* host, uint64_t dll, mdpsr_dllptr* out);
    int  (*dll_pool_release)(void* host, uint64_t dll);

    /* --- 消息 ---
     * emit     : 按目标 handle 绑定的分流队列投递; 找不到 / 投不进就回退到 queue_core。
     * queue_emit: 明确投到某个分流队列; q 为 NULL 或投递失败同样回退 queue_core。
     */
    int  (*emit)(void* host, uint64_t handle_key, const void* data, size_t len);
    int  (*queue_emit)(void* host, mdpsr_queue* q, uint64_t handle_key,
                       const void* data, size_t len);

    /* --- 分流队列 / 分发器 (后门级) --- */
    int  (*queue_create)(void* host, const char* name, uint32_t capacity,
                         uint64_t dll, mdpsr_queue** out);
    int  (*queue_destroy)(void* host, mdpsr_queue* q);
    int  (*queue_bind)(void* host, uint64_t handle_key, mdpsr_queue* q);
    mdpsr_queue* (*queue_core)(void* host);
    int  (*queue_list)(void* host, mdpsr_queue** out, uint32_t cap, uint32_t* out_count);
    size_t (*dispatcher_count)(void* host);

    /* --- 清单 --- */
    int  (*manifest_dll)(void* host, uint64_t dll, uint32_t index, char* out, uint32_t cap);
    int  (*manifest_count)(void* host, uint64_t dll, int kind, uint32_t* out_count);
    int  (*manifest_entry)(void* host, uint64_t dll, int kind, uint32_t index,
                           mdpsr_manifest_entry* out);
    const char* (*manifest_path)(void* host, uint64_t dll);

    /* --- 插件装载 (宿主与 DLLMgr 共用同一套实现) ---
     * manifest_rel 是「相对运行时根目录」的 plugin.json 路径。
     * plugin_load  : 解析清单 -> LoadLibrary -> module_init -> 建专属池
     *                -> 调 state/object 工厂并注册 -> 注册 handle 并绑定 state/object
     * plugin_unload: 失效 -> 等计数归零 -> 析构 object/state -> 摘表
     *                -> 整池释放 -> FreeLibrary
     */
    int  (*plugin_load)(void* host, const char* manifest_rel, uint64_t* out_dll);
    int  (*plugin_unload)(void* host, uint64_t dll);
    int  (*plugin_loaded)(void* host, uint64_t dll);              /* 1 = 已加载 */
    int  (*plugin_keys)(void* host, uint64_t dll, uint64_t* out,
                        uint32_t cap, uint32_t* out_count);       /* 该 dll 在 Map 里的键 */
} mdpsr_host;

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ==========================================================================
 *  八、共享工具 (宿主与插件必须完全一致)
 * ==========================================================================*/
#ifdef __cplusplus

/* FNV-1a 变体, 0 映射为 1 */
static inline uint64_t mdpsr_hash64(const char* s) {
    if (!s) return 0;
    uint64_t h = 1469598103934665603ull;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 1099511628211ull;
    }
    return h ? h : 1ull;
}

/* State 构造 / 析构助手: 宿主与所有插件共用同一套布局,
   保证内核能安全释放插件创建的 State。payload 对齐固定 16 字节。 */
#define MDPSR_STATE_ALIGN 16u

static inline mdpsr_state* mdpsr_state_new(mdpsr_resource* res, uint32_t kind,
                                           size_t payload_size) {
    if (!res) return nullptr;
    void* m = res->allocate(sizeof(mdpsr_state), alignof(mdpsr_state));
    if (!m) return nullptr;
    mdpsr_state* st = new (m) mdpsr_state{};
    st->kind = kind;
    st->res  = res;
    st->size = payload_size;
    if (payload_size) {
        st->payload = res->allocate(payload_size, MDPSR_STATE_ALIGN);
    }
    return st;
}

static inline void mdpsr_state_delete(mdpsr_state* st) {
    if (!st) return;
    mdpsr_resource* r = st->res;
    if (!r) return;
    if (st->destroy) {
        st->destroy(st);          /* 先让载荷自己收尾 */
        st->destroy = nullptr;
    }
    if (st->payload) r->deallocate(st->payload, st->size, MDPSR_STATE_ALIGN);
    st->~mdpsr_state();
    r->deallocate(st, sizeof(mdpsr_state), alignof(mdpsr_state));
}

template <typename T>
static inline T* mdpsr_state_as(mdpsr_state* st) {
    return st ? static_cast<T*>(st->payload) : nullptr;
}

/* 相对路径拼接 + 规范化 "." / "..", 结果始终是「相对运行时根目录」的形式。
   例: mdpsr_path_join(out, n, "core", "../capture/plugin.json")
       -> "capture/plugin.json"                                        */
static inline void mdpsr_path_join(char* out, size_t cap,
                                   const char* base, const char* rel) {
    if (!out || cap == 0) return;
    out[0] = '\0';

    auto copy_plain = [&](const char* s) {
        size_t i = 0;
        if (s) { for (; s[i] && i + 1 < cap; ++i) out[i] = s[i]; }
        out[i] = '\0';
    };

    if (!rel || !*rel) { copy_plain(base); return; }

    /* 绝对路径直接返回 */
    if (rel[0] == '/' || rel[0] == '\\' || rel[1] == ':') { copy_plain(rel); return; }

    char tmp[MDPSR_PATH_MAX * 2];
    size_t n = 0;
    auto append = [&](const char* s, size_t len) {
        for (size_t i = 0; i < len && n + 1 < sizeof(tmp); ++i) tmp[n++] = s[i];
    };
    if (base && *base) {
        size_t bl = 0; while (base[bl]) ++bl;
        append(base, bl);
        if (n && tmp[n - 1] != '/') tmp[n++] = '/';
    }
    {
        size_t rl = 0; while (rel[rl]) ++rl;
        append(rel, rl);
    }
    tmp[n] = '\0';

    /* 按 '/' 切段, 消解 . 与 .. */
    const char* seg[128];
    size_t seglen[128];
    size_t cnt = 0;
    size_t i = 0;
    while (i < n && cnt < 128) {
        while (i < n && (tmp[i] == '/' || tmp[i] == '\\')) ++i;
        size_t s = i;
        while (i < n && tmp[i] != '/' && tmp[i] != '\\') ++i;
        size_t l = i - s;
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

/* 从消息字节流安全地取一段 POD; 长度不够返回 false */
template <typename T>
static inline bool mdpsr_read_pod(const uint8_t* data, size_t len, T* out) {
    if (!data || !out || len < sizeof(T)) return false;
    uint8_t* q = reinterpret_cast<uint8_t*>(out);
    for (size_t i = 0; i < sizeof(T); ++i) q[i] = data[i];
    return true;
}

/* 读取 NUL 结尾字符串 (在 [data+offset, data+len) 范围内), 越界返回 nullptr */
static inline const char* mdpsr_read_str(const uint8_t* data, size_t len, size_t offset) {
    if (!data || offset >= len) return nullptr;
    const char* s = reinterpret_cast<const char*>(data + offset);
    for (size_t i = offset; i < len; ++i) {
        if (s[i - offset] == '\0') return s;
    }
    return nullptr;
}

/* 在 ctx->states 里按名称取 state */
static inline mdpsr_state* mdpsr_find_state(const mdpsr_state_slot* slots, size_t count,
                                            const char* name) {
    if (!slots || !name) return nullptr;
    const uint64_t k = mdpsr_hash64(name);
    for (size_t i = 0; i < count; ++i) {
        if (slots[i].key == k) return slots[i].state;
    }
    return nullptr;
}

/* 组一条 DLLMgr 消息: {int32 _cmd, uint32 _pad, uint64 _dll, char _path[]} */
static inline size_t mdpsr_dllmgr_pack(uint8_t* out, size_t cap,
                                       int32_t cmd, uint64_t dll, const char* path) {
    size_t plen = 0;
    if (path) { while (path[plen]) ++plen; }
    if (!out || cap < sizeof(mdpsr_dllmgr_cmd) + plen + 1) return 0;
    mdpsr_dllmgr_cmd h{};
    h._cmd = cmd;
    h._pad = 0;
    h._dll = dll;
    size_t n = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&h);
    for (size_t i = 0; i < sizeof(h); ++i) out[n++] = p[i];
    for (size_t i = 0; i < plen; ++i) out[n++] = static_cast<uint8_t>(path[i]);
    out[n++] = 0;
    return n;
}

#endif /* __cplusplus */

/* 插件必需的导出宏 */
#ifdef _WIN32
#  define MDPSR_EXPORT extern "C" __declspec(dllexport)
#  define MDPSR_IMPORT extern "C" __declspec(dllimport)
#else
#  define MDPSR_EXPORT extern "C" __attribute__((visibility("default")))
#  define MDPSR_IMPORT extern "C"
#endif
