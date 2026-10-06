/* ============================================================================
 *  mdpsr/test/mt/mt_test.cpp
 *  多线程读写压力测试 (独立可执行, 不加载任何插件)
 *
 *  它直接链接运行时静态库, 只考验"多线程下读写会不会出错":
 *
 *    阶段 1  起 queue_core (等价于后门 DLL_init 干的事)
 *    阶段 2  注册一个测试 handle, 多线程并发投喂到 4 条不同的分流队列,
 *            校验消息一条不多一条不少、值也没串
 *    阶段 3  Map 引用计数压力: 多线程反复 acquire/release, 结束时 r/w 必须全 0
 *    阶段 4  多线程并发在 Pool_state 里造 State / 注册 / 摘除
 *    阶段 5  多线程并发建立 / 销毁分流队列 (考验 Pool_thread + Pool_dispatcher)
 *    阶段 6  汇总: 计数平衡、无分发错误、无回退
 *
 *  它不读 config.json、不 LoadLibrary 任何插件 —— 纯粹打运行时本身。
 * ==========================================================================*/
#include "runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_total = 0;
int g_failed = 0;

void check(bool ok, const std::string& name, const std::string& detail = "") {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %-48s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
    std::fflush(stdout);
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2] = { 0 };
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring s(buf, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : s.substr(0, k);
}

/* ==========================================================================
 *  测试用 handle —— 跑在 dispatcher 线程上, 只做统计
 * ==========================================================================*/
constexpr uint64_t kMsgTotal = 40000;      /* 阶段 2 总消息数 */

std::atomic<uint64_t> g_recv_count{ 0 };
std::atomic<uint64_t> g_recv_sum{ 0 };
std::atomic<uint64_t> g_recv_xor{ 0 };
std::atomic<uint64_t> g_per_queue[4] = {};

int test_handle(const uint8_t* msgData, size_t msgLen,
                const uint8_t* ctxData, size_t ctxLen) {
    mdpsr_content cc{};
    if (!mdpsr_read_pod(ctxData, ctxLen, &cc)) return MDPSR_ERR_NULLPTR;

    uint64_t v = 0;
    if (!mdpsr_read_pod(msgData, msgLen, &v)) return MDPSR_ERR_BAD_MESSAGE;

    g_recv_count.fetch_add(1, std::memory_order_relaxed);
    g_recv_sum.fetch_add(v, std::memory_order_relaxed);
    g_recv_xor.fetch_xor(v, std::memory_order_relaxed);

    /* 顺便记一下是哪条队列在吃 —— 用来证明分流真的分流了 */
    if (cc.queue) {
        for (int i = 0; i < 4; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "mt.q%d", i);
            if (cc.queue->name == mdpsr_hash64(name)) {
                g_per_queue[i].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }
    return MDPSR_OK;
}

mdpsr_handle g_handle{};                   /* 必须活得比 Runtime 久 */

} /* namespace */

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::SetConsoleOutputCP(CP_UTF8);

    std::wstring root = (argc > 1) ? argv[1] : exe_dir();

    std::printf("==========================================================\n");
    std::printf(" mdpsr 多线程读写压力测试 (mt)\n");
    std::printf("==========================================================\n");
    std::wprintf(L"  工作目录: %s\n", root.c_str());

    mdpsr::Runtime rt;
    rt.init(root);
    rt.set_log_file(root + L"\\mt.log");
    rt.log(0, "===== 多线程读写压力测试开始 =====");

    /* 几万条日志会把压力测试拖慢, 这里只留 WARN 以上 */
    rt.set_log_level(1);

    /* ------------------------------------------------------------------
     *  阶段 1: 起 queue_core (后门 DLL_init 平时干的事)
     * ------------------------------------------------------------------ */
    mdpsr_queue* core = nullptr;
    int rc = rt.queue_create(MDPSR_QUEUE_CORE_NAME, 0, 0, true, &core);
    check(rc == MDPSR_OK && core != nullptr, "阶段1 queue_core 建立成功");
    check(rt.queue_core() != nullptr, "阶段1 Map.Queue* 能取到 queue_core");
    check(rt.dispatcher_count() == 1, "阶段1 分流线程数 = 1",
          std::to_string(rt.dispatcher_count()));

    /* 再起 4 条自定义队列, 和 queue_core 一共 5 条分流 */
    mdpsr_queue* qs[4] = {};
    for (int i = 0; i < 4; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "mt.q%d", i);
        const int r = rt.queue_create(name, 2048, 0, false, &qs[i]);
        if (r != MDPSR_OK || !qs[i]) {
            check(false, std::string("阶段1 建立分流队列 ") + name);
            return 1;
        }
    }
    check(rt.dispatcher_count() == 5, "阶段1 分流线程数 = 5",
          std::to_string(rt.dispatcher_count()));

    {
        uint32_t n = 0;
        rt.queue_list(nullptr, 0, &n);
        check(n == 5, "阶段1 queue_list 报告 5 条队列", std::to_string(n));
    }

    /* ------------------------------------------------------------------
     *  阶段 2: 注册测试 handle, 多线程并发投喂
     * ------------------------------------------------------------------ */
    g_handle.id   = mdpsr_hash64("Mt.Handle");
    g_handle.dll  = 0;
    g_handle.kind = 0;
    g_handle.fn   = &test_handle;
    std::snprintf(g_handle.name, sizeof(g_handle.name), "%s", "Mt.Handle");

    {
        mdpsr_entry e{};
        e.key  = g_handle.id;
        e.dll  = 0;
        e.type = MDPSR_ENTRY_HANDLE;
        e.name = "Mt.Handle";
        e.ptr  = &g_handle;
        check(rt.map().reg(e) == MDPSR_OK, "阶段2 测试 handle 注册进 Map");
    }

    constexpr int kProducers = 4;
    const uint64_t per_producer = kMsgTotal / kProducers;
    std::atomic<bool> start{ false };

    {
        std::vector<std::thread> ths;
        for (int t = 0; t < kProducers; ++t) {
            ths.emplace_back([&, t]() {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (uint64_t i = 0; i < per_producer; ++i) {
                    const uint64_t v = static_cast<uint64_t>(t) * per_producer + i + 1;
                    /* 轮着往 4 条队列投, 制造真实分流 */
                    mdpsr_queue* q = qs[(t + static_cast<int>(i)) & 3];
                    rt.queue_emit(q, g_handle.id, &v, sizeof(v));
                }
            });
        }
        const auto t0 = std::chrono::steady_clock::now();
        start.store(true, std::memory_order_release);

        /* 等消费完 */
        bool done = false;
        while (std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now() - t0).count() < 20) {
            if (g_recv_count.load() >= kMsgTotal) { done = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (auto& th : ths) th.join();

        /* 再给一点时间把尾部的吃完 */
        for (int i = 0; i < 40 && g_recv_count.load() < kMsgTotal; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(done || g_recv_count.load() >= kMsgTotal, "阶段2 全部消息都被消费掉",
              std::to_string(g_recv_count.load()) + "/" + std::to_string(kMsgTotal));
    }

    {
        const uint64_t n = g_recv_count.load();
        /* 期望: 收到条数 = 总数, 求和与异或都与理论值一致 (能查出丢包/重复/串值) */
        uint64_t expect_sum = 0, expect_xor = 0;
        for (uint64_t v = 1; v <= kMsgTotal; ++v) {
            expect_sum += v;
            expect_xor ^= v;
        }
        check(n == kMsgTotal, "阶段2 消息条数一条不多一条不少",
              std::to_string(n) + "/" + std::to_string(kMsgTotal));
        check(g_recv_sum.load() == expect_sum, "阶段2 消息内容求和无损",
              std::to_string(g_recv_sum.load()) + " vs " + std::to_string(expect_sum));
        check(g_recv_xor.load() == expect_xor, "阶段2 消息内容异或无损 (无重复/错位)",
              std::to_string(g_recv_xor.load()) + " vs " + std::to_string(expect_xor));

        uint64_t per = 0;
        for (int i = 0; i < 4; ++i) {
            const uint64_t c = g_per_queue[i].load();
            per += c;
            std::printf("         队列 mt.q%d 消化 %llu 条\n", i, (unsigned long long)c);
        }
        check(per == kMsgTotal, "阶段2 四条队列的消化量之和 = 总数",
              std::to_string(per));
    }

    /* ------------------------------------------------------------------
     *  阶段 3: Map 引用计数压力
     * ------------------------------------------------------------------ */
    {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 20000;
        uint64_t keys[4];
        mdpsr_state* sts[4] = {};
        for (int i = 0; i < 4; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "Mt.Ref.%d", i);
            keys[i] = mdpsr_hash64(name);
            sts[i] = mdpsr_state_new(rt.pools().state(), 0x4D545246u, sizeof(uint64_t));
            *static_cast<uint64_t*>(sts[i]->payload) = 0;
            sts[i]->name = keys[i];
            mdpsr_entry e{};
            e.key = keys[i]; e.dll = 0; e.type = MDPSR_ENTRY_STATE;
            e.name = name; e.ptr = sts[i];
            if (rt.map().reg(e) != MDPSR_OK) {
                check(false, "阶段3 准备测试 State 失败");
                return 1;
            }
        }

        std::atomic<bool> go{ false };
        std::vector<std::thread> ths;
        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&, t]() {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < kPerThread; ++i) {
                    const uint64_t k = keys[(t + i) & 3];
                    /* 读 */
                    void* p = nullptr;
                    if (rt.map().acquire(k, false, nullptr) == MDPSR_OK) rt.map().release(k, false);
                    /* 写 */
                    if (rt.map().acquire(k, true, nullptr) == MDPSR_OK) rt.map().release(k, true);
                    (void)p;
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& th : ths) th.join();

        bool all_idle = true;
        uint32_t n = 0;
        std::vector<mdpsr_entry_info> infos(64);
        rt.map().snapshot(0, infos.data(), (uint32_t)infos.size(), &n);
        for (uint32_t i = 0; i < n && i < infos.size(); ++i) {
            if (infos[i].r_count != 0 || infos[i].w_count != 0) {
                all_idle = false;
                std::printf("         残留计数: %s r=%d w=%d\n", infos[i].name,
                            infos[i].r_count, infos[i].w_count);
            }
        }
        check(all_idle, "阶段3 压力后所有条目 r/w 计数归零");

        for (int i = 0; i < 4; ++i) {
            rt.map().erase(keys[i]);
            mdpsr_state_delete(sts[i]);
        }
    }

    /* ------------------------------------------------------------------
     *  阶段 4: 并发造 State / 注册 / 摘除 (打 Pool_state 与 Pool_map)
     * ------------------------------------------------------------------ */
    {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 400;
        std::atomic<int> ok_count{ 0 };
        std::atomic<bool> go{ false };
        std::vector<std::thread> ths;

        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&, t]() {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < kPerThread; ++i) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "Mt.Churn.%d.%d", t, i);
                    const uint64_t key = mdpsr_hash64(name);

                    mdpsr_state* st = mdpsr_state_new(rt.pools().state(), 0x4D544348u, 128);
                    if (!st) continue;
                    st->name = key;
                    *static_cast<uint64_t*>(st->payload) = static_cast<uint64_t>(t) * 1000 + i;

                    mdpsr_entry e{};
                    e.key = key; e.dll = 0; e.type = MDPSR_ENTRY_STATE;
                    e.name = name; e.ptr = st;
                    if (rt.map().reg(e) == MDPSR_OK) {
                        ok_count.fetch_add(1, std::memory_order_relaxed);
                        rt.map().erase(key);
                    }
                    mdpsr_state_delete(st);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& th : ths) th.join();

        check(ok_count.load() == kThreads * kPerThread, "阶段4 并发 State 注册/摘除全部成功",
              std::to_string(ok_count.load()) + "/" + std::to_string(kThreads * kPerThread));

        /* 阶段 4 的临时条目必须都摘干净了 */
        uint32_t n = 0;
        std::vector<mdpsr_entry_info> infos(64);
        rt.map().snapshot(0, infos.data(), (uint32_t)infos.size(), &n);
        bool no_leak = true;
        for (uint32_t i = 0; i < n && i < infos.size(); ++i) {
            if (std::string(infos[i].name).rfind("Mt.Churn.", 0) == 0) no_leak = false;
        }
        check(no_leak, "阶段4 临时条目没有泄漏在 Map 里");
    }

    /* ------------------------------------------------------------------
     *  阶段 5: 并发建立 / 销毁分流队列 (打 Pool_thread + Pool_dispatcher)
     * ------------------------------------------------------------------ */
    {
        constexpr int kThreads = 4;
        constexpr int kPerThread = 15;
        std::atomic<int> made{ 0 };
        std::atomic<bool> go{ false };
        std::vector<std::thread> ths;

        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&, t]() {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < kPerThread; ++i) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "Mt.Q.%d.%d", t, i);
                    mdpsr_queue* q = nullptr;
                    if (rt.queue_create(name, 1024, 0, false, &q) != MDPSR_OK || !q) continue;
                    made.fetch_add(1, std::memory_order_relaxed);
                    /* 投一条进去, 确认它的 dispatcher 真的在跑 */
                    uint64_t v = 1;
                    rt.queue_emit(q, g_handle.id, &v, sizeof(v));
                    rt.queue_destroy(q);
                }
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& th : ths) th.join();

        check(made.load() == kThreads * kPerThread, "阶段5 并发生成/销毁分流队列全部成功",
              std::to_string(made.load()) + "/" + std::to_string(kThreads * kPerThread));

        /* 回到 5 条 */
        check(rt.dispatcher_count() == 5, "阶段5 收拾后仍剩 5 条分流线程",
              std::to_string(rt.dispatcher_count()));
    }

    /* ------------------------------------------------------------------
     *  阶段 6: 收尾汇总
     * ------------------------------------------------------------------ */
    rt.stop_dispatchers();
    check(rt.dispatcher_count() == 0, "阶段6 停掉后分流线程全清",
          std::to_string(rt.dispatcher_count()));

    {
        uint32_t n = 0;
        rt.map().snapshot(0, nullptr, 0, &n);
        check(n == 1, "阶段6 Map 只剩测试 handle", std::to_string(n) + " 项");
    }

    check(rt.dispatch_errors == 0, "阶段6 无分发错误",
          std::to_string(rt.dispatch_errors) + " 条");
    check(rt.fallback_total == 0, "阶段6 没有回退 queue_core",
          std::to_string(rt.fallback_total) + " 次");
    check(rt.emitted_total >= kMsgTotal, "阶段6 入队计数合理",
          std::to_string(rt.emitted_total) + " 条");


    rt.log(0, "===== 多线程读写压力测试结束: " + std::to_string(g_total) +
              " 项校验, 失败 " + std::to_string(g_failed) + " 项 =====");

    std::printf("\n----------------------------------------------------------\n");
    std::printf(" %d 项校验, 失败 %d 项\n", g_total, g_failed);
    std::printf("----------------------------------------------------------\n");
    return g_failed == 0 ? 0 : 1;
}
