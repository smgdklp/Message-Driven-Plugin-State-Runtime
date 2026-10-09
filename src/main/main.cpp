/* ============================================================================
 *  mdpsr/main.cpp —— 宿主主程序
 *
 *  初始化顺序:
 *      1. Runtime::init  —— 池 + 注册表 + 两条固定队列 (各自一条线程)
 *      2. boot           —— 读 config.json: 按顺序装插件 (含初始化点火)
 *      3. 主循环         —— (v3 起架空) 主线程只等结束, 不参与分发, 也不泵 Win32
 *         (或者 --selftest: 跑一遍热插拔压力测试 + GUI 阶段, 用退出码告诉你行不行)
 *         (或者 --guitest:  只跑 GUI 阶段, 几秒钟出结果)
 *      4. shutdown       —— 停队列线程 -> 反序卸载 -> 汇总
 *
 *  参数:
 *      --root <dir>      运行时根目录 (默认 exe 所在目录)
 *      --duration <ms>   跑多久 (0 = 一直跑到进程被杀)
 *      --log <file>      同时写日志文件
 *      --quiet           只打 WARN 以上
 *      --debug           连 DBG 级日志一起打
 *      --selftest        跑热插拔自测 + GUI 阶段, 结束退出 (成功 0 / 失败 3)
 *      --guitest         只跑 GUI 阶段 (自测夹具见 test/, 成功 0 / 失败 4)
 *      --cycles <n>      自测的循环次数 (默认 20)
 *      --watchdog <ms>   定期打印"谁握着哪把锁 / 队列在哪一步"
 *
 *  ★ GUI 阶段的测试代码整体在 ./test (test/gui_selftest.cpp): 宿主只调一个入口。
 *    三个纯色圆测试客户端也在 ./test/<插件名>/, 产物落在 build/plugins/<名字>/。
 * ==========================================================================*/
#include "runtime/runtime.h"

#include "demo_protocol.h"

/* GUI 阶段自测的入口 (实现在 ./test/gui_selftest.cpp) */
int mdpsr_gui_selftest(mdpsr::Runtime& rt, int cycles, bool quick);

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace mdpsr;

namespace {

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4] = { 0 };
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring s(buf, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : s.substr(0, k);
}

/* ==========================================================================
 *  自测 —— 一边装卸, 一边有别的线程在往这些插件里打消息
 *
 *  这是整个重构最要紧的一段代码: 热插拔的正确性只能靠"真去插拔"来证明。
 *  v1 从来没有跑过这条路径 (cmd=1/cmd=2 一个调用点都没有)。
 *
 *  GUI 阶段不在这里 —— 它整个搬到了 ./test/gui_selftest.cpp (见那个文件顶部
 *  的清单): 测试夹具和框架分开住, 宿主这边只留一行调用。
 * ==========================================================================*/
int run_selftest(Runtime& rt, int cycles) {
    int fails = 0;
    auto bad  = [&](const std::string& m) { ++fails; rt.log(2, "  [FAIL] " + m); };
    auto good = [&](const std::string& m) { rt.log(0, "  [ ok ] " + m); };

    const uint64_t k_alpha = mdpsr_plugin_key("alpha");
    const uint64_t k_beta  = mdpsr_plugin_key("beta");
    const uint64_t k_gamma = mdpsr_plugin_key("gamma");
    const uint64_t h_sys   = mdpsr_hash64(DEMO_SYSMGR_HANDLE);

    const size_t   base_entries = rt.reg().size();
    uint32_t       base_queues  = 0;
    rt.queue_list(nullptr, 0, &base_queues);

    rt.log(0, "== 热插拔自测开始 (cycles=" + std::to_string(cycles) + ") ==");
    rt.log(0, "   基线: 注册表 " + std::to_string(base_entries) + " 个条目, " +
              std::to_string(base_queues) + " 条队列");

    /* ---- 背景"捣乱线程": 一边插拔一边有人往这些 handle 上打消息 ---- */
    struct Target { uint64_t key; int32_t cmd; uint32_t ms; const char* what; };
    const Target targets[4] = {
        /* alpha 那条队列带 12ms 限速 (约 83 条/秒)。捣乱线程要是比它快太多, 就会
         * 把它的队列灌满 —— 那属于"生产者不认背压", 是测试自己写错了。
         * 所以给 alpha 留 20ms 的节奏, 让它能稳稳跑完自己的自转循环;
         * 另外两条目标是 pace=0 的, 可以往死里打。 */
        { mdpsr_hash64(DEMO_ALPHA_HANDLE),  MDPSR_CMD_PING,   20, "alpha PING" },
        { mdpsr_hash64(DEMO_BETA_HANDLE),   BETA_CMD_BUMP,     3, "beta BUMP" },
        { mdpsr_hash64(DEMO_GAMMA_WORK),    GAMMA_CMD_WORK,    1, "gamma WORK" },
        { mdpsr_hash64(DEMO_GAMMA_REPORT),  GAMMA_CMD_REPORT,  1, "gamma REPORT" },
    };

    std::atomic<bool>     stop{ false };
    std::atomic<uint64_t> sent{ 0 };
    std::atomic<uint64_t> rejected{ 0 };
    std::vector<std::thread> hammers;
    for (int i = 0; i < 4; ++i) {
        hammers.emplace_back([&rt, &stop, &sent, &rejected, t = targets[i]] {
            uint32_t seq = 1;
            while (!stop.load(std::memory_order_relaxed)) {
                const uint32_t v = seq++;
                const int rc = rt.emit(t.key, 0, t.cmd, &v, sizeof(v));
                sent.fetch_add(1, std::memory_order_relaxed);
                /* ★ 生产者要认背压: 队列满就退让久一点, 别把有界队列一直顶死。
                 *   这正是 QUEUE_FULL 这个错误码存在的意义 —— 投递方必须能看见
                 *   "我没投进去", 而不是被静默丢掉。 */
                if (rc == MDPSR_ERR_QUEUE_FULL) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(t.ms * 5));
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(t.ms));
                }
            }
        });
    }
    rt.log(0, "   4 条捣乱线程已起 (插件被卸载的那一瞬间它们的消息会变成死信, 这是对的)");

    /* ---- (1) 宿主直接装卸 gamma ----
     * 每轮两个方向都查: 装上之后条目/队列应该正好多出它那一份, 卸掉之后
     * 应该一个不剩地回到"没有 gamma"的那条基线。这条检查同时验证了
     * "每插件池整池回收"和"队列线程确实停干净了"。 */
    uint32_t dry_queues = 0;      /* 没有 gamma 时的队列数 */
    size_t   dry_entries = 0;     /* 没有 gamma 时的条目数 */
    {
        const int rc = rt.plugin_uninstall(k_gamma, 0);
        if (rc != MDPSR_OK) {
            bad("第一次卸载 gamma 就失败 rc=" + std::to_string(rc));
        } else {
            for (int w = 0; w < 300; ++w) {
                rt.queue_list(nullptr, 0, &dry_queues);
                if (dry_queues == base_queues - 2) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            dry_entries = rt.reg().size();
            good("先卸一次 gamma: 队列 " + std::to_string(base_queues) + " -> " +
                 std::to_string(dry_queues) + " (它占了两条: 清单里的 Queue_gamma + "
                 "cmd=0 里运行时自建的 Queue_gamma_aux), 条目 -> " +
                 std::to_string(dry_entries));

            /* 装回去, 循环从"装着"的状态开始 */
            const int r2 = rt.plugin_install("plugins/gamma/plugin.json", nullptr, nullptr);
            if (r2 != MDPSR_OK) bad("把 gamma 装回去失败 rc=" + std::to_string(r2));
        }

        bool ok = (rc == MDPSR_OK);
        for (int i = 0; i < cycles && ok; ++i) {
            /* 现在 gamma 是装着的, 两个方向都核一遍 */
            uint32_t q = 0; rt.queue_list(nullptr, 0, &q);
            mdpsr_plugin_info pi{};
            rt.plugin_info(k_gamma, &pi);
            const size_t e = rt.reg().size();
            if (q != dry_queues + 2) {
                bad("第 " + std::to_string(i) + " 轮: 装着的 gamma 应该有两条队列, 实际 " +
                    std::to_string(q - dry_queues));
                ok = false; break;
            }
            if (e != dry_entries + pi.entry_count) {
                bad("第 " + std::to_string(i) + " 轮: 条目数对不上 " + std::to_string(e) +
                    " != " + std::to_string(dry_entries) + "+" + std::to_string(pi.entry_count));
                ok = false; break;
            }

            int rc2 = rt.plugin_uninstall(k_gamma, 0);
            if (rc2 != MDPSR_OK) {
                bad("第 " + std::to_string(i) + " 轮卸载 gamma 失败 rc=" + std::to_string(rc2));
                ok = false; break;
            }
            uint32_t q2 = 0;
            for (int w = 0; w < 300; ++w) {
                rt.queue_list(nullptr, 0, &q2);
                if (q2 == dry_queues) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (q2 != dry_queues) {
                bad("第 " + std::to_string(i) + " 轮: 卸载后队列没回到基线 " +
                    std::to_string(q2) + " != " + std::to_string(dry_queues));
                ok = false; break;
            }
            if (rt.reg().size() != dry_entries) {
                bad("第 " + std::to_string(i) + " 轮: 卸载后条目没回到基线 " +
                    std::to_string(rt.reg().size()) + " != " + std::to_string(dry_entries));
                ok = false; break;
            }

            rc2 = rt.plugin_install("plugins/gamma/plugin.json", nullptr, nullptr);
            if (rc2 != MDPSR_OK) {
                bad("第 " + std::to_string(i) + " 轮装载 gamma 失败 rc=" + std::to_string(rc2));
                ok = false; break;
            }
        }
        if (ok && rc == MDPSR_OK) {
            good("宿主路径: gamma 装-卸 " + std::to_string(cycles) +
                 " 轮, 每轮队列/条目都精确回到基线, 池整池回收");
        }
    }

    /* ---- (2) 重载 beta ---- */
    {
        bool ok = true;
        for (int i = 0; i < cycles && ok; ++i) {
            const int rc = rt.plugin_reload(k_beta, nullptr);
            if (rc != MDPSR_OK) { bad("重载 beta 失败 rc=" + std::to_string(rc)); ok = false; }
        }
        if (ok) good("宿主路径: beta 重载 " + std::to_string(cycles) + " 次");
    }

    /* ---- (3) 消息驱动路径: 让 sysmgr 自己去装自己卸 ---- */
    {
        mdpsr_plugin_info before{};
        rt.plugin_info(k_alpha, &before);

        sys_req req{};
        req._cmd  = SYS_CMD_CYCLE;
        req.rounds = cycles;
        req.plugin = k_alpha;
        const int er = rt.emit(h_sys, 0, SYS_CMD_CYCLE, &req, sizeof(req));
        if (er != MDPSR_OK) {
            bad("投 CYCLE 给 sysmgr 失败 rc=" + std::to_string(er));
        } else {
            uint32_t now_count = 0;
            bool done = false;
            for (int w = 0; w < 3000; ++w) {
                mdpsr_plugin_info now{};
                rt.plugin_info(k_alpha, &now);
                now_count = now.install_count;
                if (now.install_count >= before.install_count + (uint32_t)cycles) { done = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (done) {
                good("消息驱动: sysmgr 自己把 alpha 装-卸了 " + std::to_string(cycles) +
                     " 轮 (install_count " + std::to_string(before.install_count) + " -> " +
                     std::to_string(now_count) + ")");
            } else {
                bad("消息驱动: 等 sysmgr 的装-卸循环超时 (install_count " +
                    std::to_string(now_count) + ", 期望 " +
                    std::to_string(before.install_count + (uint32_t)cycles) + ")");
            }
        }
    }

    /* ---- 收工 ---- */
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : hammers) t.join();
    rt.log(0, "   捣乱线程结束: 投出 " + std::to_string(sent.load()) + " 条, 其中被拒 " +
              std::to_string(rejected.load()) + " 条");

    /* ---- (4) GUI 阶段: winmsg broker + paint + 三个测试客户端 (见 test/) ---- */
    fails += mdpsr_gui_selftest(rt, cycles, false);

    /* ---- 校验 ---- */
    const size_t end_entries = rt.reg().size();
    if (end_entries != base_entries) {
        bad("注册表条目没回到基线: " + std::to_string(end_entries) + " != " +
            std::to_string(base_entries) + " (有东西没摘干净)");
    } else {
        good("注册表条目回到基线: " + std::to_string(end_entries));
    }

    uint32_t end_queues = 0;
    rt.queue_list(nullptr, 0, &end_queues);
    if (end_queues != base_queues) {
        bad("队列数没回到基线: " + std::to_string(end_queues) + " != " + std::to_string(base_queues));
    } else {
        good("队列数回到基线: " + std::to_string(end_queues));
    }

    if (rt.t_leaked.load() != 0) {
        bad("有插件漏还锁, 宿主兜底了 " + std::to_string(rt.t_leaked.load()) + " 次");
    } else {
        good("没有任何插件漏还锁");
    }
    if (rt.t_failed.load() != 0) {
        bad("有 " + std::to_string(rt.t_failed.load()) +
            " 条消息的 handle 返回了非 OK (死信不算, 那是卸载期间的正常现象)");
    } else {
        good("没有任何 handle 返回过失败码");
    }
    if (rt.reg().total_order_violations() != 0) {
        bad("有 " + std::to_string(rt.reg().total_order_violations()) +
            " 次违反「按键升序借用」的调用 (有人写错了借资源的顺序)");
    } else {
        good("借资源的顺序全程合法");
    }
    if (rt.t_queue_full.load() != 0) {
        rt.log(1, "  [note] 队列满被拒 " + std::to_string(rt.t_queue_full.load()) +
                  " 次 (有界队列的正常反馈, 生产者退让一下就过去了, 不算错)");
    }
    if (rt.t_dropped.load() != 0) {
        rt.log(0, "  [note] 卸载时队列里被丢掉 " + std::to_string(rt.t_dropped.load()) +
                  " 条 (那个插件本来就要走了, 消息没有意义)");
    }
    if (rt.t_busy_requeue.load() != 0) {
        rt.log(0, "  [note] 因忙被回滚重投 " + std::to_string(rt.t_busy_requeue.load()) +
                  " 条 (拿不到锁不是错, 宿主把它扔回队列下轮再来)");
    }

    const uint64_t check[3] = { k_alpha, k_beta, k_gamma };
    int not_ready = 0;
    for (uint64_t k : check) {
        mdpsr_plugin_info pi{};
        if (rt.plugin_info(k, &pi) != MDPSR_OK || pi.status != MDPSR_PLUGIN_READY) {
            ++not_ready;
            bad(std::string("插件 ") + pi.name + " 最后不在 READY 状态 (status=" +
                std::to_string(pi.status) + ")");
        }
    }
    if (!not_ready) good("alpha / beta / gamma 最后都是 READY");

    rt.log(0, "   借资源 " + std::to_string(rt.reg().total_acquires()) + " 次, 因忙被推迟的独占 " +
              std::to_string(rt.reg().total_busy()) + " 次, 兜底还锁 " +
              std::to_string(rt.reg().total_leaks()) + " 次");

    rt.log(0, std::string("== 自测结束: ") + (fails ? "失败 " + std::to_string(fails) + " 项"
                                                     : "全部通过") + " ==");
    return fails;
}

} /* namespace */

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    std::wstring root;
    std::wstring logfile;
    uint32_t     duration = 0;
    int          log_level = 0;
    bool         selftest = false;
    bool         guitest = false;
    int          cycles = 20;
    uint32_t     wd_ms = 0;
    bool         debug = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == L"--root")           next(root);
        else if (a == L"--log")       next(logfile);
        else if (a == L"--duration")  { std::wstring v; next(v); if (!v.empty()) duration = (uint32_t)_wtoi(v.c_str()); }
        else if (a == L"--cycles")    { std::wstring v; next(v); if (!v.empty()) cycles = _wtoi(v.c_str()); }
        else if (a == L"--quiet")     log_level = 1;
        else if (a == L"--debug")     debug = true;
        else if (a == L"--selftest")  selftest = true;
        else if (a == L"--guitest")   guitest = true;
        else if (a == L"--watchdog")  { std::wstring v; next(v); if (!v.empty()) wd_ms = (uint32_t)_wtoi(v.c_str()); }
        else if (a.rfind(L"--", 0) != 0 && root.empty()) root = a;
    }
    if (root.empty()) root = exe_dir();
    if (cycles <= 0) cycles = 1;

    Runtime rt;
    const int ir = rt.init(root);
    if (ir != MDPSR_OK) {
        std::printf("Runtime::init 失败: %d\n", ir);
        return 1;
    }
    if (!logfile.empty()) rt.set_log_file(logfile);
    rt.set_log_level(log_level);
    rt.set_log_debug(debug);

    rt.log(0, "================ Message-Driven Plugin State Runtime ================");
    rt.log(0, "ABI v" + std::to_string(MDPSR_ABI_VERSION) + " / 帧头 " +
              std::to_string(MDPSR_MSG_HEADER) + " 字节 / 主线程 " +
              std::to_string(rt.main_thread_id()));

    const int br = rt.boot();
    if (br != MDPSR_OK) {
        rt.log(2, "引导失败: " + std::to_string(br) + " (config.json 里声明的插件没装全)");
        rt.shutdown();
        return 2;
    }

    /* ---- 看门狗 (可选): 卡住的时候靠它把"谁握着什么"打出来 ---- */
    std::atomic<bool> wd_stop{ false };
    std::thread       wd_thread;
    if (wd_ms) {
        wd_thread = std::thread([&rt, &wd_stop, wd_ms] {
            while (!wd_stop.load(std::memory_order_relaxed)) {
                for (uint32_t i = 0; i < wd_ms / 100; ++i) {
                    if (wd_stop.load(std::memory_order_relaxed)) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                rt.watchdog_dump();
            }
        });
        rt.log(0, "看门狗已启动: 每 " + std::to_string(wd_ms) + "ms 打一次状态");
    }
    auto stop_watchdog = [&] {
        wd_stop.store(true, std::memory_order_relaxed);
        if (wd_thread.joinable()) wd_thread.join();
    };

    if (selftest) {
        const int fails = run_selftest(rt, cycles);
        stop_watchdog();
        rt.shutdown();
        return fails ? 3 : 0;
    }

    /* ---- 只跑 GUI 阶段 (几秒钟, 给改 winmsg / GUI 客户端时快速迭代用) ---- */
    if (guitest) {
        const int fails = mdpsr_gui_selftest(rt, cycles, true);
        stop_watchdog();
        rt.shutdown();
        return fails ? 4 : 0;
    }

    /* ---- 主循环 ★ 已经"架空"了 ----
     *
     * v3 起宿主主线程不再拥有任何队列、也不再泵 Windows 消息:
     *   · 需要线程亲和的活 (GUI) 由插件自己开线程实现 (见 components/winmsg);
     *   · 宿主自己没有窗口, 所以没有要泵的 Win32 消息;
     *   · 主线程就只负责"等结束" (--duration 到点, 或者一直等到进程被杀)。
     *
     * 这不是偷懒: 少一条"宿主主线程上的队列"就少一条全框架最难测的路径,
     * 而它本来也没人能保证比插件自己开线程更好。 */
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (duration) {
            const uint64_t el = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (el >= duration) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    stop_watchdog();
    rt.shutdown();
    return 0;
}
