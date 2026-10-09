/* ============================================================================
 *  circ_c.cpp —— GUI 测试插件 C (放大 2 倍 + 偏置跑到出界)
 *
 *  逻辑全在 test/gui_client_common.h; 参数在 kGtcProfiles[2]。
 *
 *  它验证的: "先缩放、再偏置、再写画布、窗口外交给系统裁"这条管线 —— 偏置摆到
 *  负值 (图的左上角跑到窗口外) 时, 窗口里剩下的那部分必须正确, 而且**没被图
 *  盖到的画布像素必须是 alpha=0 (透明)**。顺带验证点击关窗 + 自愈。
 * ==========================================================================*/
#include "gui_client_common.h"

#define GTC_PROFILE_INDEX 2

MDPSR_EXPORT void* mdpsr_state_circ_c_Surface(const mdpsr_factory_ctx* ctx) {
    return gtc_state_factory(ctx, kGtcProfiles[GTC_PROFILE_INDEX]);
}
MDPSR_EXPORT int mdpsr_queue_Queue_circ_c(const mdpsr_factory_ctx* ctx, mdpsr_queue_desc* out) {
    return gtc_queue_factory(ctx, kGtcProfiles[GTC_PROFILE_INDEX], out);
}
MDPSR_EXPORT int mdpsr_handle_circ_c_Tick(const mdpsr_msg* msg, const uint8_t* body,
                                          uint32_t len, const mdpsr_ctx* ctx) {
    return gtc_tick_handle(kGtcProfiles[GTC_PROFILE_INDEX], msg, body, len, ctx);
}
MDPSR_EXPORT int mdpsr_handle_circ_c_Events(const mdpsr_msg* msg, const uint8_t* body,
                                            uint32_t len, const mdpsr_ctx* ctx) {
    return gtc_events_handle(kGtcProfiles[GTC_PROFILE_INDEX], msg, body, len, ctx);
}

MDPSR_DECL_ABI_VERSION()

MDPSR_EXPORT int mdpsr_module_init(const mdpsr_factory_ctx* ctx) {
    if (!ctx || !ctx->host) return MDPSR_ERR_NULLPTR;
    gtc_log(ctx->host, 0, "[circ_c] 已挂载 (GUI 测试客户端; 放大 2 倍, 偏置会出界)");
    return MDPSR_OK;
}
MDPSR_EXPORT void mdpsr_module_fini(void) {}
