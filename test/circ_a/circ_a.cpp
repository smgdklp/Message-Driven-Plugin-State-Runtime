/* ============================================================================
 *  circ_a.cpp —— GUI 测试插件 A (图比窗口小)
 *
 *  本文件只有"把公共实现接到自己的导出符号上"这一件事, 逻辑全在
 *  test/gui_client_common.h。参数在 test/gui_test_protocol.h 的 kGtcProfiles[0]。
 *
 *  它验证的: 窗口里没图的地方必须透明(透出桌面)、换色动画、点击关窗后自愈。
 * ==========================================================================*/
#include "gui_client_common.h"

#define GTC_PROFILE_INDEX 0

MDPSR_EXPORT void* mdpsr_state_circ_a_Surface(const mdpsr_factory_ctx* ctx) {
    return gtc_state_factory(ctx, kGtcProfiles[GTC_PROFILE_INDEX]);
}
MDPSR_EXPORT int mdpsr_queue_Queue_circ_a(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out) {
    return gtc_queue_factory(ctx, kGtcProfiles[GTC_PROFILE_INDEX], out);
}
MDPSR_EXPORT int mdpsr_handle_circ_a_Tick(const mdpsr_msg* msg, const uint8_t* body,
                                          uint32_t len, const mdpsr_ctx* ctx) {
    return gtc_tick_handle(kGtcProfiles[GTC_PROFILE_INDEX], msg, body, len, ctx);
}
MDPSR_EXPORT int mdpsr_handle_circ_a_Events(const mdpsr_msg* msg, const uint8_t* body,
                                            uint32_t len, const mdpsr_ctx* ctx) {
    return gtc_events_handle(kGtcProfiles[GTC_PROFILE_INDEX], msg, body, len, ctx);
}

MDPSR_DECL_ABI_VERSION()

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    gtc_log(ctx->host, 0, "[circ_a] 已挂载 (GUI 测试客户端; 一行 Windows 代码都没有)");
    return MDPSR_OK;
}
MDPSR_EXPORT void mdpsr_module_fini(void) {}
