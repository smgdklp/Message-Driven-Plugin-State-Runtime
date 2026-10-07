/* ============================================================================
 *  mdpsr/main.cpp —— 宿主主程序
 *
 *  初始化顺序:
 *      1. Runtime::init  —— 五个池 + MainMap + 三条固定队列
 *         (Queue_windowsgui 挂在主线程上, 不起自己的线程)
 *      2. boot           —— 读 config.json: 按顺序装载插件, 调后门, 投 cmd=0
 *      3. 主循环         —— 泵 Win32 消息 + pump_main + 睡 1ms
 *      4. shutdown       —— 停队列线程 -> 反序卸载插件 -> 汇总
 *
 *  本文件很薄: 真正的运行时是一个静态库 (mdpsr_runtime), /test 下的测试
 *  项目直接链接它。
 * ==========================================================================*/
#include "runtime/runtime.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4] = { 0 };
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    std::wstring s(buf, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : s.substr(0, k);
}

} /* namespace */

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    std::wstring root;
    std::wstring logfile;
    uint32_t duration = 0;              /* 0 = 一直跑到用户关窗/按 Ctrl+C */
    int log_level = 0;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == L"--root")          next(root);
        else if (a == L"--log")      next(logfile);
        else if (a == L"--duration") { std::wstring v; next(v); if (!v.empty()) duration = (uint32_t)_wtoi(v.c_str()); }
        else if (a == L"--quiet")    log_level = 1;
        else if (a.rfind(L"--", 0) != 0 && root.empty()) root = a;
    }
    if (root.empty()) root = exe_dir();

    mdpsr::Runtime rt;
    const int ir = rt.init(root);
    if (ir != MDPSR_OK) {      /* MDPSR_OK 是宏, 不能加命名空间限定 */
        std::printf("Runtime::init 失败: %d\n", ir);
        return ir;
    }
    if (!logfile.empty()) rt.set_log_file(logfile);
    rt.set_log_level(log_level);

    rt.log(0, "================ Message-Driven Plugin State Runtime ================");
    rt.log(0, "运行时根目录: " + rt.root_utf8());
    rt.log(0, "ABI 版本: " + std::to_string(MDPSR_ABI_VERSION));

    const int br = rt.boot();
    if (br != MDPSR_OK) {
        rt.log(2, "引导失败: " + std::to_string(br));
        rt.shutdown();
        return br;
    }

    /* ---- 主循环 ----
     * 主线不是空转的: Queue_windowsgui 的 dispatcher 就挂在主线程上,
     * 所以必须一边泵 Win32 消息、一边驱动它。 */
    auto pump_once = [&]() {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        rt.pump_main(0);
    };

    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        pump_once();
        if (duration) {
            const uint64_t el = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (el >= duration) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    rt.shutdown();
    return 0;
}
