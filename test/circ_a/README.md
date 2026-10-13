# circ_a —— GUI 测试插件 A（图比窗口小）

本文件夹只有"把公共实现接到自己的导出符号上"这一件事，**逻辑全在 [../gui_client_common.h](../gui_client_common.h)**，
参数在 [../gui_test_protocol.h](../gui_test_protocol.h) 的 `kGtcProfiles[0]`。

**验证**：窗口里没被图盖到的地方必须**透明**（透出桌面）、换色动画、点击关窗后自愈。

**测试夹具，不是框架的一部分** —— 删掉整个 `test/`，宿主和组件照样跑；所以它住 `test/` 而不是
`src/components/`。

跑法：`build_TXST1\mdpsr.exe --guitest`（几秒钟，**230 条断言**）。见 [../README.md](../README.md) 与
[../../doc/GUI.md](../../doc/GUI.md)。
