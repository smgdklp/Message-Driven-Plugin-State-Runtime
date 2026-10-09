# circ_b —— GUI 测试插件 B（图比窗口大 + 缩放动画 + 整窗可拖）

逻辑全在 [../gui_client_common.h](../gui_client_common.h)，参数在 [../gui_test_protocol.h](../gui_test_protocol.h) 的 `kGtcProfiles[1]`。

**它验证的**：

- 图片比窗口大时由**系统**裁掉（画布只覆盖窗口那么大）
- 缩放正确（0.6 ~ 1.5 来回摆）
- "整窗可拖"是**客户端在状态里点名**才开的，不是默认行为

它是**测试夹具，不是框架的一部分** —— 删掉整个 `test/` 目录，宿主和组件照样跑。

跑法：`build\mdpsr.exe --guitest`。见 [doc/GUI.md](../../doc/GUI.md)。
