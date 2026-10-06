/* ============================================================================
 *  mdpsr/scr/main/main.cpp —— 宿主主程序
 *
 *  初始化顺序:
 *      1. 完成 Pool_msg / Pool_map / Pool_state / Pool_dll
 *         + Pool_thread (线程池) / Pool_dispatcher (dispatcher 池) 的初始化
 *      2. 读取 exe 同目录的 config.json
 *             "plugin"   : 要预先加载的插件 plugin.json 相对位置列表
 *             "init_cmd" : 后门级别函数的标签, 宿主直接 GetProcAddress 调用它
 *      3. 统一装载 plugin 列表
 *      4. 调用 init_cmd 后门 (内核 DLL_init 会建立 queue_core + 它的分发线程,
 *         再把第一条消息投进队列)
 *      5. **主线程不再泵消息** —— 每条分流队列都有自己的常驻线程在跑 dispatcher,
 *         主线程只等运行时长
 *      6. 向 queue_core 注入 cmd=3 (清空区域) 等它收敛, 停掉所有分发线程, 卸载模块
 *
 *  本文件很薄: 真正的运行时是一个库 (mdpsr_runtime), /test 下的测试项目直接链接它。
 * ==========================================================================*/
#include "runtime.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2] = { 0 };
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring s(buf, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : s.substr(0, k);
}

} /* namespace */

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::SetConsoleOutputCP(CP_UTF8);

    std::wstring root;
    std::wstring logfile;
    uint32_t duration = 12000;
    bool dump = false;
    bool no_clear = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == L"--root")          next(root);
        else if (a == L"--log")      next(logfile);
        else if (a == L"--duration") { std::wstring v; next(v); if (!v.empty()) duration = (uint32_t)_wtoi(v.c_str()); }
        else if (a == L"--dump")     dump = true;
        else if (a == L"--no-clear") no_clear = true;
        else if (a.rfind(L"--", 0) != 0 && root.empty()) root = a;
    }
    if (root.empty()) root = exe_dir();

    mdpsr::Runtime rt;
    rt.init(root);
    if (!logfile.empty()) rt.set_log_file(logfile);

    rt.log(0, "================ Message-Driven Plugin State Runtime ================");
    rt.log(0, "运行时根目录: " + rt.root_utf8());
    rt.log(0, "ABI 版本: " + std::to_string(MDPSR_ABI_VERSION));

    /* ---- 1~4. 池 + 清单装载 + 后门点火 ---- */
    uint32_t np = 0;
    std::string init_cmd;
    int backdoor_rc = 0;
    const int r = rt.boot(&np, &init_cmd, &backdoor_rc);
    if (r != MDPSR_OK) {
        rt.log(2, "引导失败, 退出码 " + std::to_string(r));
        return r;
    }
    rt.log(0, "分流线程数: " + std::to_string(rt.dispatcher_count()));

    /* ---- 5. 主线程只等时长; 消息由各分流线程自己吃 ---- */
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const uint64_t el = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (el >= duration) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    /* ---- 6. 收尾 ---- */
    if (!no_clear) rt.request_clear(15000);
    rt.stop_dispatchers();
    rt.unload_all();

    if (dump) {
        std::vector<mdpsr_entry_info> infos(256);
        uint32_t n = 0;
        rt.map().snapshot(0, infos.data(), (uint32_t)infos.size(), &n);
        rt.log(0, "----- Map 快照 (条目 " + std::to_string(n) + ") -----");
        for (uint32_t i = 0; i < n && i < infos.size(); ++i) {
            char line[256];
            std::snprintf(line, sizeof(line), "  key=%016llX type=%d r=%d w=%d valid=%d name=%s",
                          (unsigned long long)infos[i].key, infos[i].type,
                          infos[i].r_count, infos[i].w_count, infos[i].valid, infos[i].name);
            rt.log(3, line);
        }
    }

    rt.log(0, "运行结束. 入队 " + std::to_string(rt.emitted_total) +
              " 条 / 分发 " + std::to_string(rt.dispatched_total) +
              " 条 / 回退 queue_core " + std::to_string(rt.fallback_total) +
              " 次 / 错误 " + std::to_string(rt.dispatch_errors) + " 条");
    return 0;
}
