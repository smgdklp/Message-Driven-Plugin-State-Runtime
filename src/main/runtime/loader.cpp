/* ============================================================================
 *  mdpsr/runtime/loader.cpp
 *  清单解析 + 插件装载 / 卸载
 *
 *  plugin.json 的形状 (见 doc/插件规范.md 第六节):
 *
 *      {
 *        "name":       "core",                             // 插件名
 *        "dll_path":   ["mdpsr_core.dll"],                 // 相对本清单所在目录
 *        "State":      ["core_State_ResourceDict"],        // -> mdpsr_state_<条目名>
 *        "Object":     ["core_Mgr"],                       // -> mdpsr_object_<条目名>
 *        "Handle":     ["core_Handle_Mgr"],                // -> mdpsr_handle_<条目名>
 *        "Queue":      ["Queue_pluginmgr"],                // 可选: 额外要的队列
 *        "Init":       "core_Handle_Mgr"                   // 可选: 谁的 cmd=0 是初始化入口
 *      }
 *
 *  条目名 = 清单里写的那个字符串; 导出符号名由它推出来 (见 symbol_for_*)。
 *  两边用的是同一个字符串, 所以不存在"名字和 symbol 对不上"的可能。
 *
 *  ⚠ 条目名必须**全局唯一** —— MainMap 只有一张键空间, 不分种类。
 *    core 的三个条目特意带了前缀 (core_Mgr / core_Handle_Mgr / ...),
 *    否则 State/Object/Handle 都叫 "Mgr" 会互相撞键。
 * ==========================================================================*/
#include "runtime.h"

#include "json.h"
#include "loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace mdpsr {

/* UTF-8 -> 宽字符 (清单路径要交给 _wfopen) */
static std::wstring to_w(const std::string& u) {
    if (u.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &w[0], n);
    for (auto& c : w) if (c == L'/') c = L'\\';
    return w;
}

/* ==========================================================================
 *  符号名推导 (与 abi.h 的命名规范一致)
 *
 *  条目名是给人看的字符串, 允许带点号/横杠 (比如 "Ticker.Handle.Tick");
 *  但导出符号名里不能有这些字符, 所以这里统一规范化:
 *      非 [A-Za-z0-9_] 的字符 -> '_'
 *  规范化之后必须仍然唯一 —— 加载器会检查 (见 plugin_load 里的重名检测)。
 * ==========================================================================*/
static std::string sanitize(const std::string& n) {
    std::string s;
    s.reserve(n.size());
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        s.push_back(ok ? c : '_');
    }
    return s;
}

std::string symbol_for_state(const std::string& n)  { return "mdpsr_state_"  + sanitize(n); }
std::string symbol_for_object(const std::string& n) { return "mdpsr_object_" + sanitize(n); }
std::string symbol_for_handle(const std::string& n) { return "mdpsr_handle_" + sanitize(n); }
std::string symbol_for_queue(const std::string& n)  { return "mdpsr_queue_"  + sanitize(n); }

static std::string dir_of(const std::string& p) {
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? std::string() : p.substr(0, k);
}

/* 清单里的字符串数组 -> TagEntry 列表。接受 "X" 或 ["X","Y"] 两种写法。 */
static int read_tags(const Json& node, std::vector<TagEntry>* out, const char* what,
                     std::string* err) {
    if (node.is_null()) return MDPSR_OK;                 /* 字段可以整个不写 */
    if (node.is_string()) {
        TagEntry e;
        e.name = node.as_string();
        if (!e.name.empty()) out->push_back(std::move(e));
        return MDPSR_OK;
    }
    if (!node.is_array()) {
        if (err) *err = std::string(what) + " 必须是字符串或字符串数组";
        return MDPSR_ERR_BAD_CONFIG;
    }
    for (const Json& x : node.elements()) {
        if (!x.is_string()) {
            if (err) *err = std::string(what) + " 里的元素必须是字符串";
            return MDPSR_ERR_BAD_CONFIG;
        }
        TagEntry e;
        e.name = x.as_string();
        if (!e.name.empty()) out->push_back(std::move(e));
    }
    return MDPSR_OK;
}

int load_manifest(const std::string& rel_json, Manifest* out, std::string* err) {
    if (!out) return MDPSR_ERR_NULLPTR;

    /* rel_json 是"相对运行时根目录"的路径; Runtime::init 已经把当前目录
     * 设成根目录, 所以直接打开即可。 */
    Json j;
    if (!Json::load_file(to_w(rel_json), &j, err)) return MDPSR_ERR_PLUGIN_NOT_FOUND;
    if (!j.is_object()) {
        if (err) *err = "清单的顶层必须是对象";
        return MDPSR_ERR_BAD_CONFIG;
    }

    Manifest m;
    m.rel_path = rel_json;
    m.dir = dir_of(rel_json);
    m.plugin_name = j["name"].as_string();
    m.init_handle = j["Init"].as_string();

    if (m.plugin_name.empty()) {
        if (err) *err = "清单缺少 name";
        return MDPSR_ERR_BAD_CONFIG;
    }
    if (m.plugin_name.size() >= MDPSR_NAME_MAX) {
        if (err) *err = "name 太长 (上限 " + std::to_string(MDPSR_NAME_MAX - 1) + " 字节)";
        return MDPSR_ERR_NAME_TOO_LONG;
    }

    /* dll_path: 相对本清单所在目录 */
    {
        const Json& dp = j["dll_path"];
        auto push_one = [&](const std::string& raw) -> int {
            if (raw.empty()) return MDPSR_OK;
            char buf[MDPSR_PATH_MAX];
            mdpsr_path_join(buf, sizeof(buf), m.dir.c_str(), raw.c_str());
            if (!buf[0]) {
                if (err) *err = "dll_path 解析后为空: " + raw;
                return MDPSR_ERR_BAD_CONFIG;
            }
            m.dlls.emplace_back(buf);
            return MDPSR_OK;
        };
        if (dp.is_string()) {
            const int r = push_one(dp.as_string());
            if (r != MDPSR_OK) return r;
        } else if (dp.is_array()) {
            for (const Json& d : dp.elements()) {
                if (!d.is_string()) {
                    if (err) *err = "dll_path 里的元素必须是字符串";
                    return MDPSR_ERR_BAD_CONFIG;
                }
                const int r = push_one(d.as_string());
                if (r != MDPSR_OK) return r;
            }
        } else {
            if (err) *err = "清单缺少 dll_path";
            return MDPSR_ERR_BAD_CONFIG;
        }
        if (m.dlls.empty()) {
            if (err) *err = "dll_path 是空的";
            return MDPSR_ERR_BAD_CONFIG;
        }
    }

    int r = MDPSR_OK;
    if ((r = read_tags(j["State"],  &m.states,  "State",  err)) != MDPSR_OK) return r;
    if ((r = read_tags(j["Object"], &m.objects, "Object", err)) != MDPSR_OK) return r;
    if ((r = read_tags(j["Handle"], &m.handles, "Handle", err)) != MDPSR_OK) return r;
    if ((r = read_tags(j["Queue"],  &m.queues,  "Queue",  err)) != MDPSR_OK) return r;

    if (m.handles.empty()) {
        if (err) *err = "清单至少要声明一个 Handle";
        return MDPSR_ERR_BAD_CONFIG;
    }

    /* Init 必须指向一个真的声明过的 Handle */
    if (!m.init_handle.empty() && !m.has_tag(m.handles, m.init_handle)) {
        if (err) *err = "Init 指向的 Handle '" + m.init_handle + "' 没有在 Handle 里声明";
        return MDPSR_ERR_BAD_CONFIG;
    }

    *out = std::move(m);
    return MDPSR_OK;
}

/* ==========================================================================
 *  装载 / 卸载
 * ==========================================================================*/
PluginInfo* Runtime::find_plugin(uint64_t key) {
    for (PluginInfo* pi : _plugins) if (pi->key == key) return pi;
    return nullptr;
}

static void* get_symbol(HMODULE mod, const std::string& sym) {
    if (!mod || sym.empty()) return nullptr;
    return reinterpret_cast<void*>(::GetProcAddress(mod, sym.c_str()));
}

int Runtime::plugin_load(const std::string& manifest_rel, uint64_t* out_plugin) {
    std::lock_guard<std::mutex> plk(_plugin_mtx);

    Manifest m;
    std::string err;
    const int mr = load_manifest(manifest_rel, &m, &err);
    if (mr != MDPSR_OK) {
        log(2, "清单解析失败: " + manifest_rel + " (" + err + ")");
        return mr;
    }

    const uint64_t pkey = plugin_key(m.plugin_name);
    if (find_plugin(pkey)) {
        log(1, "插件已加载, 跳过: " + m.plugin_name);
        return MDPSR_ERR_ALREADY_EXISTS;
    }

    auto* pi = new PluginInfo();
    pi->key = pkey;
    pi->m = m;

    /* --- 1. LoadLibrary + ABI 版本核对 --- */
    for (const std::string& d : m.dlls) {
        const std::wstring full = resolve(d);
        HMODULE mod = ::LoadLibraryW(full.c_str());
        if (!mod) mod = ::LoadLibraryW(to_wide(d).c_str());
        if (!mod) {
            log(2, "LoadLibrary 失败 (" + std::to_string(GetLastError()) + "): " + d);
            for (void* mm : pi->modules) ::FreeLibrary(static_cast<HMODULE>(mm));
            delete pi;
            return MDPSR_ERR_PLUGIN_LOAD;
        }

        auto abi = reinterpret_cast<uint32_t (*)(void)>(
            ::GetProcAddress(mod, "mdpsr_abi_version"));
        if (!abi) {
            log(2, "插件没有导出 mdpsr_abi_version: " + d);
            ::FreeLibrary(mod);
            for (void* mm : pi->modules) ::FreeLibrary(static_cast<HMODULE>(mm));
            delete pi;
            return MDPSR_ERR_PLUGIN_EXPORT;
        }
        const uint32_t v = abi();
        if (v != MDPSR_ABI_VERSION) {
            log(2, "ABI 不匹配 (插件 " + std::to_string(v) + " != 宿主 " +
                   std::to_string(MDPSR_ABI_VERSION) + "): " + d);
            ::FreeLibrary(mod);
            for (void* mm : pi->modules) ::FreeLibrary(static_cast<HMODULE>(mm));
            delete pi;
            return MDPSR_ERR_PLUGIN_ABI;
        }

        auto init = reinterpret_cast<int (*)(const mdpsr_host*, uint64_t)>(
            ::GetProcAddress(mod, "mdpsr_module_init"));
        if (init) {
            const int rr = init(&_api, pkey);
            if (rr != MDPSR_OK) {
                log(2, "mdpsr_module_init 返回 " + std::to_string(rr) + ": " + d);
                ::FreeLibrary(mod);
                for (void* mm : pi->modules) ::FreeLibrary(static_cast<HMODULE>(mm));
                delete pi;
                return rr;
            }
        }
        pi->modules.push_back(mod);
        log(0, "LoadLibrary 成功: " + d);
    }

    HMODULE first = static_cast<HMODULE>(pi->modules.front());
    pi->ready = true;

    /* --- 2. 插件自己申请的额外队列 (缺省就用 Queue_default) --- */
    std::vector<Queue*> own_queues;
    for (const TagEntry& qe : m.queues) {
        if (qe.name == MDPSR_QUEUE_PLUGINMGR_NAME ||
            qe.name == MDPSR_QUEUE_DEFAULT_NAME ||
            qe.name == MDPSR_QUEUE_WINDOWSGUI_NAME) {
            continue;                          /* 三条固定队列, 不重复建 */
        }

        Queue::Desc desc;
        if (void* sym = get_symbol(first, symbol_for_queue(qe.name))) {
            mdpsr_factory_ctx fctx{};
            fctx.name = qe.name.c_str();
            fctx.plugin_name = m.plugin_name.c_str();
            fctx.manifest = m.rel_path.c_str();
            fctx.manifest_dir = m.dir.c_str();
            fctx.plugin = pkey;
            fctx.host = &_api;
            fctx.pool = _pool_msg.get();          /* 队列工厂拿到 Pool_msg */

            mdpsr_queue_desc qd{};
            qd.struct_size = sizeof(qd);
            const int qr = reinterpret_cast<mdpsr_queue_fn>(sym)(&fctx, &qd);
            if (qr != MDPSR_OK) {
                log(1, "队列 '" + qe.name + "' 的工厂拒绝建立 (rc=" + std::to_string(qr) + "), 跳过");
                continue;
            }
            if (qd.capacity) desc.capacity = qd.capacity;
            desc.pace_ms = qd.pace_ms;
        }

        Queue* q = nullptr;
        if (queue_create(qe.name, desc, pkey, false, &q) != MDPSR_OK || !q) {
            log(2, "队列 '" + qe.name + "' 建立失败");
            continue;
        }
        own_queues.push_back(q);
        pi->keys.push_back(mdpsr_hash64(qe.name.c_str()));
    }

    /* --- 3. State: 从 Pool_state 分配, 由 Ptr 托管 --- */
    for (const TagEntry& se : m.states) {
        void* sym = get_symbol(first, symbol_for_state(se.name));
        if (!sym) {
            log(2, "State '" + se.name + "' 找不到导出 " + symbol_for_state(se.name));
            continue;
        }
        mdpsr_factory_ctx fctx{};
        fctx.name = se.name.c_str();
        fctx.plugin_name = m.plugin_name.c_str();
        fctx.manifest = m.rel_path.c_str();
        fctx.manifest_dir = m.dir.c_str();
        fctx.plugin = pkey;
        fctx.host = &_api;
        fctx.pool = _pool_state.get();

        void* payload = reinterpret_cast<mdpsr_state_fn>(sym)(&fctx);
        if (!payload) {
            log(2, "State '" + se.name + "' 工厂返回空");
            continue;
        }
        const uint64_t sk = mdpsr_hash64(se.name.c_str());
        mdpsr_ptr* p = nullptr;
        const int ar = _map.attach(MDPSR_KIND_STATE, sk, se.name.c_str(), payload, &p);
        if (ar != MDPSR_OK) {
            log(1, "State '" + se.name + "' 注册失败 (rc=" + std::to_string(ar) + ")");
            continue;
        }
        pi->keys.push_back(sk);
        log(0, "  state  " + se.name);
    }

    /* --- 4. Object: 从统一 Pool_object 分配, 由 Ptr 托管 --- */
    for (const TagEntry& oe : m.objects) {
        void* sym = get_symbol(first, symbol_for_object(oe.name));
        if (!sym) {
            log(2, "Object '" + oe.name + "' 找不到导出 " + symbol_for_object(oe.name));
            continue;
        }
        mdpsr_factory_ctx fctx{};
        fctx.name = oe.name.c_str();
        fctx.plugin_name = m.plugin_name.c_str();
        fctx.manifest = m.rel_path.c_str();
        fctx.manifest_dir = m.dir.c_str();
        fctx.plugin = pkey;
        fctx.host = &_api;
        fctx.pool = _pool_object.get();      /* Object 工厂拿到 Pool_object */

        void* inst = reinterpret_cast<mdpsr_object_fn>(sym)(&fctx);
        if (!inst) {
            log(2, "Object '" + oe.name + "' 工厂返回空");
            continue;
        }
        const uint64_t ok = mdpsr_hash64(oe.name.c_str());
        mdpsr_ptr* p = nullptr;
        const int ar = _map.attach(MDPSR_KIND_OBJECT, ok, oe.name.c_str(), inst, &p);
        if (ar != MDPSR_OK) {
            log(1, "Object '" + oe.name + "' 注册失败 (rc=" + std::to_string(ar) + ")");
            continue;
        }
        pi->keys.push_back(ok);
        log(0, "  object " + oe.name);
    }

    /* --- 5. Handle: 描述符进 Pool_object, 由 Ptr 托管 --- */
    for (const TagEntry& he : m.handles) {
        void* sym = get_symbol(first, symbol_for_handle(he.name));
        if (!sym) {
            log(2, "Handle '" + he.name + "' 找不到导出 " + symbol_for_handle(he.name));
            continue;
        }
        void* mem = _pool_object->allocate(sizeof(mdpsr_handle_desc), alignof(mdpsr_handle_desc));
        if (!mem) {
            log(2, "Handle '" + he.name + "' 描述符分配失败");
            continue;
        }
        auto* hd = new (mem) mdpsr_handle_desc{};
        hd->fn = reinterpret_cast<mdpsr_handle_fn>(sym);
        hd->plugin = pkey;
        std::snprintf(hd->name, sizeof(hd->name), "%s", he.name.c_str());

        const uint64_t hk = mdpsr_hash64(he.name.c_str());
        mdpsr_ptr* p = nullptr;
        const int ar = _map.attach(MDPSR_KIND_HANDLE, hk, he.name.c_str(), hd, &p);
        if (ar != MDPSR_OK) {
            log(1, "Handle '" + he.name + "' 注册失败 (rc=" + std::to_string(ar) + ")");
            _pool_object->deallocate(hd, sizeof(mdpsr_handle_desc), alignof(mdpsr_handle_desc));
            continue;
        }
        pi->keys.push_back(hk);
        _handle_owner[hk] = pkey;
        /* 默认去向: 插件声明的第一条"不是固定队列"的队列; 没有就用 Queue_default。
         * 需要改绑的 (比如 core 要跑在 Queue_pluginmgr 上) 由后门自己调
         * host->queue_create + 再绑一次。 */
        {
            uint64_t qk = mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME);
            for (const TagEntry& qe : m.queues) {
                if (qe.name == MDPSR_QUEUE_PLUGINMGR_NAME) continue;
                if (qe.name == MDPSR_QUEUE_DEFAULT_NAME) continue;
                if (qe.name == MDPSR_QUEUE_WINDOWSGUI_NAME) continue;
                const uint64_t cand = mdpsr_hash64(qe.name.c_str());
                if (queue_of(cand)) { qk = cand; break; }
            }
            _handle_queue[hk] = qk;
        }
        log(0, "  handle " + he.name + "  (队列 " + std::to_string(_handle_queue[hk]) + ")");
    }

    /* --- 6. 插件自己也是一个条目, 用来记住它的资源 (给 core_Mgr 枚举) --- */
    {
        mdpsr_ptr* p = nullptr;
        const int ar = _map.attach(MDPSR_KIND_OBJECT, pkey,
                                   ("plugin:" + m.plugin_name).c_str(),
                                   pi, &p);
        if (ar == MDPSR_OK) pi->keys.push_back(pkey);
        else log(1, "插件条目注册失败 (rc=" + std::to_string(ar) + ")");
    }

    _plugins.push_back(pi);
    if (out_plugin) *out_plugin = pkey;

    log(0, "插件装载完成: " + m.plugin_name + " (" + manifest_rel + ", 资源 " +
           std::to_string(pi->keys.size()) + " 项)");
    return MDPSR_OK;
}

int Runtime::plugin_keys(uint64_t plugin, uint64_t* out, uint32_t cap, uint32_t* out_count) {
    std::lock_guard<std::mutex> lk(_plugin_mtx);
    PluginInfo* pi = find_plugin(plugin);
    if (!pi) {
        if (out_count) *out_count = 0;
        return MDPSR_ERR_PLUGIN_NOT_FOUND;
    }
    uint32_t n = 0;
    for (uint64_t k : pi->keys) {
        if (k == plugin) continue;             /* 插件条目自己不算资源 */
        if (out && n < cap) out[n] = k;
        ++n;
    }
    if (out_count) *out_count = n;
    return MDPSR_OK;
}

int Runtime::init_handle_of(uint64_t plugin, char* out, uint32_t cap) {
    if (!out || cap == 0) return MDPSR_ERR_NULLPTR;
    out[0] = '\0';
    std::lock_guard<std::mutex> lk(_plugin_mtx);
    PluginInfo* pi = find_plugin(plugin);
    if (!pi) return MDPSR_ERR_PLUGIN_NOT_FOUND;
    std::snprintf(out, cap, "%s", pi->m.init_handle.c_str());
    return MDPSR_OK;
}

int Runtime::plugin_unload(uint64_t plugin) {
    std::lock_guard<std::mutex> plk(_plugin_mtx);

    PluginInfo* pi = find_plugin(plugin);
    if (!pi) return MDPSR_ERR_PLUGIN_NOT_FOUND;

    /* --- 0. 先把 (键, Ptr) 抄下来 ---
     * 因为第 1 步会把它们全部置无效, 之后 _map.get() 就再也拿不到了,
     * 而后面取锁 / 摘表 / 释放都还需要这些 Ptr。 */
    std::vector<uint64_t> keys = pi->keys;
    std::vector<MultiLock::Item> items;
    items.reserve(keys.size());
    for (uint64_t k : keys) {
        mdpsr_ptr* p = _map.peek(k);
        if (p) items.push_back(MultiLock::Item{ k, p });
    }

    /* --- 绝对不许卸自己: 我们正跑在这个插件的队列线程上, join 自己会死锁 --- */
    for (uint64_t k : keys) {
        Queue* q = queue_of(k);
        if (q && q->running() && !q->main_thread() &&
            q->thread_id() == std::this_thread::get_id()) {
            log(2, "拒绝在自己的队列线程上卸载插件: " + pi->m.plugin_name);
            return MDPSR_ERR_PLUGIN_BUSY;
        }
    }

        /* --- 1. 全部条目置无效: 挡住所有新的 get() --- */
    for (uint64_t k : keys) _map.invalidate(k);

        /* --- 2. 按键升序一次性拿全部锁 ---
     * 顺序和 dispatch 完全一致, 所以两边不会互咬。
     * 拿不到 == 有人正在用 -> 整批回滚, 报 BUSY, 由 core_Mgr 下轮重试。 */
    MultiLock locks;
    const int lr = locks.acquire_raw(_map, items);
    if (lr == MDPSR_ERR_BUSY) {
        /* 关键: 必须把 valid 恢复回去。否则这个插件会"跑不了也卸不掉",
         * 那是最糟的状态 —— 比卸载失败糟得多。
         * 恢复是安全的: 我们还没释放任何东西, 而且锁告诉我们没人在用。 */
        for (const MultiLock::Item& it : items) {
            if (it.p) it.p->valid.store(1, std::memory_order_release);
        }
        log(1, "卸载推迟: 有资源正在使用 (" + pi->m.plugin_name + "), 已恢复有效, 下轮重试");
        return MDPSR_ERR_PLUGIN_BUSY;
    }

        /* --- 3. 停掉这个插件自己的队列 ---
     * 线程必须在 FreeLibrary 之前停干净: 否则它可能正跑在插件的代码里,
     * 而 dll 已经被卸掉。 */
    for (uint64_t k : keys) {
        Queue* q = queue_of(k);
        if (!q || q->main_thread()) continue;
        q->request_stop();
    }
    for (uint64_t k : keys) {
        Queue* q = queue_of(k);
        if (!q || q->main_thread()) continue;
        q->join();
    }

        /* --- 4. 释放资源 ---
     * 顺序: 先析构 Object (要调插件导出的 _destroy, 必须赶在 FreeLibrary 之前),
     * 再放 Handle 描述符, 最后 Queue 本体。
     * 类型用 kind_of() 确认 (attach 时登记过), 不靠猜。 */
    HMODULE first_mod = pi->modules.empty() ? nullptr
                                            : static_cast<HMODULE>(pi->modules.front());
    for (const MultiLock::Item& it : items) {
        if (!it.p || !it.p->ptr) continue;
        const int kind = _map.kind_of(it.key);

        if (kind == MDPSR_KIND_QUEUE) {
            auto* q = static_cast<Queue*>(it.p->ptr);
            {
                std::lock_guard<std::mutex> lk(_q_mtx);
                _queues.erase(std::remove(_queues.begin(), _queues.end(), q), _queues.end());
            }
            q->~Queue();
            _pool_object->deallocate(q, sizeof(Queue), alignof(Queue));
            continue;
        }
        if (kind == MDPSR_KIND_HANDLE) {
            auto* hd = static_cast<mdpsr_handle_desc*>(it.p->ptr);
            hd->~mdpsr_handle_desc();
            _pool_object->deallocate(hd, sizeof(mdpsr_handle_desc), alignof(mdpsr_handle_desc));
            continue;
        }
        if (kind == MDPSR_KIND_OBJECT) {
            /* 插件自身的条目 (ptr == PluginInfo*) 由 delete pi 负责 */
            if (it.p->ptr == static_cast<void*>(pi)) continue;

            for (const TagEntry& oe : pi->m.objects) {
                if (mdpsr_hash64(oe.name.c_str()) != it.key) continue;
                const std::string dsym = symbol_for_object(oe.name) + "_destroy";
                if (void* ds = get_symbol(first_mod, dsym)) {
                    reinterpret_cast<mdpsr_object_destroy_fn>(ds)(it.p->ptr);
                } else {
                    log(1, "Object '" + oe.name + "' 没有 _destroy 导出, 实例没法析构");
                }
                break;
            }
            continue;
        }
        /* State 的载荷在统一 Pool_state 里; 池子生命周期与进程一致,
         * 这里只把条目摘掉 (见第 5 步)。 */
    }

        /* --- 5. 摘表 --- */
    for (uint64_t k : keys) _map.detach_locked(k);
    for (uint64_t k : keys) _handle_owner.erase(k);
    for (uint64_t k : keys) _handle_queue.erase(k);

        /* --- 6. 卸模块 --- */
    for (void* mm : pi->modules) {
        if (!mm) continue;
        auto fini = reinterpret_cast<void (*)(uint64_t)>(
            ::GetProcAddress(static_cast<HMODULE>(mm), "mdpsr_module_fini"));
        if (fini) fini(plugin);
        ::FreeLibrary(static_cast<HMODULE>(mm));
    }
    pi->modules.clear();
    pi->ready = false;
    pi->keys.clear();

    _plugins.erase(std::remove(_plugins.begin(), _plugins.end(), pi), _plugins.end());
    delete pi;

    log(0, "插件已卸载: " + std::to_string(plugin));
    return MDPSR_OK;
}

} /* namespace mdpsr */
