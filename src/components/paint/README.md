# paint —— GUI 演示客户端

**这个文件夹里没有一行 Windows 代码**：没有 `windows.h` / `HWND` / `HDC` / `WndProc`。它只知道三件事：

1. 一个窗口名（`uint64`）
2. 自己的一张画面 State（图像指针 + 缩放 + 偏置 + `content_seq`）
3. 自己的两个 handle（刷新用 / 收窗口事件用）

| 条目 | 名字 | 说明 |
| --- | --- | --- |
| State | `paint.Surface` | 自己的画面资源，图像内存从自己的池里出 |
| Handle | `paint.Handle` | 业务刷新，跑在 `Queue_paint` 上，`pace_ms = 8` 一拍刷一版 |
| Handle | `paint.Events` | 收窗口事件（消息分流） |

**一句话**：每一拍改纯色 + 挪偏置 → `content_seq++` → 窗口那一侧负责把画面贴上去。它是"GUI 客户端该怎么写"的参考实现。

要窗口就 include [../winmsg/winmsg_client.h](../winmsg/winmsg_client.h)，协议细节见 [doc/GUI.md](../../../doc/GUI.md)。
