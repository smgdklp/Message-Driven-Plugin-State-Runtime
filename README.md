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
  "list":     ["demo", "paced"]         // 可选: 给工厂的附加字符串数组
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
| `list` | 给工厂的附加字符串数组 | 工厂里从 `ctx->list` / `ctx->list_count` 取 |

**模块本身必须导出三个：**

| 导出 | 必需 | 说明 |
| --- | --- | --- |
| `uint32_t mdpsr_abi_version(void)` | ✔ | 写 `MDPSR_DECL_ABI_VERSION()` 宏即可；装载时核对，对不上就拒绝 |
| `int mdpsr_module_init(const mdpsr_factory_ctx*)` | ✔ | dll 装载后调一次 |
| `void mdpsr_module_fini(void)` | | 可选 |

工厂一律从 `ctx->pool`（= 本插件专属池）分配，别用 `new`/`malloc` —— 卸载时整池回收；
`State`/`Object` 的 `_destroy` 里只做析构（`p->~T()`），不要自己 `free`。

**命令行约定：命令号在帧头里，不在 body 里。** `mdpsr_handle_fn` 拿到的是
`msg` + `body`，判命令**只读 `msg->cmd`**（帧头偏移 16）；body 里的参数"长度够才读"。

| `msg->cmd` | 含义 |
| --- | --- |
| `MDPSR_CMD_INIT`（**0**） | **初始化，约等于别的框架里的 Main**。清单里 `Init` 指的那个 handle 会在装载完成后收到它 |
| `1..15` | 框架保留（`REPLY` / `FAIL` / `PING` / `PONG` / `STOP`），礼貌处理，别回 `UNKNOWN_CMD` |
| `MDPSR_CMD_USER_BASE`（**16**）起 | 插件自己的命令 |

没有"主线"的组件（比如纯窗口类）把 `cmd=0` 空着、直接返回 `MDPSR_OK` 就行。

---

## 2. 命名规范

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

## 3. 编译快速说明

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

## 4. 怎么跑

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
