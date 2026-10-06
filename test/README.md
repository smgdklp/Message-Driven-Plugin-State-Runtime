# test —— 测试项目目录

这里**每一个文件夹都是一个独立的测试项目**，各自带一份 `CMakeLists.txt`，
能单独配置、编译出完整的可执行测试。

```
test/
├── run_all.ps1          依次构建并运行所有测试项目
├── queue/               消息队列单元测试
│   ├── CMakeLists.txt
│   └── queue_test.cpp
├── mt/                  多线程读写压力测试
│   ├── CMakeLists.txt
│   └── mt_test.cpp
└── _legacy/img/         旧架构时期留下的截屏证据 (仅存档, 不参与构建)
```

## 跑

```powershell
pwsh -File test\run_all.ps1                 # 全部
pwsh -File test\run_all.ps1 -Test queue     # 只跑 queue
pwsh -File test\run_all.ps1 -SkipBuild      # 复用上次的构建
```

手工跑某一个（默认生成器是 MSBuild；不要用 Ninja，原因见根 README）：

```powershell
cmake -S test\queue -B build\test\queue -G "Visual Studio 18 2026" -A x64
cmake --build build\test\queue --config Release
build\test\queue\mdpsr_test_queue.exe
```

每个测试项目的 CMakeLists 都会 `add_subdirectory(scr/main)`，也就是把
**整套项目（宿主 + 内核 + 各插件）一起构建**，然后链接运行时静态库 `mdpsr_runtime`。
所以测试项目是"完整可执行"的，不依赖外部脚本。

`run_all.ps1` 默认依次跑 `queue` 和 `mt` 两个项目，可用的参数：
`-Test <名字...>` 选项目、`-Config <Release|Debug>`、
`-Generator "<生成器名>"`、`-SkipBuild` 复用上次构建。

## queue —— 消息队列单元测试

验证 `Queue` 这条可拓展环形字节缓冲的帧语义：

- `int32 _len | uint64 _handle | body[]` 先进先出，二进制安全（含 `0x00` / `0xFF`）
- 200KB 载荷触发扩容后内容不错位
- 500 次交错收发制造环形回绕，顺序与内容都正确
- `push_frame` 拒绝长度不符 / `_len` 小于帧头的非法帧，且不污染队列
- 空 body 消息可以收发

## mt —— 多线程读写压力测试

**不加载任何插件、也不读 `config.json`**，只链接 `mdpsr_runtime`，
纯粹打运行时本身。分 6 个阶段：

| 阶段 | 干什么 | 验什么 |
| --- | --- | --- |
| 1 | 起 `queue_core`，再起 4 条自定义队列（`mt.q0` ~ `mt.q3`） | 分流线程数 = 5；`queue_core()` / `queue_list()` 都能取到 |
| 2 | 4 个生产者线程并发往 4 条队列投喂 **4 万条**（`kMsgTotal`），每条带一个递增 `uint64` | 全部被消费；条数一条不多一条不少；**求和**与**异或**都和理论值一致（能查出丢包 / 重复 / 串值）；四条队列的消化量之和 = 总数 |
| 3 | 8 线程 × 2 万次反复 `map.acquire` / `map.release`（读、写各一轮） | 压力结束后所有条目 `r_count == w_count == 0` |
| 4 | 8 线程 × 400 次并发 `mdpsr_state_new`（从 `Pool_state`）+ 注册 + 摘除 + `mdpsr_state_delete` | 全部成功，且临时条目没有泄漏在 Map 里 |
| 5 | 4 线程 × 15 次并发 `queue_create` + 投一条 + `queue_destroy` | 全部成功；收拾后仍剩 5 条分流线程（考验 `Pool_thread` + `Pool_dispatcher`） |
| 6 | `stop_dispatchers()` 后汇总 | 分流线程全清、Map 只剩测试 handle、`dispatch_errors == 0`、**`fallback_total == 0`**（证明分流真的分流了，没有回退 `queue_core`）、入队计数合理 |

它验的核心就一句话：**多线程下读写不出错**——
消息不丢不重不串、引用计数能归零、并发生成/销毁分流队列不会把线程池和
dispatcher 池搞坏。

产物：`build/test/mt/mt.log`（测试里把日志级别调到了 1，只留 WARN 以上，
否则几万条日志会把压力测试本身拖慢）。
