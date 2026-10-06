# DLLMgr —— 内核组件

> 文档索引：[项目说明](项目说明.md) · [插件规范](插件规范.md) · [DLLMgr 内核组件](DLLMgr内核组件.md)

`DLLMgr` 是"加载 / 卸载插件"这件事的实现者，也是唯一一个**内核级**组件。
但它**没有任何特殊路径**——和普通插件一样，全部配置写在
[`scr/core/plugin.json`](../scr/core/plugin.json) 里，由宿主用同一套 `plugin_load`
装进来。宿主里没有一行代码知道 DLLMgr 长什么样。

它唯一"特殊"的地方是**多导出了一个后门函数 `DLL_init`**——宿主按 `config.json`
里的 `init_cmd` 标签直接调用它来点火。见 [第 5 节](#5-后门-dll_init)。

---

## 1. 清单

```jsonc
{
  "dll_path": ["mdpsr_core.dll"],

  "handle": {
    "DLLMgr_handle": {
      "symbol": "mdpsr_handle_dllmgr",
      "type": "handle_core",
      "states": ["DLLMgr_resecoure_Dict", "DLLMgr_link", "DLLMgr_plugin"],
      "object": "DLLMgr"
    }
  },

  "object": {
    "DLLMgr": { "symbol": "mdpsr_object_dllmgr" }
  },

  "state": {
    "DLLMgr_resecoure_Dict": { "symbol": "mdpsr_state_resource_dict" },

    "DLLMgr_link": {
      "symbol": "mdpsr_state_link",
      "param": ["../capture/plugin.json", "../cmd/plugin.json"]
    },

    "DLLMgr_plugin": { "symbol": "mdpsr_state_plugin" }
  }
}
```

注意 `DLLMgr_link` 里写的是 `../capture/plugin.json`——它相对**本清单所在目录**
（`build/core/`），所以最终解析成 `capture/plugin.json`。

> DLLMgr **自己不申请分流队列**（清单里没有 `queue` 段），
> 也不给 handle 写 `"queue"`，所以它整个跑在默认的 `queue_core` 上。

---

## 2. 三个 State

三个 State 的载荷都是**由本 DLL 自己解释的 C++ 对象**，
宿主只负责按 `Pool_state` 分配、按名称注册、卸载时调 `destroy`。

| State | `kind` | 载荷类型 | 作用 |
| --- | --- | --- | --- |
| `DLLMgr_resecoure_Dict` | `'DICR'` | `ResourceDict` | 所有已加载 dll 的资源索引字典 |
| `DLLMgr_link` | `'LINK'` | `LinkList` | 要 link 的插件 `plugin.json` 路径列表 |
| `DLLMgr_plugin` | `'PLUG'` | `PluginPath` | DLLMgr 自己的 `plugin.json` 相对位置 |

### 2.1 `DLLMgr_resecoure_Dict`

```cpp
struct DictEntry {
    uint64_t                   dll;
    std::pmr::vector<uint64_t> keys;   // 这个 dll 在 Map 里的全部键
};

struct ResourceDict {
    std::pmr::map<uint64_t, DictEntry*> table;  // dll 键 -> 资源索引
    mdpsr_resource*                     res;
};
```

* 初始化时是**空字典**（`plugin.json` 里没有 `param`）。
* 每次 `cmd=1` 加载成功后把 `host->plugin_keys(dll, …)` 的结果填进去；
  每次 `cmd=2` 卸载成功后把对应条目删掉。
* 因为 `plugin_keys` 返回的是"这个 dll 在 Map 里的**全部**键"，
  所以字典里**同时包含 State / Object / Handle，以及这个 dll 申请的分流队列**
  （`MDPSR_ENTRY_QUEUE`）。
* 它是"这个进程现在挂着哪些插件"的唯一权威记录，
  `cmd=3 清空区域`就是靠遍历它来生成卸载指令的。

> 名字里的 `resecoure` 是沿用需求原文的拼写（`resource` 的手滑），
> 代码里三处保持一致，改名的话要同时改：清单、handle 里的查询字符串、
> 以及文档。

### 2.2 `DLLMgr_link`

```cpp
struct LinkList {
    std::pmr::vector<std::pmr::string> items;   // 相对运行时根目录的清单路径
};
```

* 工厂从 `ctx->list` 逐条读 `plugin.json` 里 `param` 数组，
  再用 `mdpsr_path_join(ctx->manifest_dir, entry)` 拼成**相对运行时根目录**的形式。
* 想增删要 link 的插件，**只改这里**（见 [第 8 节](#8-怎么加一个新插件)）。

### 2.3 `DLLMgr_plugin`

```cpp
struct PluginPath { std::pmr::string path; };
```

* 工厂直接把 `ctx->manifest` 存进来，也就是 DLLMgr 自己的清单路径
  `core/plugin.json`。
* 用途有两个：
  1. `cmd=0` 时打日志，明确"我在解析自己的清单"；
  2. 遍历 `DLLMgr_link` 时**把自己排除掉**——自己不需要、也不能给自己发加载消息。

---

## 3. Object：`DLLMgr`

```cpp
class DllMgr {
public:
    DllMgr(std::pmr::memory_resource* pool, const mdpsr_host* host, const char* name);
    mdpsr_resource* Pool() const;
    // q = 本次调用所在的分流队列 (来自 Content::queue, 回滚的消息优先留在它上面)
    int cmd(int c, uint64_t dll, const char* path,
            ResourceDict* dict, LinkList* link, PluginPath* self, mdpsr_queue* q);
private:
    int on_init  (ResourceDict*, LinkList*, PluginPath*, mdpsr_queue*);
    int on_load  (const char* path, ResourceDict*, mdpsr_queue*);
    int on_unload(uint64_t dll,     ResourceDict*, mdpsr_queue*);
    int on_clear (ResourceDict*, mdpsr_queue*);

    mdpsr_resource*   _pool;
    const mdpsr_host* _host;
    char              _name[MDPSR_NAME_MAX];
    size_t            _last_drain;   // cmd=3 的停滞保护
};
```

实例在 **DLLMgr 自己的专属池**里 `new` 出来；对象本身不持有数据，
三个 State 每次都从 `Content::states` 传进来——
所以 handle 依旧无状态，数据也依旧归 Map 管。

多出来的 `mdpsr_queue* q` 是**发起本次分发的那条分流队列**：DLLMgr 回滚出来的
加载 / 卸载指令会优先投在它上面（`host->queue_emit`），投不进去由宿主回退
`queue_core`。这样"谁在驱动我，我的后续动作就留在谁的线程上"。

---

## 4. Handle：`DLLMgr_handle`

类型是 `handle_core`，所以拿到的是 `mdpsr_core_content`。

### 4.1 字节流格式

```
偏移 0    int32_t  _cmd    0 初始化 / 1 加载 / 2 卸载 / 3 清空
偏移 4    uint32_t _pad
偏移 8    uint64_t _dll    dll 键 (卸载用)
偏移 16   char     _path[] NUL 结尾的 UTF-8 路径, 可为空
```

对应 `abi.h` 里的：

```c
typedef struct mdpsr_dllmgr_cmd {
    int32_t  _cmd;
    uint32_t _pad;
    uint64_t _dll;
    /* char _path[]; 紧跟其后, NUL 结尾 */
} mdpsr_dllmgr_cmd;
```

打包助手：`mdpsr_dllmgr_pack(out, cap, cmd, dll, path)`。

### 4.2 转发逻辑

```cpp
MDPSR_EXPORT int mdpsr_handle_dllmgr(const uint8_t* msgData, size_t msgLen,
                                     const uint8_t* ctxData, size_t ctxLen) {
    mdpsr_core_content cc{};
    if (!mdpsr_read_pod(ctxData, ctxLen, &cc)) return MDPSR_ERR_NULLPTR;

    mdpsr_dllmgr_cmd head{};
    if (!mdpsr_read_pod(msgData, msgLen, &head)) return MDPSR_ERR_BAD_MESSAGE;
    const char* path = mdpsr_read_str(msgData, msgLen, sizeof(mdpsr_dllmgr_cmd));

    auto* obj  = static_cast<DllMgr*>(cc.classptr->instance);
    auto* dict = mdpsr_state_as<ResourceDict>(
                     mdpsr_find_state(cc.states, cc.state_count, "DLLMgr_resecoure_Dict"));
    auto* link = mdpsr_state_as<LinkList>(
                     mdpsr_find_state(cc.states, cc.state_count, "DLLMgr_link"));
    auto* self = mdpsr_state_as<PluginPath>(
                     mdpsr_find_state(cc.states, cc.state_count, "DLLMgr_plugin"));

    /* cc.queue 就是发起本次分发的 Map.Queue*, 回滚消息优先留在它上面 */
    return obj->cmd(head._cmd, head._dll, path ? path : "", dict, link, self, cc.queue);
}
```

handle 自己**不做任何业务**：解析 → 取三个 State、对象、还有 `cc.queue` → 转发。

---

## 5. 后门 `DLL_init`

### 5.1 它是什么

宿主读完 `config.json` 之后，会按里面的 `"init_cmd"` 标签在**所有已加载模块**里
`GetProcAddress` 找同名导出，然后**直接调用** —— **不经过消息队列**。

当前 [`scr/main/config.json`](../scr/main/config.json) 写的就是：

```jsonc
{
  "plugin":   ["core/plugin.json"],
  "init_cmd": "DLL_init"
}
```

也就是说：**宿主的点火动作 = 调用 `DLL_init`**。
（早先那个十六进制消息列表 `"message"` 已经废弃不用了。）

### 5.2 签名

```cpp
MDPSR_EXPORT int DLL_init(const mdpsr_host* H);
```

对应 `abi.h` 里的后门函数类型：

```c
typedef int (*mdpsr_backdoor_fn)(const struct mdpsr_host* host);
```

宿主会先校验 `H->abi_version`，并确认 `queue_create` / `queue_emit` 这些
后门级接口存在。

### 5.3 它做三件事

```
DLL_init(H)
  ├─ ① 建 queue_core
  │     H->queue_create(H->host, MDPSR_QUEUE_CORE_NAME, 0, 0, &core)
  │     —— 宿主会为它建 Queue + Dispatcher + 一条常驻线程,
  │        名字以 Map.Queue* 挂上去 (queue_create 是幂等的)
  │
  ├─ ② 把内核自己的 handle 绑到 queue_core
  │     H->queue_bind(H->host, mdpsr_hash64("DLLMgr_handle"), core)
  │     —— 别的组件不声明 queue 时, 默认也走这条
  │
  └─ ③ 往 queue_core 投出第一条消息 (cmd=0 初始化)
        H->queue_emit(H->host, core, mdpsr_hash64("DLLMgr_handle"), buf, n)
        —— 从此正常链路跑起来: cmd=0 会回滚加载消息, 把插件一个个装进来
```

关键代码摘录：

```cpp
MDPSR_EXPORT int DLL_init(const mdpsr_host* H) {
    if (!H || !H->abi_version) return MDPSR_ERR_NULLPTR;
    if (H->abi_version != MDPSR_ABI_VERSION) return MDPSR_ERR_PLUGIN_ABI;
    if (!H->queue_create || !H->queue_emit) return MDPSR_ERR_PLUGIN_ABI;

    /* 1. queue_core + 它的分发线程 */
    mdpsr_queue* core = nullptr;
    const int r = H->queue_create(H->host, MDPSR_QUEUE_CORE_NAME, 0, 0, &core);
    if (r != MDPSR_OK || !core) {
        dlog(H, 2, "[DLLMgr] DLL_init: 建立 queue_core 失败(" + std::to_string(r) + ")");
        return r != MDPSR_OK ? r : MDPSR_ERR_CREATE_FAILED;
    }

    /* 2. 内核 handle 绑到 queue_core */
    const uint64_t h = mdpsr_hash64("DLLMgr_handle");
    H->queue_bind(H->host, h, core);

    dlog(H, 0, std::string("[DLLMgr] DLL_init: Map.Queue* '") + core->label +
                  "' 已挂载, 容量 " + std::to_string(core->capacity) + "B");

    /* 3. 投出第一条消息 */
    uint8_t buf[sizeof(mdpsr_dllmgr_cmd) + 1];
    const size_t n = mdpsr_dllmgr_pack(buf, sizeof(buf), MDPSR_DLLMGR_CMD_INIT, 0, "");
    if (!n) return MDPSR_ERR_BAD_MESSAGE;
    const int er = H->queue_emit(H->host, core, h, buf, n);
    dlog(H, er == MDPSR_OK ? 0 : 2,
         "[DLLMgr] DLL_init: 首条 cmd=0 已投进 queue_core (rc=" + std::to_string(er) + ")");
    return er;
}
```

### 5.4 为什么要有"后门"

* **点火必须早于任何消息**：这时候还没有队列、没有线程，发消息没有意义，
  所以只能用"直接调用"这种方式建立第一台发动机。
* **它绕过了清单**：`init_cmd` 不需要在 `plugin.json` 里声明、不占 Map 键，
  也不需要经过 `queue`/`state`/`object`/`handle` 任何一套工厂。
* **它只认符号名**：宿主在所有已加载模块里找，谁先导出就用谁；
  都找不到会打 ERR 日志并返回 `MDPSR_ERR_NOT_FOUND`。
* 库里可以有多个后门，用不同标签区分；`config.json` 一次只指定一个。

> **注意**：如果某个插件在 `DLL_init` 之前就调了 `emit`，
> 宿主会发现 `queue_core` 不存在，于是**惰性建一个**并打一条
> WARN：`queue_core 在 DLL_init 之前被惰性创建`。
> 这是兜底，不是正常路径——正常路径下 `DLL_init` 才是 `queue_core` 的创建者。

---

## 6. 命令表

| `_cmd` | 名称 | 参数 | 行为 |
| --- | --- | --- | --- |
| 0 | 初始化 | — | 解析 `DLLMgr_plugin`；读 `DLLMgr_link`，**把每个要 link 的插件变成一条加载消息回滚进 queue**（跳过自己） |
| 1 | 加载 | `_path` | `host->plugin_load(path, &dll)`；成功后把该 dll 的资源键记进 `DLLMgr_resecoure_Dict` |
| 2 | 卸载 | `_dll` | `host->plugin_unload(dll)`；成功后从 `DLLMgr_resecoure_Dict` 删掉该条目 |
| 3 | 清空区域 | — | 把字典里所有插件都回滚成 `cmd=2`；**若当前仍非空，再回滚一条 `cmd=3`** |

### 6.1 `cmd=0` 初始化

```
DLLMgr_handle(cmd=0)            ← 跑在 queue_core 的常驻线程上
  └─ DllMgr.on_init(dict, link, self, q)
       ├─ 打印 "解析 DLLMgr_plugin = core/plugin.json"
       ├─ _last_drain = SIZE_MAX          (重置停滞保护)
       └─ for item in DLLMgr_link.items:
              if item == DLLMgr_plugin: 跳过     ← 自己不发给自己
              queue_emit(q, DLLMgr_handle, {cmd=1, path=item})
```

这一步**只发消息，不直接加载**。加载消息回到队列里，
被 `queue_core` 的分发线程逐条取出，才真正走到 `cmd=1`。
好处是：整个装载过程被拆成一条条可观测、可插队的消息，
宿主和别的插件都能在中间观察甚至干预。

### 6.2 `cmd=1` 加载

```
DllMgr.on_load(path, dict, q)
  ├─ host->plugin_load(path, &dll)
  │     └─ 清单解析 / LoadLibrary / module_init / 建专属池
  │        / 建组件申请的 queue (Queue + Dispatcher + 常驻线程)
  │        / state 工厂 / object 工厂 / handle 注册与绑定
  ├─ dict[dll] = { dll, keys = host->plugin_keys(dll) }   ← 含该 dll 的队列键
  └─ 日志: "cmd=1 完成: <path> -> dll=<键>, DLLMgr_resecoure_Dict 记录 N 项"
```

### 6.3 `cmd=2` 卸载

```
DllMgr.on_unload(dll, dict, q)
  ├─ host->plugin_unload(dll)
  │     └─ 失效 → 等 r/w 计数归零 → 摘出队列并置停
  │        → 锁外 join 线程 → 析构 object/state/handle
  │        → 释放队列槽位 → 摘表 → 整池释放 → FreeLibrary
  ├─ 从 dict 删掉该条目
  └─ 日志: "cmd=2 完成: dll=<键>, DLLMgr_resecoure_Dict 剩余 N 项"
```

### 6.4 `cmd=3` 清空区域与收敛

```
DllMgr.on_clear(dict, q)
  ├─ 字典为空          → 直接返回 OK        ← 收敛出口
  ├─ 条目数 == _last_drain → 返回 PLUGIN_BUSY, 不再回滚   ← 防死循环
  └─ 否则:
        _last_drain = 当前条目数
        for dll in dict: queue_emit(q, DLLMgr_handle, {cmd=2, dll})
        queue_emit(q, DLLMgr_handle, {cmd=3})   ← "当前非空, 再回滚一条 cmd=3"
```

收敛过程（**异步**：都是由 `queue_core` 的常驻线程在吃）：

```
轮次   DLLMgr_resecoure_Dict     本轮回滚
 1     {capture, cmd}            2 条 cmd=2 + 1 条 cmd=3
                                 ↓ 两条 cmd=2 执行完
 2     { }                       1 条 cmd=3
                                 ↓
 3     { }                       直接返回 OK, 结束
```

宿主的 [`Runtime::request_clear()`](../scr/main/runtime.cpp) 只是往 `queue_core`
投一条 `cmd=3`，然后**轮询 `queue_core` 的可读字节数**：连续 300ms 为空
（或超时）就认为收敛完成，接着才 `stop_dispatchers()` + `unload_all()`。

`_last_drain` 是安全网：万一某个插件因为
"资源被别的线程占着"卸不掉（`plugin_unload` 返回 `MDPSR_ERR_PLUGIN_BUSY`），
字典大小不会变，下一条 `cmd=3` 就会认出"停滞"并停下，
而不是无限回滚把消息队列撑爆。

---

## 7. 完整初始化链路

```
mdpsr.exe
  │
  ├─ 1. 建池: Pool_msg / Pool_map / Pool_state / Pool_dll
  │          + Pool_thread (线程池) / Pool_dispatcher (dispatcher 池), 以及 Map
  │
  ├─ 2. 读 build/config.json
  │        "plugin":   ["core/plugin.json"]
  │        "init_cmd": "DLL_init"
  │
  ├─ 3. plugin_load("core/plugin.json")
  │        LoadLibrary mdpsr_core.dll → mdpsr_module_init
  │        → state: DLLMgr_resecoure_Dict (空字典)
  │                 DLLMgr_link (解析 param 数组 → capture/cmd 两条路径)
  │                 DLLMgr_plugin (="core/plugin.json")
  │        → object: DLLMgr
  │        → handle: DLLMgr_handle (绑 3 个 state + object DLLMgr, 队列默认 queue_core)
  │        (这一步还没有任何分流队列, 也没有分发线程)
  │
  ├─ 4. 调后门 init_cmd = "DLL_init"
  │        → queue_create("queue_core") ⇒ Queue + Dispatcher + 常驻线程
  │        → queue_bind(DLLMgr_handle, queue_core)
  │        → queue_emit(queue_core, DLLMgr_handle, {cmd=0})
  │
  ├─ 5. **主线程不再泵消息**, 只等运行时长; queue_core 的常驻线程自己在跑:
  │        DLLMgr_handle(cmd=0)
  │          └─ 回滚 2 条 cmd=1 加载消息
  │        DLLMgr_handle(cmd=1, "capture/plugin.json")
  │          → 装上截屏插件 (连带建它的分流队列 Capture.Thread + 常驻线程)
  │        DLLMgr_handle(cmd=1, "cmd/plugin.json")
  │          → 装上 cmd 插件 (没声明 queue, 走 queue_core)
  │        …之后就是插件之间的消息了
  │
  └─ 6. 退出前: 往 queue_core 注入 cmd=3 → 等它异步收敛
                → stop_dispatchers() 停掉全部常驻线程 → unload_all()
```

`config.json` 里**只预加载 core、只指定一个后门**——
capture 和 cmd 完全是 DLLMgr 读了自己的 `DLLMgr_link` 之后装进来的。
这条链路本身就是"宿主不关心任何业务"的最好证明。

---

## 8. 怎么加一个新插件

1. 把插件目录放到 `build/` 下（由它自己的 CMake 负责产出 dll + `plugin.json`）。
2. 在 [`scr/core/plugin.json`](../scr/core/plugin.json) 的 `DLLMgr_link.param`
   数组里加一条**相对 `build/core/`** 的路径：

   ```jsonc
   "param": [
     "../capture/plugin.json",
     "../cmd/plugin.json",
     "../myplugin/plugin.json"     // ← 新增
   ]
   ```

3. 重新构建。下次启动时 DLLMgr 的 `cmd=0` 会自动多回滚一条加载消息。

不需要改宿主，不需要改 DLLMgr 的代码。

> 如果新插件只应在特定时机加载（不是启动就上），
> 就别放进 `DLLMgr_link`，改成在需要的时候由别的组件发一条
> `{cmd=1, path="myplugin/plugin.json"}` 给 `DLLMgr_handle`。
>
> 新插件想要自己的分流线程，就在它自己的 `plugin.json` 里写 `queue` 段，
> 并把 handle 的 `"queue"` 指过去——宿主装载它时会顺手把队列和线程建好，
> 卸载它时又会一起回收。

---

## 9. 为什么 DLLMgr 不把自己放进字典

`DLLMgr_resecoure_Dict` 里只登记**别的**插件。
原因是 `cmd=3` 会为字典里每一项回滚 `cmd=2`，而 `cmd=2` 会走到
`FreeLibrary` ——**在自己正在执行的代码里把自己卸载掉**是未定义行为。

所以：

* DLLMgr 自己不在字典里，`cmd=3` 永远不会卸它；
* 进程退出时轮到 [`Runtime::stop_dispatchers()`](../scr/main/runtime.cpp)
  先停掉所有分发线程，再由 [`Runtime::unload_all()`](../scr/main/runtime.cpp)
  兜底把还挂着的模块直接卸掉（此时已经不会再有消息分发了）。

---

## 10. 排错对照

| 日志 / 现象 | 原因 | 处理 |
| --- | --- | --- |
| `在所有已加载模块里都找不到后门函数: DLL_init` | `config.json` 的 `init_cmd` 拼错，或 core 没加载成功 | 用 `dumpbin /exports mdpsr_core.dll` 确认导出名；确认 `plugin` 列表里有 core |
| `建立 queue_core 失败(14001/11005)` | 队列建不起来（重名冲突 / 线程起不来） | 看有没有别的组件也叫 `queue_core`；看系统线程资源 |
| `queue_core 在 DLL_init 之前被惰性创建` | 有组件在点火前就 `emit` 了 | 正常情况下不该出现；检查是不是有插件在 `mdpsr_module_init` 里发消息 |
| `cmd=1 加载失败(13001)` | 清单路径不对，或 `dll_path` 指向的 DLL 不存在 | 检查 `DLLMgr_link` 里的相对路径是不是相对 `build/core/`；`plugin_load` 只收**相对运行时根目录**的路径 |
| `cmd=1 加载失败(13003)` | 插件编译时用的 `abi.h` 与宿主不是一个版本 | 整个项目一起重编（`build.ps1 -Clean`） |
| `cmd=1 加载失败(13004)` | 缺 `mdpsr_module_init`，或清单里 `symbol` 写错 | 用 `dumpbin /exports mdpsr_xxx.dll` 对一下导出名 |
| `state 'X' 注册失败 (键冲突?)` | 条目名和别的条目重名（Map 键 = `hash(名称)`） | 改名；同一清单里 handle / object / 队列名也不能撞 |
| `state 'X' 找不到导出 Y` | 清单 `symbol` 与 DLL 导出名不一致 | 改清单或加 `extern "C" __declspec(dllexport)` |
| `queue 'X' 工厂拒绝建立 (返回 N), 跳过` | queue 工厂返回了非 `MDPSR_OK` | 看那个插件的 queue 工厂为什么拒绝；它的 handle 会回退 `queue_core` |
| `handle 'X' 想绑定的队列 'Y' 不存在, 回退 queue_core` | 清单里 `"queue"` 写的名字和 `queue` 段声明的名字对不上 | 让两处名字逐字一致；或者把 `queue` 段补上 |
| `拒绝在自己的分发线程上销毁队列 'X'` | 某个 handle 在自己的线程上调了 `queue_destroy` 销毁自己那条队列（会自 join 死锁） | 别这么干；把销毁交给卸载流程或别的线程 |
| `卸载中止: 队列在自己的分发线程上被要求销毁 (会自 join)` | 等价情况，发生在 `plugin_unload` 路径上 | 由别的线程触发卸载；或让宿主退出时的 `stop_dispatchers()` 统一收尾 |
| `cmd=3 停滞: 仍有 N 项卸载不掉` | 有插件卸载时计数迟迟不归零 | 查那个插件的线程有没有一直攥着引用（`map_lookup` 没配对 `map_release`） |
| `丢弃消息: 未知或已失效的 handle` | 消息发给了一个还没加载 / 已卸载的 handle | 如果是启动初期，说明消息跑在了加载消息前面——把顺序调好 |
| `Map 键冲突` | 两个不同插件用了同一个条目名 | 条目名要全局唯一，建议加组件前缀（`Capture.` / `Cmd.`） |
