# Message-Driven Plugin State Runtime (mdpsr)

借助DSH完成的一个魔改状态机程序框架,如果没有ai可能一个学期都写不好(((
POV:DSH完美学习到了我糟糕的表达能力,至少我能看得懂(?).....


一个**消息驱动 + 状态托管 + 多线程分流**的 Windows 插件运行时。

宿主 `mdpsr.exe` 只做四件事：**管池子（内存池 + 线程池 + dispatcher 池）、
管引用计数、把字节流按 handle 分流分发、按清单装载插件**。
所有业务逻辑都是插件——包括"加载/卸载插件"本身（那是内核组件 DLLMgr 干的）。

**每条分流队列都有一条自己的常驻线程在跑 dispatcher**，宿主主线程不泵消息。

## 文档

| 文档 | 内容 |
| --- | --- |
| [整体项目说明](doce/项目说明.md) | 架构、四条基本约定、池/Map/分流分发、初始化流程、组件清单 |
| [DLL 插件规范声明](doce/插件规范.md) | ABI v3 契约：清单 schema、四类工厂、分流队列、后门、上下文、宿主 API、硬性规则 |
| [DLLMgr 内核组件](doce/DLLMgr内核组件.md) | 内核组件的清单、三个 State、命令表、初始化链路、排错 |
| [测试说明](test/README.md) | `/test` 下的独立测试项目 |

## 快速开始

```powershell
# 一键构建 (入口 = scr/main/CMakeLists.txt, 默认生成器 "Visual Studio 18 2026")
pwsh -File build.ps1

# 等价的手工写法
cmake -S scr/main -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release

build\mdpsr.exe --duration 12000     # 跑 12 秒
pwsh -File test\run_all.ps1          # 构建并运行全部测试
```

## 目录

```
/include            头文件 (abi.h = 唯一的二进制契约)
/coment             公共库链接的 cmake (mdpsr_common)
/scr                每个组件一个文件夹, 每个组件都能独立编译自己
    /main           ★ 主程序 + 组织编译成完整项目的 cmake
    /core           内核组件 DLLMgr
    /capture        截屏插件
    /cmd            仿真 cmd 控制台插件
/build              编译产物 (mdpsr.exe / config.json / 各插件包)
/test               每个测试项目一个文件夹, 各自有独立 cmake
/doce               文档
```

## 产物布局

```
build/
├── mdpsr.exe               宿主 (config.json 与它同目录)
├── config.json             初始化配置: plugin 预加载列表 + init_cmd 后门标签
├── opencv_world500.dll
├── core/                   内核组件包
├── capture/                截屏插件包
├── cmd/                    仿真 cmd 插件包
└── test/                   测试产物
```

## 工具链

| 工具 | 本机约定位置 |
| --- | --- |
| CMake ≥ 3.20 | `D:\Cpp\cmake\bin\cmake.exe`（已挂进机器 PATH） |
| MSVC (x64) | Visual Studio Community 2026，装在 `D:\Program Files\vs`（`vswhere` 探测） |
| VS 组件清单 | `C:\Users\ThinkPad\Documents\.vsconfig` |
| 生成器 | 默认 **`Visual Studio 18 2026`（MSBuild）+ `-A x64`** |
| OpenCV | `D:\Cpp\Packages\opencv\build` |

> **为什么不用 Ninja**：中文版 MSVC 的 `/showIncludes` 前缀经过码页转换后与
> `cl.exe` 实际输出对不上，会让 Ninja 的依赖库变成空的（改头文件不重编）。

`build.ps1` 会按上面的顺序自动探测，也可以用 `-CMake <路径>` 指定 cmake、
用 `-Generator "<生成器名>"` 换生成器；`-DMDPSR_OPENCV_ROOT=<路径>` 覆盖 OpenCV 位置。
