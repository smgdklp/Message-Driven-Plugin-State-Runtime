# alpha —— 演示插件：自转循环 / 队列限速 / 请求-回包

| 条目 | 名字 | 说明 |
| --- | --- | --- |
| State | `alpha.Stats` | 统计账本，生命周期由宿主托管，卸载随池回收 |
| Object | `alpha.Keeper` | 干活的对象 |
| Handle | `alpha.Handle` | 入口，跑在**自己声明的** `Queue_alpha` 上（不在默认队列） |
| Queue | `Queue_alpha` | 队列工厂回答 `pace_ms = 12`，给循环限速 |

**一句话**：`cmd=0` 之后把 `cmd=TICK` 回滚给自己实现自转；跑够 `MAX_TICKS` 自己收工（限速是保险不是许可）；`cmd=PING` 回 `PONG` 给 `msg->src`。

**看日志找**：`[alpha]`、借资源用的 `acquire_many` / `release_all`、卸载时的 `_destroy`。

规则见 [doc/插件规范.md](../../../doc/插件规范.md)，运行时细节见 [doc/架构.md](../../../doc/架构.md)。
