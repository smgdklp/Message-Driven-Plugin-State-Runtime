# Message-Driven Plugin State Runtime (mdpsr)

一个**消息驱动 + 状态托管**的 Windows 插件运行时。

宿主 `mdpsr.exe` 只做这几件事：**管池子、管一张大表（MainMap）、把字节流按
handle 分发、按清单装载插件、提供主线程队列**。所有业务逻辑都是插件 ——
包括"装载 / 卸载插件"本身（那是内核组件 `core` 干的，它也只是个普通插件）。

> 本文件描述的是**当前**骨架（ABI v1）。
> 旧的那套（`scr/` + `doce/` + ABI v4 + image/lable 组件）已整体归档到
> [`archive/`](archive/)，只作参考，不参与构建。

---

## 一、四条约定（先记这个）

### 1. 消息是字节流

队列里一条消息序列化为连续字节，帧头 16 字节：

```
偏移 0    uint64_t _handle   目标 handle 的哈希
偏移 8    int32_t  _cmd      命令号
偏移 12   int32_t  _len      本条消息总字节长 (= 16 + body)
偏移 16   uint8_t  body[]    载荷，由收件组件自己解释
```

**约定 body 一律按"命令行"写**：前 4 字节恒为 `_cmd`，
`cmd = MDPSR_CMD_INIT (0)` 就是初始化（约等于别的框架里的 Main），
`>= 1` 才是组件自己的命令。

> 一个容易踩的坑：同一条命令**可能只有 `_cmd`、也可能带更多字段**。所以判分
> 只该读前 4 字节，参数在长度够的时候再读。先要求整条结构体再判断，会让组件
> 在收到短消息时直接变砖。

### 2. 名称一律哈希成 `uint64_t` 当标识

`mdpsr_hash64(名称)` 是全进程唯一的物体标识，也是 MainMap 的键。
组件之间**不需要 include 对方的头文件**，名字对上就能互相找到。

条目名是给人看的字符串，可以带点号（`Ticker.Handle.Tick`）；导出符号里这些
字符会变成 `_`（`mdpsr_handle_Ticker_Handle_Tick`）。两边用的是同一个字符串，
所以不存在"名字和 symbol 对不上"的可能。

### 3. 只有托管资源才用 `Ptr`，而 `Ptr.mtx` 是"正在使用"的唯一判据

```c
typedef struct mdpsr_ptr {
    void*                ptr;     /* State* / Object* / Handle* / Queue* */
    std::atomic<int32_t> valid;   /* 0 一律当作 nullptr */
    /* mtx —— 谁在用谁持有 */
} mdpsr_ptr;
```

**没有读写计数。** 锁本身就表达了"有人在用"：

* 宿主分发一条消息时，对目标 handle（以及它声明的 state/object）取锁 →
  调用 → 返回即释放；
* 卸载时反过来：先置 `valid = false`（挡住新的），再取锁。
  **拿到锁 == 没人在用 == 可以直接删；拿不到 == 有人正在用 == 这一轮先不删**，
  把消息回滚到下轮再来。

多把锁一律**按键升序**取，所以分发路径和卸载路径永远不会互相咬住。

### 4. dll 内的类必须自己做好"找不到资源"的防御

资源随时可能因为卸载而变无效。取到的指针是 `nullptr` 就直接返回错误码，
**绝不解引用** —— 这是 dll 作者的责任，宿主不会替你兜。

---

## 二、命名规范

| 东西 | 规则 | 例子 |
| --- | --- | --- |
| Queue 类对象 | `Queue_<功能名称>` | `Queue_default` / `Queue_pluginmgr` / `Queue_ticker` |
| Handle 导出 | `mdpsr_handle_<条目名>` | `mdpsr_handle_core_Handle_Mgr` |
| State 导出 | `mdpsr_state_<条目名>` | `mdpsr_state_Ticker_Ticks` |
| Object 导出 | `mdpsr_object_<条目名>` (+ `_destroy`) | `mdpsr_object_core_Mgr` |
| Queue 工厂 | `mdpsr_queue_<条目名>` | `mdpsr_queue_Queue_ticker` |

**条目名全局唯一**（MainMap 只有一张键空间，不分种类）。所以同一个插件里
`State`/`Object`/`Handle` 不能都叫 `Mgr` —— 会撞键。core 用的是
`core_Mgr` / `core_Handle_Mgr` / `core_State_ResourceDict`。

---

## 三、三个池 + 一张大表 + 三条队列

| 池 | 装什么 |
| --- | --- |
| `Pool_map` | MainMap 的条目（`mdpsr_ptr`）与名字副本 |
| `Pool_state` | State 的载荷（公用数据对象） |
| `Pool_object` | **统一**的对象池：Object 实例、Handle 描述符、Queue 本体 |
| `Pool_msg` | 队列的环形缓冲、消息字节流 |

`MainMap` 是一张 `uint64_t -> Ptr` 的大表，外加**四个按类型拆出的二级字典**
（object / state / handle / queue）。二级字典的值就是大表里的 `Ptr*`，
所以"查表"是指针查询、不产生拷贝。**类型在查找时就隔离了，混淆在结构上不可能。**

三条队列都由宿主在 `init` 阶段建好：

| 队列 | 谁跑 | 用途 |
| --- | --- | --- |
| `Queue_pluginmgr` | 自己一条线程 | 内核组件专用：装载 / 卸载 |
| `Queue_default` | 自己一条线程 | 没声明 `Queue` 的组件都走这条 |
| `Queue_windowsgui` | **宿主主线程**（由主循环泵） | GUI 组件专用，保证窗口操作在属主线程 |

`Queue` 自带**限速计时器**（`pace_ms`）：两条消息的处理至少隔那么久。
这就是"防止有组件拿队列当 while 用"的地方 —— 插件自己不需要 `sleep`。
主线程队列上的限速**不会 sleep**（那会堵死 Win32 消息泵），而是直接返回让
主循环下一轮再来。

---

## 四、插件清单 `plugin.json`

```jsonc
{
  "name":     "ticker",                    // 插件名
  "dll_path": ["mdpsr_ticker.dll"],        // 相对本清单所在目录

  "State":  ["Ticker.Ticks"],              // -> mdpsr_state_Ticker_Ticks
  "Object": ["Ticker.Keeper"],             // -> mdpsr_object_Ticker_Keeper
  "Handle": ["Ticker.Handle.Tick"],        // -> mdpsr_handle_Ticker_Handle_Tick
  "Queue":  ["Queue_ticker"],              // 可选: 自己额外要的队列
  "Init":   "Ticker.Handle.Tick"           // 可选: 谁的 cmd=0 是初始化入口
}
```

`Queue` **可以没有** —— 省略就用 `Queue_default`。

必需导出：`mdpsr_abi_version` / `mdpsr_module_init`（`mdpsr_module_fini` 可选）。

---

## 五、构建与运行

```powershell
pwsh -File build.ps1                       # 一键构建 (Release)
pwsh -File build.ps1 -Clean                # 先清 build/
build\mdpsr.exe --duration 4000            # 跑 4 秒
build\mdpsr.exe --log run.log              # 同时写日志文件
build\mdpsr.exe --quiet                    # 只打 WARN 以上
```

产物（每个插件一个自包含子目录，可以单独拷走）：

```
build/
├── mdpsr.exe               宿主
├── config.json             初始化配置（必须与 exe 同目录）
├── core/                   mdpsr_core.dll + plugin.json
└── ticker/                 mdpsr_ticker.dll + plugin.json
```

`config.json` 的 `plugin` 列表**必须按依赖顺序排**：宿主依次装载，全部装完后
按同样顺序给每个 handle 投 `cmd=0` 点火；`init_cmd` 指定后门函数（不经过
消息队列，直接 `GetProcAddress` 调用）。

---

## 六、目录

```
src/
├── main/                       宿主
│   ├── include/mdpsr/abi.h     ★ 唯一的二进制契约
│   ├── runtime/                MainMap / Queue / 装载器 / JSON / host api
│   ├── main.cpp                主程序（很薄，真正的东西是静态库）
│   ├── CMakeLists.txt          组织整个项目
│   └── config.json
├── components/
│   ├── core/                   内核组件（core_Mgr：装载/卸载）
│   └── ticker/                 示例插件（把每条机制都走一遍）
└── cmake/                      公共编译设置（mdpsr_common）
```

每个组件的 `CMakeLists.txt` 都能独立编译自己：

```powershell
cmake -S src/components/ticker -B build/ticker -G "Visual Studio 18 2026" -A x64
```

---

## 七、示例插件 `ticker` 演示了什么

它不带任何图形/文件依赖，纯粹用来验证骨架：

| 机制 | 怎么演示的 |
| --- | --- |
| State 托管 | `Ticker.Ticks` 一个纯数值对象，由 MainMap 记生命周期 |
| 统一 Object 池 | `Ticker.Keeper` 干活的对象 |
| 无状态 Handle | 所有跨消息的东西都在 State 里，handle 自身不存任何东西 |
| 消息回滚自转 | `cmd=0` 初始化后把 `cmd=1` 投给自己，循环就转起来 |
| **Queue 限速** | `mdpsr_queue_Queue_ticker` 返回 `pace_ms = 20`，两拍之间自动隔 20ms |
| 自己一条线程 | 在 `Queue` 段声明 `Queue_ticker`，于是 handle 不在默认队列上跑 |
| 跨队列投递 | `cmd=2` 把消息投到 `Queue_default` |
| 循环要能停 | 跑够 50 拍自己收工，不留下停不下来的自转 |

跑一遍看到的应该是（**零 ERR、零 WARN、退出码 0**）：

```
队列已建立: 'Queue_pluginmgr' / 'Queue_default' / 'Queue_windowsgui'
[core] State 'core_State_ResourceDict' 已挂载
[core] Object 'core_Mgr' 已构造
[ticker] State 'Ticker.Ticks' 已挂载
[ticker] Object 'Ticker.Keeper' 已构造
[core] 后门: handle 'core_Handle_Mgr' -> Queue_pluginmgr (rc=0)
[ticker] 第 1 拍 / 第 11 拍 / ... / 跑够 50 拍, 循环收工 (sum=1275)
[shutdown] 3/3 完成. 入队 52 / 分发 52 / 限速挡下 0 / 错误 0
```

---

## 八、工具链

| 工具 | 本机位置 |
| --- | --- |
| CMake ≥ 3.20 | `D:\Cpp\cmake\bin\cmake.exe`（已挂进 PATH） |
| MSVC (x64) | Visual Studio Community 2026，`D:\Program Files\vs`（`vswhere` 探测） |
| 生成器 | **`Visual Studio 18 2026`（MSBuild）+ `-A x64`** |

> **为什么不用 Ninja**：中文版 MSVC 的 `/showIncludes` 前缀经过码页转换后与
> `cl.exe` 实际输出对不上，会让 Ninja 的依赖库变成空的（改头文件不重编）。
