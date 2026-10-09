# circ_c —— GUI 测试插件 C（放大 2 倍 + 偏置跑到出界）

逻辑全在 [../gui_client_common.h](../gui_client_common.h)，参数在 [../gui_test_protocol.h](../gui_test_protocol.h) 的 `kGtcProfiles[2]`。

**它验证的**：把"**先缩放 → 再偏置 → 再写画布 → 窗口外交给系统裁**"这条管线压到极限 ——

- 偏置摆到负值（图的左上角跑到窗口外）时，窗口里剩下的那部分必须正确
- 没被图盖到的画布像素必须是 `alpha = 0`（透明）
- 顺带验证点击关窗 + 自愈

它是**测试夹具，不是框架的一部分** —— 删掉整个 `test/` 目录，宿主和组件照样跑。

跑法：`build\mdpsr.exe --guitest`。见 [doc/GUI.md](../../doc/GUI.md)。
