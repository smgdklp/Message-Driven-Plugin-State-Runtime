# beta —— 演示插件：没有自己的队列 + 跨插件请求/回包

| 条目 | 名字 | 说明 |
| --- | --- | --- |
| Handle | `beta.Handle` | 入口，**故意不声明 Queue** → 落在宿主的默认队列 `Queue_default` 上 |

**一句话**：`cmd=0` 时给 `alpha.Handle` 发一条 `PING` 并把 `src` 写成自己，alpha 回一条 `PONG` 落到 `beta.Handle`。

它演示的是**插件之间只靠消息说话**：beta 完全不需要知道 alpha 的 dll、头文件或函数指针，只知道一个名字 `alpha.Handle`。协议在 [../demo_protocol.h](../demo_protocol.h)。

规则见 [doc/插件规范.md](../../../doc/插件规范.md)。
