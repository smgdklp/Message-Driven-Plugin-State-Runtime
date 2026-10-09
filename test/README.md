# test —— 测试夹具（**不是框架的一部分**）

这里装的是"用来压框架和 GUI 的东西"。删掉整个 `test/` 目录，宿主和 `src/components/`
里的组件**照样跑** —— 只是 `--guitest` / `--selftest` 的 GUI 阶段没夹具了。

**唯一的例外**：`gui_selftest.cpp` 会被编进 `mdpsr.exe`（宿主只调一个入口
`mdpsr_gui_selftest()`），所以删掉 `test/` 之前要先把 `src/main/CMakeLists.txt` 里
那两处引用也摘掉。

## 里面有什么

| 文件 / 目录 | 是什么 |
| --- | --- |
| `gui_test_protocol.h` | 三个测试插件的**参数表** `kGtcProfiles[]` + **参考光栅器** `gtc_expect()`（插件和自测共用同一份，保证"期望值"和"实际画法"不会各写一遍） |
| `gui_client_common.h` | 三个测试客户端的**公共实现**（`circ_a/b/c` 只负责把自己的导出符号接上去） |
| `gui_selftest.{h,cpp}` | **GUI 阶段的全部断言**（122 条），宿主只调一个入口 |
| `circ_a/` | GUI 测试插件 A：图比窗口小、换色动画、点击关窗后自愈 |
| `circ_b/` | GUI 测试插件 B：图比窗口大（系统裁剪）、缩放动画、整窗可拖 |
| `circ_c/` | GUI 测试插件 C：放大 2 倍 + 偏置跑到出界 |

每个文件夹里都有自己的短 `README.md`。

## 怎么编、怎么跑

三个 `circ_*` 是**普通的独立 CMake 工程**（和 `src/components/` 里的一样），
由根 `CMakeLists.txt` 按 `PLUGIN_LIST` / `Mode` 编出来，产物进
`<BUILD>/plugins/circ_a/` 之类。

能跑通 GUI 自测的是 **`Mode=TXST1`** 那个场景 —— 它就是照着 GUI 夹具的要求推出来的
（`test/` 三个客户端 + `winmsg` broker + `paint` 当第 4 扇窗）：

```powershell
cmake -S . -B build_TXST1 -G "Visual Studio 18 2026" -A x64 -DMode=TXST1 -DBUILD="$PWD\build_TXST1"
cmake --build build_TXST1 --config Release --parallel

.\build_TXST1\mdpsr.exe --guitest                          # 几秒钟, 122 条断言, 成功退出码 0
.\build_TXST1\mdpsr.exe --duration 8000                    # 手动看: 4 扇透明窗口, 各一个纯色圆
Get-Process mdpsr | Select MainWindowTitle                 # "winmsg: paints=N"
```

`build_TXST1\config.json` 是根 CMakeLists 生成的，内容恰好是
`sysmgr / winmsg / paint / circ_a / circ_b / circ_c`。

> `--selftest` 会先跑热插拔压力测试再跑 GUI 阶段，所以它同时要 `alpha`/`beta`/`gamma`
> 和这里的 `circ_*` —— **全部九个插件都在**才跑得完整。只想验 GUI 就用 `--guitest`。
>
> 顺便：`TXST2`（只编 `src/components/`）**跑不了** `--selftest` 的 GUI 阶段，就因为没有
> 这里的 `circ_*`；而且 `alpha`/`beta`/`gamma` 是历史遗留的演示插件，不保证和当前框架适配。
> **能跑通的就是 `TXST1` 这一套。**

## 权威文档

* [../doc/GUI.md](../doc/GUI.md) —— GUI 与 `winmsg` 的唯一权威文档（参数手册 / 事件 / 坑 / 自测逐条说明）
* [../doc/插件规范.md](../doc/插件规范.md) —— 怎么写一个插件
* [../doc/架构.md](../doc/架构.md) —— 锁序 / 生命周期 / 构建与产物
