/* ============================================================================
 *  circ_b.cpp —— GUI 测试插件 B (图比窗口大 + 缩放动画 + 整窗可拖)
 *
 *  逻辑全在 test/gui_client_common.h; 参数在 kGtcProfiles[1]。
 *
 *  它验证的: 图片比窗口大时由**系统**裁掉 (画布只覆盖窗口那么大)、缩放正确
 *  (0.6~1.5 来回摆)、以及"整窗可拖"是客户端在状态里点名才开的。
 * ==========================================================================*/
#include "gui_client_common.h"

#define GTC_PROFILE_INDEX 1

MDPSR_EXPORT void* mdpsr_state_circ_b_Surface(const mdpsr_factory_ctx* ctx) {
    return gtc_state_factory(ctx, kGtcProfiles[GTC_PROFILE_INDEX]);
}
MDPSR_EXPORT int mdpsr_queue_Queue_circ_b(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out) {
    return gtc_queue_factory(ctx, kGtcProfiles[GTC_PROFILE_INDEX], out);
}
MDPSR_EXPORT int mdpsr_handle_circ_b_Tick(const mdpsr_msg* msg, const uint8_t* body,
                                          uint32_t len, const mdpsr_ctx* ctx) {
    return gtc_tick_handle(kGtcProfiles[GTC_PROFILE_INDEX], msg, body, len, ctx);
}
MDPSR_EXPORT int mdpsr_handle_circ_b_Events(const mdpsr_msg* msg, const uint8_t* body,
                                            uint32_t len, const mdpsr_ctx* ctx) {
    return gtc_events_handle(kGtcProfiles[GTC_PROFILE_INDEX], msg, body, len, ctx);
}

MDPSR_DECL_ABI_VERSION()

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    gtc_log(ctx->host, 0, "[circ_b] 已挂载 (GUI 测试客户端; 图比窗口大, 缩放动画, 可拖)");
    return MDPSR_OK;
}
MDPSR_EXPORT void mdpsr_module_fini(void) {}
