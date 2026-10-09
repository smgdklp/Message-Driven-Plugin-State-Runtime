# mdpsr 快速上手

> 这是什么项目 / 架构 / 演示插件 / 已知边界 → **[doc/总览.md](doc/总览.md)**
> 框架内部（锁序、生命周期、为什么不会死锁）→ [doc/架构.md](doc/架构.md)
> 插件怎么写（字段表、四类导出、借还协议、线程模型）→ [doc/插件规范.md](doc/插件规范.md)
> GUI 与 winmsg 的唯一权威文档 → [doc/GUI.md](doc/GUI.md)

---

## 1. 插件快速说明：`plugin.json` 与它对应的导出

最小可编译的清单：

```jsonc
{
  "name":     "hello",                  // 插件名, 全局唯一
  "dll_path": "mdpsr_hello.dll",        // 相对本清单所在目录; 也可以是数组

  "State":    ["hello.Count"],          // 公用数据 -> mdpsr_state_hello_Count
  "Object":   ["hello.Keeper"],         // 私有的类 -> mdpsr_object_hello_Keeper
  "Handle":   ["hello.Handle"],         // 消息入口 -> mdpsr_handle_hello_Handle
  "Queue":    ["Queue_hello"],          // 可选, 省了就落 Queue_default
  "Init":     "hello.Handle",           // 可选: 谁的 cmd=0 是初始化入口
  "list":     ["demo", "paced"],        // 可选: 给工厂的附加字符串数组
  "Static_State": {                     // 可选: 静态配置, 宿主会登记成公用 STATE 条目
    "hello.Cfg": {                      //   初级名称 (宿主新起的条目名, 不能和别处撞)
      "scale": "2"                      //   次级名称 -> 字符串 (必须都是字符串)
    }
  }
}
```

**每一段对应哪个 C++ 导出：**

| 段 | 是什么 | 对应的导出（`<条目名>` 里的 `.` 变 `_`） |
| --- | --- | --- |
| `name` | 插件名，全局唯一，≤ 63 字节 | —— |
| `dll_path` | 相对**本清单所在目录**的 dll；字符串或字符串数组 | —— |
| `State` | **公用数据对象**。跨插件共享的就是它 | `void* mdpsr_state_<条目名>(const mdpsr_factory_ctx*)`<br>可选 `void mdpsr_state_<条目名>_destroy(void*, const mdpsr_factory_ctx*)` |
| `Object` | **私有的类实例**（别人 `acquire` 会拿到 `NO_PERM`） | `void* mdpsr_object_<条目名>(const mdpsr_factory_ctx*)`<br>可选 `void mdpsr_object_<条目名>_destroy(void*, const mdpsr_factory_ctx*)` |
| `Handle` | **无状态消息入口**，至少一个 | `int mdpsr_handle_<条目名>(const mdpsr_msg*, const uint8_t* body, uint32_t body_len, const mdpsr_ctx*)` |
| `Queue` | 声明自己要用（或要建）的**分流队列**；省略 = 用宿主的 `Queue_default` | `int mdpsr_queue_<队列名>(const mdpsr_factory_ctx*, mdpsr_queue_desc*)`<br>只回答 `capacity` / `pace_ms`；找不到这个导出就退回默认参数 |
| `Init` | **谁的 `cmd=0` 是初始化入口**；必须在 `Handle` 里声明过 | —— |
| `kernel` | `true` 才有装卸别的插件的权限（默认 `false`） | —— |
| `list` | 给工厂的附加字符串数组（插件级，所有工厂共用一份） | 工厂里从 `ctx->list` / `ctx->list_count` 取 |
| `Static_State` | **静态配置**：`{"初级名称": {"次级名称": "字符串"}}`。**宿主在装载时**按初级名称登记一个公用 `STATE` 条目（键 = `mdpsr_hash64(初级名称)`），卸载时一起摘掉 | 插件侧没有新导出：运行时 `acquire(初级名称)` 拿 `mdpsr_static_dict`（见下） |

**模块本身必须导出三个：**

| 导出 | 必需 | 说明 |
| --- | --- | --- |
| `uint32_t mdpsr_abi_version(void)` | ✔ | 写 `MDPSR_DECL_ABI_VERSION()` 宏即可；装载时核对，对不上就拒绝 |
| `int mdpsr_module_init(const mdpsr_factory_ctx*)` | ✔ | dll 装载后调一次 |
| `void mdpsr_module_fini(void)` | | 可选 |

工厂一律从 `ctx->pool`（= 本插件专属池）分配，别用 `new`/`malloc` —— 卸载时整池回收；
`State`/`Object` 的 `_destroy` 里只做析构（`p->~T()`），不要自己 `free`。

### 借资源：State / Object 怎么用

**每个条目自带一把内核锁，插件不用自己加** —— `acquire` 拿、`release` 放：

```c
const uint64_t key = mdpsr_hash64("alpha.Stats");
void*    p = NULL;
uint32_t g = 0;
if (host->acquire(host->self, key, 0, &p, &g) == MDPSR_OK) {   /* want_gen 0 = 哪一代都行 */
    ... 用 p ...
    host->release(host->self, key);
}
```

| 规则 | 说明 |
| --- | --- |
| 一次借多个 | **`acquire_many` 推荐**：全有或全无，宿主内部按键升序；用完 `release_all` |
| **按键升序** | 同线程多次 `acquire`，**键必须严格递增**，否则 `MDPSR_ERR_LOCK_ORDER`(11014) —— 防死锁 |
| 有界等待 | **不是无限等**：最多 `MDPSR_LOCK_WAIT_MS(50ms) × MDPSR_LOCK_TRIES(8)`，等不到返回 `MDPSR_ERR_BUSY`(11011)，回滚重来即可 |
| 代际 | 传上次拿到的 `gen` 就能检出"换过代了" → `MDPSR_ERR_STALE`(11015)；传 0 则哪代都行 |
| 兜底归还 | 调用返回时宿主把这次借到的一起还掉；`release` 没借过的键 → `MDPSR_ERR_ENTRY_INVALID`(12002) |

⚠ **想在 State 里托管"指向别人的通用代理指针"，存键不存指针：**

```c
struct MyState { uint64_t target_key; uint32_t target_gen; };   /* 代理 = 键 + 代 */
```

`Entry`（旧树里叫 `Countptr`）是**宿主内部的控制块，不在 ABI 里** —— 插件看不到也拿不到，别指望把它塞进 State。存 `(key, gen)` 天然跨代/跨重载安全，要用时现 `acquire` 解析。

### 池

`ctx->pool` 是个不透明封装指针 `mdpsr_pool*`（每插件一份），可反复复用：

```c
void* p = host->pool_alloc(host->self, ctx->pool, bytes, align);
host->pool_free (host->self, ctx->pool, p, bytes, align);
```

拿别的插件的池 → `MDPSR_ERR_POOL_MISMATCH`(11018)。**卸载时整池销毁**，所以"忘了 free"最坏只在这个插件的一生里占着，不会泄漏到别人头上；工厂里想提前还就用 `mdpsr_fctx_free(ctx, p, n, a)`。

### 静态参数：怎么写

清单**只认固定那几个键**（上面那张表）。**你自己加的字段没人读、也不会注入** —— 想给插件塞静态参数，正规通道就是顶层那个 **`list`**：

```jsonc
{
  "name":     "hello",
  "dll_path": "mdpsr_hello.dll",
  "Handle":   ["hello.Handle"],
  "list":     ["rescourse/a.png", "128", "fast"]   // ← 你的静态参数
}
```

它是**插件级**的字符串数组（不是每个条目各一份），**本插件的所有工厂**拿到的是同一份：

```c
void* mdpsr_state_hello_Count(const mdpsr_factory_ctx* ctx) {
    for (uint32_t i = 0; i < ctx->list_count; ++i) {
        const char* s = ctx->list[i];         /* "rescourse/a.png" / "128" / "fast" */
    }
    /* ctx->manifest_dir 是基准目录, 要读文件就从它拼绝对路径 */
}
```

**想要结构化参数**（不是纯字符串数组）就两步：

1. `list[0]` 放你自己的配置文件路径（相对 `ctx->manifest_dir`）
2. 插件自己读那个 json 并校验 —— 里面想加什么字段都随你

框架只负责把 `list` 原样递过来，解析和校验是插件自己的事。工厂上下文里能拿到的还有：`name`（本条目名）/ `plugin_name` / `manifest_path` / `manifest_dir` / `plugin` / `plugin_gen` / `host` / `pool`。

### 结构化静态配置：`Static_State`

上面那条 `list` 是**插件级扁平字符串数组**。要结构化、而且**能在运行时读写**，用 `Static_State`：

```jsonc
"Static_State": {
  "my.cfg": {                        // 初级名称: 宿主会拿它登记一个新条目
    "greeting": "你好, mdpsr",         // 次级名称 -> 字符串
    "scale":    "2"
  }
}
```

**这是宿主（工厂）自己干的活，插件侧什么都不用加** —— 装载插件时宿主顺便做两件事：

1. 拿**初级名称**登记一个 `STATE` 条目，键 = `mdpsr_hash64(初级名称)`；
2. 条目载荷是一个字典：`mdpsr_hash64(次级名称)` → **UTF-8 字节流**。

条目和本插件的其它条目**一起上线**（同过 `publish` 门闸），卸载时**一起摘掉**，重载后自动回来。

插件侧直接 `acquire` 就行 —— 不需要新工厂、不需要 JSON 解析器：

```c
void*    p = NULL;
uint32_t g = 0;
if (mdpsr_acquire(host, mdpsr_hash64("my.cfg"), 0, &p, &g) == MDPSR_OK) {
    const mdpsr_static_dict* d = (const mdpsr_static_dict*)p;
    const mdpsr_static_item* it = mdpsr_static_find(d, mdpsr_hash64("scale"));
    if (it) {
        it->text;   /* UTF-8 字节流, 恒非空, 不保证 NUL 结尾 */
        it->len;    /* ★ 权威长度 (字节数), 一律按它读 */
    }
    mdpsr_release(host, mdpsr_hash64("my.cfg"));
}
```

| 规则 | 说明 |
| --- | --- |
| 值必须是**字符串** | 写 `2` 而不是 `"2"` → 装载时 `BAD_CONFIG`。宿主不替你猜语义 |
| 初级名称要**没人占** | 它是宿主新登记条目的名字。和本插件已声明的条目撞名 → 装载时 `ALREADY_EXISTS` 报错 |
| 是**公用 State** | kind = `STATE`，所以**别的插件也借得到** —— 它本来就是公用数据 |
| 没写就没有 | 该初级名称的条目根本不存在，`acquire` 失败 |
| 上限 | 一个清单最多 16 组，每组最多 32 项（内核侧常量，不在 ABI 里） |
| 载荷 | `mdpsr_static_dict{ magic, count, items, name }`，先看 `magic` 自证 |

**字节流 → UTF-8 文本**：`text` 存的**就是** UTF-8 编码的字节，按 `len` 原样收下即可：

```c
std::string v(reinterpret_cast<const char*>(it->text), it->len);   // 本身就是 UTF-8
```

三个坑：**别 `strlen`**（不保证 NUL 结尾）、**别把字节数当字符数**（一个汉字 3 字节）、**空值也要看 `len`**（`len == 0`，但 `text` 仍非空）。

**它是注册表里的普通 STATE 条目，所以运行时改得了** —— 借到手直接写，改短/改长自己管好 `len`；借还规则见上面「借资源」那节。

**怎么选**：`list` = 插件级扁平数组，工厂建对象时读一次；`Static_State` = 结构化、挂成公用 State、运行时还能读写。

**消息与命令号见 [§2 消息规范](#2-消息规范)。** 一句话先记住：**命令号在帧头（`msg->cmd`），不在 body 里。**

---

## 2. 消息规范

### 帧头 `mdpsr_msg`（24 字节 = `MDPSR_MSG_HEADER`）

| 偏移 | 字段 | 说明 |
| --- | --- | --- |
| 0 | `uint64_t dst` | 目标 handle 键 |
| 8 | `uint64_t src` | 发送者 handle 键 |
| **16** | `int32_t cmd` | **命令号** |
| 20 | `int32_t len` | 整帧字节数 = `24 + body 长度` |
| 24 | `uint8_t body[]` | 载荷，`len - 24` 字节 |

★ **命令号在帧头，不在 body 里。** 别从 body 前 4 字节猜命令。

### handle 签名

```c
int (*mdpsr_handle_fn)(const mdpsr_msg* msg, const uint8_t* body,
                       uint32_t body_len, const mdpsr_ctx* ctx);
```

`body` / `body_len` 就是帧头后面那一段。body 里的参数一律**长度够才读**（先比 `body_len` 再取值）。

### 保留命令号

| `msg->cmd` | 名字 | body / 含义 |
| --- | --- | --- |
| **0** | `MDPSR_CMD_INIT` | 初始化，约等于别的框架里的 Main。**只发给清单 `Init` 指的那一个 handle**；没有主线的组件直接返回 `MDPSR_OK` 空着即可 |
| 1 | `MDPSR_CMD_REPLY` | `mdpsr_reply` |
| 2 | `MDPSR_CMD_FAIL` | `mdpsr_fail` |
| 3 / 4 | `MDPSR_CMD_PING` / `PONG` | `uint32_t seq` |
| 5 | `MDPSR_CMD_STOP` | 请对方停下自转循环 |
| **≥ 16** | `MDPSR_CMD_USER_BASE` | 插件自己的命令都从这里往后编 |

`1..15` 是框架保留段，收到不认识的**礼貌处理**，别回 `UNKNOWN_CMD`。

### 怎么收发

```c
/* 发: 投给 dst 所在队列, src = 我自己 */
host->emit(host->self, dst_key, MY_CMD, &payload, sizeof(payload));

/* 回: 给 req->src 回一条 */
host->reply(host->self, msg, MY_REPLY, &payload, sizeof(payload));
```

- `emit_from(self, dst, src, cmd, body, len)` —— 显式指定发送者
- `emit_to(self, queue_key, dst, src, cmd, body, len)` —— 显式指定投到哪条队列
- `reply` 的 `src` 为 0 时只记日志、不投

---

## 3. 命名规范

### 队列名：`Queue_<名字>`

框架固定两条：**`Queue_sys`**（内核/管理，装卸都在这条上跑）、**`Queue_default`**（默认队列）。
插件自己的：`Queue_alpha`、`Queue_paint`、`Queue_circ_a`、`Queue_gamma` …

### 其余条目名：`<插件名>.<条目名>`

例：`alpha.Handle`、`alpha.Stats`、`alpha.Keeper`、`sysmgr.Status`、`sysmgr.Mgr`、
`winmsg.Screen`、`winmsg.Shelf`、`winmsg.Control`、`paint.Surface`、`paint.Handle`、
`paint.Events`、`gamma.Ledger`、`gamma.Machine`、`gamma.Handle_Work`、
`gamma.Handle_Report`、`beta.Counter`、`circ_a.Surface`、`circ_a.Tick`、`circ_a.Events`。

`<条目名>` 按类型的习惯取：

| 类型 | 习惯 | 实例 |
| --- | --- | --- |
| `State` | 名词，**它是什么** | `Stats` `Status` `Screen` `Surface` `Ledger` `Counter` |
| `Object` | 名词，**干活的那个东西** | `Keeper` `Mgr` `Shelf` `Machine` |
| `Handle` | 控制/事件性动词或名词 | `Handle` `Control` `Events` `Tick` `Handle_Work` `Handle_Report` |

### 导出符号：`mdpsr_<种类>_<条目名>`

**条目名里不是 `[A-Za-z0-9_]` 的字符统一变成 `_`。**

| 条目名 | 种类 | 导出符号 |
| --- | --- | --- |
| `alpha.Stats` | State | `mdpsr_state_alpha_Stats` |
| `alpha.Keeper` | Object | `mdpsr_object_alpha_Keeper` + `mdpsr_object_alpha_Keeper_destroy` |
| `alpha.Handle` | Handle | `mdpsr_handle_alpha_Handle` |
| `Queue_alpha` | Queue | `mdpsr_queue_Queue_alpha` |

`sysmgr` 是内核组件，它的 `sysmgr.Status` / `sysmgr.Mgr` / `sysmgr.Handle` 规则完全一样，
只是它住在 `src/main/sysmgr/`、和框架一起编（**别拿它当插件模板**）。

⚠ 三条硬规则（**装载时会真的检查，违反就装不上**）：

1. 条目名在**全进程**唯一（两个插件不能都叫 `Config`）；
2. 同一个清单里不能重名；
3. 两个不同的条目名**去掉符号之后不能撞**（`A.B` 和 `A_B` 会推出同一个符号 → 拒绝）。

---

## 4. 编译快速说明

### 主入口：根目录的 `CMakeLists.txt`

**就是它编排一切** —— 加插件、换组合、改产出目录都在这里。

| 开关 | 含义 |
| --- | --- |
| `BUILD` | **产出文件夹**。所有产物都落在它下面；空 = `<根>/build` |
| `PLUGIN_LIST` | **要编译的插件文件夹**（相对根目录的路径，分号分隔）。空则由 `Mode` 决定 |
| `Mode` | `TXST1` = 框架 + `test/` 测试插件 + `winmsg`/`paint` —— **唯一能跑通 `--guitest` 的**<br>`TXST2` = `src/components/` 里的组件（含 wingui/`winmsg`）<br>空 = 只编框架 + `sysmgr` |

`PLUGIN_LIST` 不为空时它说了算；为空才用 `Mode` 推导。

### 怎么添加一个新的编译组合

1. **建插件文件夹**：`src/components/<名字>/`，放三样东西 —— `plugin.json` + 一个 `.cpp`
   + `CMakeLists.txt`（照抄 `src/components/alpha/`，最简）。
2. **让它被编出来**，二选一：
   * **点名**：configure 时 `-DPLUGIN_LIST="src/components/<名字>"`
   * **或加一条 `Mode` 分支**：在根 `CMakeLists.txt` 的
     `if(NOT _plugins) … elseif(_mode STREQUAL "TXST1") … elseif(_mode STREQUAL "TXST2") …
     elseif(_mode STREQUAL "") … else() FATAL_ERROR …` 这条链里，
     **在 `elseif(_mode STREQUAL "")` 之前**插一段
     `elseif(_mode STREQUAL "TXST3")`，在里面 `file(GLOB …)` + `list(APPEND _plugins …)`。
3. **加进基准清单**：把 `plugins/<名字>/plugin.json` 写进 `src/main/config.json`
   —— 它决定**加载顺序**（前面的先装好，后面的在 `cmd=0` 里才看得到前面插件的 State）。
   产物里那份 `<BUILD>/config.json` 是**自动生成**的，不用手改。

### 一条最小可用的完整命令

```powershell
cmake -S . -B build_TXST1 -G "Visual Studio 18 2026" -A x64 -DMode=TXST1 -DBUILD="$PWD\build_TXST1"
cmake --build build_TXST1 --config Release --parallel
```

> ⚠ **`-B` 和 `BUILD` 是两件事**：`-B` 是 CMake 的**二进制树**（放 vcxproj/obj），
> `BUILD` 是**产物文件夹**（放 exe / dll / config.json）。不传 `-DBUILD` 时产物一律落
> `<根>/build` —— 所以用了 `-B build_TXST1` 就记得配上 `-DBUILD="$PWD\build_TXST1"`，
> 否则两次 configure 会往同一个产物目录里写。

### 产物布局

```
<BUILD>/
├── mdpsr.exe
├── config.json                     ★ 生成的: 按本次真正编出来的插件集合
└── plugins/<名字>/{mdpsr_<名字>.dll, plugin.json}
```

每个插件都是**自包含的一个子目录，可以整个拷走**。`<BUILD>/_framework/` 和
`<BUILD>/_plugins/<名字>/` 只是 CMake 的中间树，可以无视。

### 只编某几个插件

`PLUGIN_LIST` 可以直接点菜（它不为空时 `Mode` 就不看了）：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
      -DPLUGIN_LIST="src/components/winmsg;src/components/paint"
```

> ⚠ **构建是纯 CMake，不再用 ps1 编排**（原来那个 `build.ps1` 已经删掉了）。

> ⚠ **两个 `Mode` 的成熟度不一样。**
> **能跑通的是 `TXST1`（框架 + wingui）** —— 它编出来的六个插件正好是 GUI 自测的夹具，
> `--guitest` 122 条断言全过。
> `TXST2` 是"`src/components/` 里的组件 + wingui 的组合场景"，但其中
> `alpha` / `beta` / `gamma` 是**历史遗留的演示插件，不保证和当前框架适配**；
> 而且 `--selftest` 的 GUI 阶段要 `test/` 里的 `circ_*`，TXST2 不含它们，
> 所以 **`--selftest` 在 TXST2 下跑不完整**。要验框架请用 `TXST1`。

---

## 5. 怎么跑

下面用默认产出目录 `build` 举例；传了 `-DBUILD=...` 就换成你自己的目录。

```powershell
build\mdpsr.exe --duration 4000                    # 跑 4 秒 (屏幕上出现 4 扇透明窗口, 各一个纯色圆)
build\mdpsr.exe --guitest                          # 只跑 GUI 阶段 (几秒钟, 122 条断言)
build\mdpsr.exe --selftest --cycles 1000           # 1000 轮装-卸压力测试 + GUI 阶段
build\mdpsr.exe --watchdog 3000 --duration 20000   # 边跑边看"谁握着什么锁"
build\mdpsr.exe --debug --duration 4000            # 连 DBG 级日志一起打
```

全部参数（来自 `src/main/main.cpp`）：

| 参数 | 说明 |
| --- | --- |
| `--root <dir>` | 运行时根目录（默认 exe 所在目录） |
| `--duration <ms>` | 跑多久（0 = 一直跑到进程被杀） |
| `--log <file>` | 同时写日志文件 |
| `--quiet` | 只打 `WARN` 以上 |
| `--debug` | 连 `DBG` 级日志一起打（默认打 `INFO/WARN/ERR`） |
| `--selftest` | 热插拔自测 + GUI 阶段（成功 `0` / 失败 `3`） |
| `--guitest` | 只跑 GUI 阶段（成功 `0` / 失败 `4`） |
| `--cycles <n>` | 自测循环次数（默认 20） |
| `--watchdog <ms>` | 定期打印"谁握着哪把锁 / 队列在哪一步" |

**各自需要哪个 `Mode` 编出来的产物：**

| 命令 | 需要 |
| --- | --- |
| `--guitest` | **`-DMode=TXST1`**（`winmsg` + `paint` + `circ_a/b/c`，正好是 GUI 自测的夹具） |
| `--selftest` | **全部九个插件**（`sysmgr` + 组件 + `test/` 的 `circ_*`）。`TXST2` 不含 `circ_*`，所以跑不完整 |
| `--duration` / `--watchdog` | 任意 `Mode`；装哪些由生成的 `<BUILD>/config.json` 决定 |

GUI 的完整参数手册、事件表、踩过的坑在 [doc/GUI.md](doc/GUI.md)。
