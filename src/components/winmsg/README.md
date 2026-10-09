# winmsg —— GUI broker（wingui）

**整个框架里唯一碰 Win32 的组件。** 别的插件只认"窗口名 + 画面 State + 事件消息"。

| 条目 | 名字 | 说明 |
| --- | --- | --- |
| State | `winmsg.Screen` | 目录：窗口名 → 目录项。客户端读写，走 `acquire` |
| Object | `winmsg.Shelf` | HWND 表 + GUI 线程。私有，只有 GUI 线程碰 |
| Handle | `winmsg.Control` | 控制入口，`cmd=0` 启动 GUI 线程 |

**渲染管线**：每窗口一张和客户区一样大的 32bpp 预乘 BGRA 画布（初始全 0 = 全透明）。客户端给的图**先按 `scale` 缩放 → 再按 `img_pos` 偏置 → 写进画布**，窗口外的部分交给系统裁。

**对外协议 / 客户端助手**（纯 C，别的插件直接 include 就行）：

- [winmsg_protocol.h](winmsg_protocol.h) —— 窗口条目、画面描述、事件消息
- [winmsg_client.h](winmsg_client.h) —— 要窗口 / 刷画面 / 收事件

**一句话**：它是"框架只做消息、GUI 全部收口到一个 broker"的落点。客户端插件（比如 [../paint](../paint)）一行 Windows 代码都不用写。

**这是唯一权威的 GUI 文档**：[doc/GUI.md](../../../doc/GUI.md)。
