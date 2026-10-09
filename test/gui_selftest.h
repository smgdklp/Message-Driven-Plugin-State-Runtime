#pragma once
/* ============================================================================
 *  test/gui_selftest.h —— GUI 阶段自测的入口
 *
 *  实现全在 test/gui_selftest.cpp。宿主 main.cpp 只调这一个函数, 所以
 *  "测试代码"整体待在 ./test 里, 宿主里不再堆 GUI 测试逻辑。
 *
 *  返回值 = 失败项数 (0 = 全过)。
 * ==========================================================================*/

namespace mdpsr { class Runtime; }

/*  cycles: 每个 GUI 测试插件各做多少轮"卸载/重装/重载"热插拔
 *  quick : 只做一遍最小校验 (给 --guitest 用, 几秒钟出结果) */
int mdpsr_gui_selftest(mdpsr::Runtime& rt, int cycles, bool quick);
