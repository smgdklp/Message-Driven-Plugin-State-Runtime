/* ============================================================================
 *  mdpsr/scr/main/loader.cpp
 *  插件清单解析 + 统一的插件装载 / 卸载实现
 *
 *  宿主引导和 DLLMgr 的 cmd=1/cmd=2 走的是同一套代码 (host->plugin_load /
 *  host->plugin_unload), 所以内核 DLLMgr 自己也只是「在 plugin.json 里声明了
 *  queue/state/object/handle 的普通插件」, 没有任何特殊路径。
 * ==========================================================================*/
#include "runtime.h"

#include <cstring>
#include <thread>

namespace mdpsr {

static std::string dir_of(const std::string& p) {
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? std::string() : p.substr(0, k);
}

template <typename T>
static T* pool_new(mdpsr_resource* res) {
    void* m = res->allocate(sizeof(T), alignof(T));
    if (!m) return nullptr;
    return new (m) T{};
}

template <typename T>
static void pool_delete(mdpsr_resource* res, T* p) {
    if (!p) return;
    p->~T();
    res->deallocate(p, sizeof(T), alignof(T));
}

static bool get_symbol(HMODULE mod, const std::string& sym, void** out) {
    if (!mod || sym.empty()) return false;
    FARPROC p = ::GetProcAddress(mod, sym.c_str());
    if (!p) return false;
    *out = reinterpret_cast<void*>(p);
    return true;
}

/* ==========================================================================
 *  清单解析
 * ==========================================================================*/
static void read_entry_list(const Json& node, std::vector<ManifestEntry>& out) {
    if (!node.is_object()) return;

    for (const auto& kv : node.items()) {
        ManifestEntry e;
        e.name = kv.first;
        const Json& v = kv.second;

        if (v.is_string()) {
            e.symbol = v.as_string();
        } else if (v.is_object()) {
            e.symbol = v["symbol"].as_string(kv.first);
            e.type   = v["type"].as_string();

            const Json& p = v["param"];
            if (p.is_array()) {
                for (const Json& x : p.elements()) e.list.push_back(x.as_string());
            } else if (p.is_string()) {
                e.param = p.as_string();
            }

            e.bind_object = v["object"].as_string();
            e.bind_queue  = v["queue"].as_string();

            /* "state": "X" 是主 state; "states": ["X","Y"] 是全部绑定 */
            const std::string primary = v["state"].as_string();
            if (!primary.empty()) e.bind_states.push_back(primary);
            const Json& sts = v["states"];
            if (sts.is_array()) {
                for (const Json& x : sts.elements()) {
                    const std::string s = x.as_string();
                    if (s.empty()) continue;
                    bool dup = false;
                    for (const auto& b : e.bind_states) if (b == s) { dup = true; break; }
                    if (!dup) e.bind_states.push_back(s);
                }
            } else if (sts.is_string()) {
                const std::string s = sts.as_string();
                if (!s.empty()) e.bind_states.push_back(s);
            }
        } else {
            continue;
        }

        if (e.symbol.empty()) e.symbol = e.name;
        if (e.type.empty())   e.type = "handle";
        e.type_id = (e.type == "handle_core" || e.type == "core")
                        ? MDPSR_ENTRY_HANDLE_CORE
                        : MDPSR_ENTRY_HANDLE;
        out.push_back(std::move(e));
    }
}

/* queue 段支持两种写法:
 *      常规: { "Capture.Thread": { "symbol": "mdpsr_queue_capture", "capacity": 8192 } }
 *      简写: { "mdpsr_queue_capture": "Capture.Thread" }    // 键是 symbol, 值是队列名 */
static void read_queue_list(const Json& node, std::vector<ManifestEntry>& out) {
    if (!node.is_object()) return;

    for (const auto& kv : node.items()) {
        ManifestEntry e;
        const Json& v = kv.second;

        if (v.is_string()) {
            e.symbol = kv.first;
            e.name   = v.as_string();
        } else if (v.is_object()) {
            e.name   = kv.first;
            e.symbol = v["symbol"].as_string();     /* 队列的 symbol 可以为空 */
            const Json& p = v["param"];
            if (p.is_array()) {
                for (const Json& x : p.elements()) e.list.push_back(x.as_string());
            } else if (p.is_string()) {
                e.param = p.as_string();
            }
            e.capacity = static_cast<uint32_t>(v["capacity"].as_int(0));
        } else {
            continue;
        }

        if (e.name.empty()) continue;
        e.type_id = MDPSR_ENTRY_QUEUE;
        out.push_back(std::move(e));
    }
}

int Runtime::load_manifest(const std::string& rel_json, uint64_t* out_dll) {
    const uint64_t key = mdpsr_hash64(rel_json.c_str());
    if (out_dll) *out_dll = key;

    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        if (_manifests.find(key) != _manifests.end()) return MDPSR_OK;
    }

    Json j;
    std::string err;
    if (!Json::load_file(resolve(rel_json), &j, true, &err)) {
        log(2, "读取插件清单失败: " + rel_json + " (" + err + ")");
        return MDPSR_ERR_PLUGIN_NOT_FOUND;
    }

    Manifest m;
    m.rel_path = rel_json;
    m.dir      = dir_of(rel_json);

    const Json& dp = j["dll_path"];
    if (dp.is_string()) {
        char buf[MDPSR_PATH_MAX];
        mdpsr_path_join(buf, sizeof(buf), m.dir.c_str(), dp.as_string().c_str());
        m.dlls.push_back(buf);
    } else if (dp.is_array()) {
        for (const Json& d : dp.elements()) {
            char buf[MDPSR_PATH_MAX];
            mdpsr_path_join(buf, sizeof(buf), m.dir.c_str(), d.as_string().c_str());
            m.dlls.push_back(buf);
        }
    } else {
        log(2, "插件清单缺少 dll_path: " + rel_json);
        return MDPSR_ERR_BAD_CONFIG;
    }

    read_queue_list(j["queue"],  m.queues);
    read_entry_list(j["state"],  m.states);
    read_entry_list(j["object"], m.objects);
    read_entry_list(j["handle"], m.handles);

    log(0, "装载清单 " + rel_json +
           "  dll=" + std::to_string(m.dlls.size()) +
           " queue=" + std::to_string(m.queues.size()) +
           " state=" + std::to_string(m.states.size()) +
           " object=" + std::to_string(m.objects.size()) +
           " handle=" + std::to_string(m.handles.size()));

    std::lock_guard<std::mutex> lk(_dll_mtx);
    _manifests.emplace(key, std::move(m));
    return MDPSR_OK;
}

const Manifest* Runtime::manifest(uint64_t dll) const {
    std::lock_guard<std::mutex> lk(_dll_mtx);
    auto it = _manifests.find(dll);
    return it == _manifests.end() ? nullptr : &it->second;
}

/* ==========================================================================
 *  插件装载
 * ==========================================================================*/
int Runtime::plugin_load(const std::string& manifest_rel, uint64_t* out_dll) {
    std::lock_guard<std::mutex> plk(_plugin_mtx);

    uint64_t dll = 0;
    int r = load_manifest(manifest_rel, &dll);
    if (r != MDPSR_OK) return r;
    if (out_dll) *out_dll = dll;

    if (module_loaded(dll)) {
        log(1, "插件已加载, 跳过: " + manifest_rel);
        return MDPSR_ERR_ALREADY_EXISTS;
    }

    Manifest m;
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        auto it = _manifests.find(dll);
        if (it == _manifests.end()) return MDPSR_ERR_PLUGIN_NOT_FOUND;
        m = it->second;
    }
    if (m.dlls.empty()) return MDPSR_ERR_BAD_CONFIG;

    /* --- 1. 主动 new 挂载 (一个清单可以带多个 dll, 每个都要 init) --- */
    HMODULE first = nullptr;
    for (const std::string& d : m.dlls) {
        void* mod = nullptr;
        r = module_load(dll, d, &mod);
        if (r != MDPSR_OK) {
            module_unload(dll);
            return r;
        }
        if (!first) first = static_cast<HMODULE>(mod);
        r = module_ready(mod);
        if (r != MDPSR_OK) {
            module_unload(dll);
            return r;
        }
    }
    if (!first) return MDPSR_ERR_BAD_CONFIG;

    /* --- 2. 专属池 --- */
    mdpsr_dllptr dp{};
    if (dll_pool_create(dll, manifest_rel.c_str(), &dp) != MDPSR_OK) {
        module_unload(dll);
        return MDPSR_ERR_MAP_POOL;
    }

    std::vector<uint64_t> keys;
    mdpsr_resource* desc_res = _pools.state();
    mdpsr_state_ctx  sctx{};
    mdpsr_object_ctx octx{};
    std::vector<const char*> plist;

    /* --- 3. 分流队列: 组件申请自己的队列 + 专属分发线程 --- */
    for (const ManifestEntry& e : m.queues) {
        uint32_t cap = e.capacity;

        if (!e.symbol.empty()) {
            void* sym = nullptr;
            if (get_symbol(first, e.symbol, &sym)) {
                plist.clear();
                for (const auto& s : e.list) plist.push_back(s.c_str());
                mdpsr_queue_ctx qctx{};
                qctx.name         = e.name.c_str();
                qctx.param        = e.param.empty() ? nullptr : e.param.c_str();
                qctx.list         = plist.empty() ? nullptr : plist.data();
                qctx.list_count   = plist.size();
                qctx.manifest     = m.rel_path.c_str();
                qctx.manifest_dir = m.dir.c_str();
                qctx.dll          = dll;
                qctx.pool         = _pools.msg();
                qctx.host         = &_api;

                mdpsr_queue_desc desc{};
                desc.struct_size = static_cast<uint32_t>(sizeof(desc));
                const int qr = reinterpret_cast<mdpsr_queue_fn>(sym)(&qctx, &desc);
                if (qr != MDPSR_OK) {
                    log(1, "queue '" + e.name + "' 工厂拒绝建立 (返回 " +
                           std::to_string(qr) + "), 跳过");
                    continue;
                }
                if (desc.capacity) cap = desc.capacity;
            } else {
                log(1, "queue '" + e.name + "' 找不到导出 " + e.symbol + ", 用默认参数");
            }
        }

        mdpsr_queue* q = nullptr;
        if (queue_create(e.name, cap, dll, false, &q) != MDPSR_OK || !q) {
            log(2, "queue '" + e.name + "' 建立失败");
            continue;
        }
        keys.push_back(q->name);
    }

    /* --- 4. State: 用 Pool_state 调插件工厂, 把公用配置/数据挂到 Map --- */
    for (const ManifestEntry& e : m.states) {
        void* sym = nullptr;
        if (!get_symbol(first, e.symbol, &sym)) {
            log(2, "state '" + e.name + "' 找不到导出 " + e.symbol);
            continue;
        }
        plist.clear();
        for (const auto& s : e.list) plist.push_back(s.c_str());

        sctx.name         = e.name.c_str();
        sctx.param        = e.param.empty() ? nullptr : e.param.c_str();
        sctx.list         = plist.empty() ? nullptr : plist.data();
        sctx.list_count   = plist.size();
        sctx.manifest     = m.rel_path.c_str();
        sctx.manifest_dir = m.dir.c_str();
        sctx.dll          = dll;
        sctx.pool         = _pools.state();
        sctx.host         = &_api;

        mdpsr_state* st = reinterpret_cast<mdpsr_state_fn>(sym)(&sctx);
        if (!st) {
            log(2, "state '" + e.name + "' 创建失败");
            continue;
        }
        st->name = mdpsr_hash64(e.name.c_str());
        st->dll  = dll;

        mdpsr_entry ent{};
        ent.key  = st->name;
        ent.dll  = dll;
        ent.type = MDPSR_ENTRY_STATE;
        ent.name = e.name.c_str();
        ent.ptr  = st;
        if (_map.reg(ent) != MDPSR_OK) {
            log(1, "state '" + e.name + "' 注册失败 (键冲突?)");
            mdpsr_state_delete(st);
            continue;
        }
        keys.push_back(st->name);
        log(0, "  state  " + e.name + "  kind=" + std::to_string(st->kind) +
               " size=" + std::to_string(st->size));
    }

    /* --- 5. Object: 用 DLL 专属池 new 出类 --- */
    for (const ManifestEntry& e : m.objects) {
        void* sym = nullptr;
        if (!get_symbol(first, e.symbol, &sym)) {
            log(2, "object '" + e.name + "' 找不到导出 " + e.symbol);
            continue;
        }
        plist.clear();
        for (const auto& s : e.list) plist.push_back(s.c_str());

        octx.name         = e.name.c_str();
        octx.param        = e.param.empty() ? nullptr : e.param.c_str();
        octx.list         = plist.empty() ? nullptr : plist.data();
        octx.list_count   = plist.size();
        octx.manifest     = m.rel_path.c_str();
        octx.manifest_dir = m.dir.c_str();
        octx.dll          = dll;
        octx.pool         = dp.pool;
        octx.host         = &_api;

        void* obj = reinterpret_cast<mdpsr_object_fn>(sym)(&octx);
        if (!obj) {
            log(2, "object '" + e.name + "' 创建失败");
            continue;
        }

        mdpsr_classptr* cp = pool_new<mdpsr_classptr>(desc_res);
        cp->name     = mdpsr_hash64(e.name.c_str());
        cp->dll      = dll;
        cp->instance = obj;
        {
            void* d = nullptr;
            if (get_symbol(first, e.symbol + "_destroy", &d)) cp->destroy = d;
        }

        mdpsr_entry ent{};
        ent.key  = cp->name;
        ent.dll  = dll;
        ent.type = MDPSR_ENTRY_OBJECT;
        ent.name = e.name.c_str();
        ent.ptr  = cp;
        if (_map.reg(ent) != MDPSR_OK) {
            log(1, "object '" + e.name + "' 注册失败 (键冲突?)");
            if (cp->destroy) {
                reinterpret_cast<mdpsr_object_destroy_fn>(cp->destroy)(cp->instance);
            }
            pool_delete(desc_res, cp);
            continue;
        }
        keys.push_back(cp->name);
        log(0, std::string("  object ") + e.name + "  destroy=" + (cp->destroy ? "yes" : "no"));
    }

    /* --- 6. Handle / Handle_core: 注册, 绑定 state / object / 分流队列 --- */
    for (const ManifestEntry& e : m.handles) {
        void* sym = nullptr;
        if (!get_symbol(first, e.symbol, &sym)) {
            log(2, "handle '" + e.name + "' 找不到导出 " + e.symbol);
            continue;
        }
        const bool is_core = (e.type_id == MDPSR_ENTRY_HANDLE_CORE);

        mdpsr_handle_core* hc = pool_new<mdpsr_handle_core>(desc_res);
        hc->base.id   = mdpsr_hash64(e.name.c_str());
        hc->base.dll  = dll;
        hc->base.kind = is_core ? 1u : 0u;
        hc->base.fn   = reinterpret_cast<mdpsr_handle_fn>(sym);
        std::snprintf(hc->base.name, sizeof(hc->base.name), "%s", e.name.c_str());
        hc->is_core = is_core ? 1u : 0u;

        mdpsr_entry ent{};
        ent.key      = hc->base.id;
        ent.dll      = dll;
        ent.type     = is_core ? MDPSR_ENTRY_HANDLE_CORE : MDPSR_ENTRY_HANDLE;
        ent.name     = e.name.c_str();
        ent.ptr      = hc;
        ent.bind_object = e.bind_object.empty() ? 0 : mdpsr_hash64(e.bind_object.c_str());
        for (const std::string& s : e.bind_states) {
            if (ent.bind_state_count >= MDPSR_MAX_BIND_STATES) break;
            ent.bind_states[ent.bind_state_count++] = mdpsr_hash64(s.c_str());
        }
        if (!e.bind_queue.empty()) {
            const uint64_t qk = mdpsr_hash64(e.bind_queue.c_str());
            if (queue_find(qk)) {
                ent.bind_queue = qk;
            } else {
                log(1, "handle '" + e.name + "' 想绑定的队列 '" + e.bind_queue +
                       "' 不存在, 回退 queue_core");
            }
        }
        if (_map.reg(ent) != MDPSR_OK) {
            log(1, "handle '" + e.name + "' 注册失败 (键冲突?)");
            pool_delete(desc_res, hc);
            continue;
        }
        keys.push_back(hc->base.id);

        std::string bind_desc;
        for (const std::string& s : e.bind_states) bind_desc += s + " ";
        log(0, std::string("  ") + (is_core ? "handle_core" : "handle     ") + " " +
               e.name + "  state=[" + bind_desc + "] object=" +
               (e.bind_object.empty() ? "-" : e.bind_object) + " queue=" +
               (e.bind_queue.empty() ? MDPSR_QUEUE_CORE_NAME : e.bind_queue));
    }

    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        _keys[dll] = keys;
    }
    log(0, "插件装载完成: " + manifest_rel + " (资源 " + std::to_string(keys.size()) + " 项)");
    return MDPSR_OK;
}

/* ==========================================================================
 *  插件卸载
 *
 *  多线程注意: 卸载时要 join 该 dll 自己的分流线程, 而那条线程可能正卡在
 *  _plugin_mtx 上等我们。所以 join 必须放到 _plugin_mtx 之外做。
 * ==========================================================================*/
int Runtime::plugin_unload(uint64_t dll) {
    std::unique_lock<std::mutex> plk(_plugin_mtx);

    if (!module_loaded(dll)) return MDPSR_ERR_PLUGIN_NOT_FOUND;

    std::vector<uint64_t> keys;
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        auto it = _keys.find(dll);
        if (it != _keys.end()) keys = it->second;
    }

    /* --- 0. 趁条目还有效, 先抄下位置/类型 (invalidate 之后就查不到了) --- */
    struct Item { uint64_t key; int type; void* ptr; };
    std::vector<Item> items;
    for (uint64_t k : keys) {
        Countptr* cp = nullptr;
        if (_map.acquire(k, false, &cp) != MDPSR_OK || !cp) continue;
        items.push_back(Item{ k, cp->type, cp->ptr });
        _map.release(k, false);
    }

    /* --- 1. 统一失效 + 等计数归零 --- */
    for (uint64_t k : keys) _map.invalidate(k);
    bool idle = true;
    for (uint64_t k : keys) {
        if (_map.wait_idle(k, 5000) != MDPSR_OK) {
            idle = false;
            log(1, "资源 0x" + std::to_string(k) + " 等待超时");
        }
    }
    if (_map.dll_idle(dll, 5000) != MDPSR_OK) idle = false;
    if (!idle) {
        log(2, "卸载中止: 仍有活跃读写计数, dll=" + std::to_string(dll));
        return MDPSR_ERR_PLUGIN_BUSY;
    }

    /* --- 2. 把这个 dll 的分流队列摘出来并置停 (join 放到锁外) --- */
    std::vector<QueueSlot> qslots;
    for (const Item& it : items) {
        if (it.type != MDPSR_ENTRY_QUEUE) continue;
        auto* q = static_cast<mdpsr_queue*>(it.ptr);
        if (!q) continue;

        QueueSlot slot;
        bool got = false;
        {
            std::lock_guard<std::mutex> lk(_queue_mtx);
            auto fit = _queues.find(q->name);
            if (fit != _queues.end() && fit->second.info == q) {
                slot = fit->second;
                _queues.erase(fit);
                got = true;
            }
        }
        if (!got) continue;

        if (slot.disp && slot.disp->thread_id() == std::this_thread::get_id()) {
            std::lock_guard<std::mutex> lk(_queue_mtx);
            _queues.emplace(slot.key, slot);
            log(2, "卸载中止: 队列在自己的分发线程上被要求销毁 (会自 join)");
            return MDPSR_ERR_PLUGIN_BUSY;
        }
        if (slot.disp) slot.disp->request_stop();
        qslots.push_back(slot);
    }

    /* --- 3. 锁外 join, 避免与"正在等 _plugin_mtx 的分发线程"死锁 --- */
    plk.unlock();
    for (QueueSlot& s : qslots) {
        if (s.thread) {
            _threads.join(s.thread);
            s.thread = nullptr;
        }
    }
    plk.lock();

    /* --- 4. 依次析构 object / state / handle --- */
    mdpsr_resource* desc_res = _pools.state();
    for (const Item& it : items) {
        if (it.type == MDPSR_ENTRY_QUEUE) continue;      /* 队列在下面统一释放 */
        if (it.type == MDPSR_ENTRY_OBJECT) {
            auto* cptr = static_cast<mdpsr_classptr*>(it.ptr);
            if (!cptr) continue;
            if (cptr->destroy) {
                reinterpret_cast<mdpsr_object_destroy_fn>(cptr->destroy)(cptr->instance);
            }
            cptr->instance = nullptr;
            pool_delete(desc_res, cptr);
        } else if (it.type == MDPSR_ENTRY_STATE) {
            mdpsr_state_delete(static_cast<mdpsr_state*>(it.ptr));
        } else if (it.type == MDPSR_ENTRY_HANDLE || it.type == MDPSR_ENTRY_HANDLE_CORE) {
            pool_delete(desc_res, static_cast<mdpsr_handle_core*>(it.ptr));
        }
    }

    /* --- 5. 释放队列槽位 (线程已 join) --- */
    for (QueueSlot& s : qslots) {
        if (s.key == _core_queue_key) _core_queue_key = 0;
        release_slot(s);
    }

    /* --- 6. 摘除剩余的 Map 条目 --- */
    for (uint64_t k : keys) _map.erase(k);
    {
        std::lock_guard<std::mutex> lk(_dll_mtx);
        _keys.erase(dll);
    }

    /* --- 7. 整池释放 --- */
    _pools.dll_pool_drop(dll);

    /* --- 8. 卸载模块 --- */
    module_unload(dll);
    log(0, "插件卸载完成: dll=" + std::to_string(dll) + " (释放 " +
           std::to_string(items.size()) + " 项资源)");
    return MDPSR_OK;
}

int Runtime::plugin_keys(uint64_t dll, uint64_t* out, uint32_t cap, uint32_t* out_count) {
    std::lock_guard<std::mutex> lk(_dll_mtx);
    auto it = _keys.find(dll);
    if (it == _keys.end()) {
        if (out_count) *out_count = 0;
        return MDPSR_OK;
    }
    uint32_t n = 0;
    for (uint64_t k : it->second) {
        if (out && n < cap) out[n] = k;
        ++n;
    }
    if (out_count) *out_count = n;
    return MDPSR_OK;
}

} /* namespace mdpsr */
