# gamma —— 演示插件：两个 handle 共用一条队列 / 运行时自己建队列

| 条目 | 名字 | 说明 |
| --- | --- | --- |
| State | `gamma.State` | 状态，卸载时能看到 `_destroy` 被调用 |
| Object | `gamma.Machine` | 干活对象，内存随本插件的池一起销毁 |
| Handle ×2 | `gamma.Handle` / `gamma.Events` | 两个 handle 默认都跑在自己声明的那条队列上 |
| Queue | 运行时用 `host->queue_create` 再建 | 登记在本插件名下，卸载时一起收掉 |

**一句话**：演示一个插件可以声明多个 handle、可以在运行时加队列，以及"内存真的随插件池回收"（不是留在全局池里不管）。

**防御写法**：所有参数都是"长度够才读"，收到 4 字节的短消息也不会变砖。

**看日志找**：`[gamma]`、`_destroy`、selftest 里"队列数回到基线"那一项。

规则见 [doc/插件规范.md](../../../doc/插件规范.md)。
