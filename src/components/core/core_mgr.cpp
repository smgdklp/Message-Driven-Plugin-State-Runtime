/* ============================================================================
 *  core_Mgr.cpp —— 内核管理对象的实现
 * ==========================================================================*/
#include "core_mgr.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace mdpsr {
namespace core {

static void clog(const mdpsr_host* H, int level, const std::string& s) {
    if (H && H->log) H->log(H->host, level, s.c_str());
}

/* 从二级字典里安全地取出资源指针。
 * "拿到非空"只说明这一瞬间有效 —— 但调用者 (core_Handle_Mgr) 已经持有
 * 对应条目的锁了, 所以在本调用期间它不会被卸载掉。 */
static void* payload_of(mdpsr_map* tbl, const char* name, const mdpsr_host* H) {
    if (!tbl || !name || !H) return nullptr;
    mdpsr_ptr* p = H->map_find(tbl, mdpsr_hash64(name));
    if (!p) return nullptr;
    if (!H->map_is_valid(H->host, p)) return nullptr;
    return H->map_ptr_of(H->host, p);
}

ResourceDict* DllMgr::dict_of(mdpsr_map* state_table) const {
    auto* d = static_cast<ResourceDict*>(payload_of(state_table, "core_State_ResourceDict", _h));
    if (!d || d->magic != ResourceDict::kMagic) return nullptr;
    return d;
}

ResourceEntry* DllMgr::entry_of(ResourceDict* d, uint64_t plugin) const {
    if (!d) return nullptr;
    for (uint32_t i = 0; i < CORE_MAX_PLUGINS; ++i) {
        if (d->entries[i].plugin == plugin) return &d->entries[i];
    }
    return nullptr;
}

ResourceEntry* DllMgr::take_free(ResourceDict* d) const {
    if (!d) return nullptr;
    for (uint32_t i = 0; i < CORE_MAX_PLUGINS; ++i) {
        ResourceEntry& e = d->entries[i];
        if (e.plugin == 0) {
            e.count = 0;
            e.pad = 0;
            std::memset(e.keys, 0, sizeof(e.keys));
            return &e;
        }
    }
    return nullptr;
}

/* ==========================================================================
 *  cmd = 1 装载
 * ==========================================================================*/
int DllMgr::do_load(const char* manifest_rel, ResourceDict* d) {
    if (!manifest_rel || !*manifest_rel) return MDPSR_ERR_BAD_CONFIG;
    if (!d) return MDPSR_ERR_BAD_STATE;

    uint64_t plugin = 0;
    const int r = _h->plugin_load(_h->host, manifest_rel, &plugin);
    if (r != MDPSR_OK) {
        if (r == MDPSR_ERR_ALREADY_EXISTS) {
            clog(_h, 1, std::string("[core] 已经装过了, 跳过: ") + manifest_rel);
            return MDPSR_OK;
        }
        clog(_h, 2, std::string("[core] 装载失败(") + std::to_string(r) + "): " + manifest_rel);
        return r;
    }

    ResourceEntry* e = entry_of(d, plugin);
    if (!e) {
        e = take_free(d);
        if (!e) {
            clog(_h, 2, "[core] 资源字典满了: 资源没法登记 (插件本身已经装上了)");
            return MDPSR_ERR_MAP_NO_SPACE;
        }
        e->plugin = plugin;
        d->count++;
    }

    uint32_t n = 0;
    _h->plugin_keys(_h->host, plugin, nullptr, 0, &n);
    if (n > CORE_MAX_KEYS) {
        clog(_h, 1, "[core] 插件资源数 " + std::to_string(n) + " 超过字典容量 " +
                       std::to_string(CORE_MAX_KEYS) + ", 只登记前 " +
                       std::to_string(CORE_MAX_KEYS) + " 项");
        n = CORE_MAX_KEYS;
    }
    e->count = 0;
    if (n) _h->plugin_keys(_h->host, plugin, e->keys, CORE_MAX_KEYS, &n);
    e->count = n;

    clog(_h, 0, std::string("[core] 装载完成: ") + manifest_rel +
                   " -> plugin=" + std::to_string(plugin) +
                   ", 登记资源 " + std::to_string(n) + " 项");
    return MDPSR_OK;
}

/* ==========================================================================
 *  cmd = 2 卸载
 * ==========================================================================*/
int DllMgr::do_unload(uint64_t plugin, ResourceDict* d) {
    if (!d) return MDPSR_ERR_BAD_STATE;

    const int r = _h->plugin_unload(_h->host, plugin);
    if (r == MDPSR_ERR_PLUGIN_BUSY) {
        /* 有人正在用 -> 不是错, 是"还没轮到"。告诉调用方把消息回滚给自己。 */
        clog(_h, 1, "[core] 卸载推迟: plugin=" + std::to_string(plugin) + " 有资源正在使用");
        return r;
    }
    if (r != MDPSR_OK) {
        clog(_h, 2, "[core] 卸载失败(" + std::to_string(r) + "): plugin=" + std::to_string(plugin));
        return r;
    }

    ResourceEntry* e = entry_of(d, plugin);
    if (e) {
        e->plugin = 0;                 /* 槽位可复用 */
        e->count = 0;
        std::memset(e->keys, 0, sizeof(e->keys));
        if (d->count) d->count--;
    }
    clear_retry(plugin);
    clog(_h, 0, "[core] 卸载完成: plugin=" + std::to_string(plugin));
    return MDPSR_OK;
}

/* ==========================================================================
 *  命令分发
 * ==========================================================================*/
int DllMgr::handle_cmd(const Cmd& c, mdpsr_map* state_table, bool* need_retry) {
    if (need_retry) *need_retry = false;
    ResourceDict* d = dict_of(state_table);
    if (!d) {
        clog(_h, 2, "[core] ResourceDict 不可用");
        return MDPSR_ERR_BAD_STATE;
    }

    /* 看门狗: 上一次操作要是卡住了, 这里把它捡回来, 免得永久 busy */
    if (_busy && std::chrono::steady_clock::now() >= _watchdog_at) {
        clog(_h, 1, "[core] 看门狗: 上一次操作超时, 清掉 busy 标记");
        _busy = false;
    }

    switch (c.cmd) {

    /* ---- cmd = 0 初始化 ---- */
    case MDPSR_CMD_INIT:
        clog(_h, 0, "[core] cmd=0 初始化: 资源字典就绪 (" +
                       std::to_string(CORE_MAX_PLUGINS) + " 个插件槽 x " +
                       std::to_string(CORE_MAX_KEYS) + " 项资源)");
        return MDPSR_OK;

    /* ---- cmd = 1 装载 ---- */
    case 1: {
        if (_busy) {
            clog(_h, 1, "[core] cmd=1 被挡下: 上一次操作还在途中");
            if (need_retry) *need_retry = true;
            return MDPSR_ERR_BUSY;
        }
        _busy = true;
        _watchdog_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        const int r = do_load(c.path, d);
        _busy = false;
        return r;
    }

    /* ---- cmd = 2 卸载 ----
     * 卸载失败且原因是 BUSY 时, 由调用方把这条消息回滚给自己 ——
     * 只有调用方知道队列在哪。 */
    case 2: {
        const int r = do_unload(c.plugin, d);
        if (r == MDPSR_ERR_PLUGIN_BUSY && need_retry) *need_retry = true;
        return r;
    }

    default:
        clog(_h, 1, "[core] 未知命令 " + std::to_string(c.cmd));
        return MDPSR_ERR_UNKNOWN_CMD;
    }
}

int DllMgr::retries_of(uint64_t plugin) const {
    for (uint32_t i = 0; i < kMaxTracked; ++i) {
        if (_retry_key[i] == plugin) return _retry_cnt[i];
    }
    return 0;
}

void DllMgr::bump_retry(uint64_t plugin) {
    for (uint32_t i = 0; i < kMaxTracked; ++i) {
        if (_retry_key[i] == plugin) { _retry_cnt[i]++; return; }
    }
    for (uint32_t i = 0; i < kMaxTracked; ++i) {
        if (_retry_key[i] == 0) {
            _retry_key[i] = plugin;
            _retry_cnt[i] = 1;
            return;
        }
    }
    /* 表满了: 退化到第一格, 只影响统计, 不影响正确性 */
    _retry_key[0] = plugin;
    _retry_cnt[0]++;
}

void DllMgr::clear_retry(uint64_t plugin) {
    for (uint32_t i = 0; i < kMaxTracked; ++i) {
        if (_retry_key[i] == plugin) {
            _retry_key[i] = 0;
            _retry_cnt[i] = 0;
            return;
        }
    }
}

} /* namespace core */
} /* namespace mdpsr */
