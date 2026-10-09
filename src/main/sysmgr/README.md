# sysmgr —— 内核组件

**它住在 `src/main` 里，和框架一起编译，不进根 `CMakeLists.txt` 的 `PLUGIN_LIST`** —— 它是框架的一部分，不是可替换插件。（产物仍然是一个自包含 dll 目录 `build/plugins/sysmgr/`，因为运行时就是靠 `config.json` 点名加载它的。）

它干的事是**装卸的策略**：谁该被装、装在哪、失败了怎么办。
**机制**（`LoadLibrary` / 摘表 / 回收池 / `FreeLibrary`）在宿主里，由 `host->plugin_install / plugin_uninstall / plugin_reload` 提供。

| 条目 | 名字 | 说明 |
| --- | --- | --- |
| State | `sysmgr.Status` | 公开账本，谁都借得来看（"现在装了些什么"） |
| Object | `sysmgr.Mgr` | 私有干活对象，只由 `sysmgr.Handle` 使用 |
| Handle | `sysmgr.Handle` | 无状态入口，跑在 `Queue_sys` 上 |

**一条必须遵守的纪律**：调用 `plugin_install / uninstall / reload` 的时候，**手上不要持有任何资源**。

**看日志找**：`[sysmgr]`、`一共装了 N / 卸了 N`、`sysmgr.Status._destroy`（说明卸载路径真的走到了）。

详细时序见 [doc/架构.md](../../../doc/架构.md)，规则见 [doc/插件规范.md](../../../doc/插件规范.md)。
