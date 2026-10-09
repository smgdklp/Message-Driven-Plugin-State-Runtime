/* ============================================================================
 *  mdpsr/runtime/loader.cpp
 *  清单解析 + 插件装载 / 卸载 / 重载
 *
 *  ==========================================================================
 *  热插拔为什么是安全的 —— 四条论证, 改这份文件之前请先读一遍
 *  ==========================================================================
 *
 *  (1) 【为什么 FreeLibrary 的时候不会有插件代码在跑】
 *      卸载的第二步会对这个插件的**全部条目**做 try_lock。全部拿到 =
 *      没有任何线程正持有其中任何一把 = 没有任何线程正处在"调用本插件的
 *      handle"中间 (分发路径必须持 handle 锁才能调 handle)。
 *      然后第三步只保留 handle 锁、放掉其余锁, 一直持有到 FreeLibrary 之后。
 *      于是:
 *        · 已经开始的调用: 在第二步就被证明不存在了;
 *        · 想开始的调用: 全部 handle 锁被我们拿着, 它进不来;
 *          等我们放锁的时候, 条目的 gen 已经是 0, 分发路径会在读 ptr 之前
 *          就判定"已失效"并回死信, 绝不会去碰已经回收的那块内存。
 *
 *  (2) 【为什么不会死锁】
 *      等待边只有三类:
 *        a. 等 _ctrl_mtx / _plg_mtx / _q_mtx 等宿主内部锁 —— 临界区都很短,
 *           而且从不嵌套逆向 (见 runtime.h 顶部的锁层次表)。
 *        b. 等 Entry::mtx —— 只在 Registry::acquire 里阻塞, 而 acquire 强制
 *           "本次调用里后借的键必须比手上所有键都大" (违反就返回 LOCK_ORDER)。
 *           于是线程手上的键是递增的, 它只会去等一个更大的键 —— 不成环。
 *        c. 等 handle 锁 —— 分发路径是"先锁 handle 再碰别的", 等 handle 的
 *           线程手上一个 Entry 锁都没有, 所以这条边不可能参与任何环。
 *      卸载路径用的是 try_lock, 永不阻塞, 因此它连"等待边"都不产生 ——
 *      最坏结果是返回 BUSY 让调用方下轮再来, 而不是卡住。
 *      ★ 唯一要小心的是"持着 _plg_mtx 去 join 线程": join 会等一个可能正
 *        阻塞在 _plg_mtx 上的线程, 那就是一个环。所以下面所有 join 都在
 *        不持有 _plg_mtx 的地方做 —— 记录永不释放, 不需要靠锁保护它的生命。
 *
 *  (3) 【为什么插件漏还锁不会把系统卡死】
 *      每个"调用插件代码"的入口都套了 CallScope。作用域退出时如果有没还的锁,
 *      宿主替它还掉并记一条 WARN。所以"某个插件忘了 release"最坏只是它自己
 *      那一轮数据可能不一致, 不会让卸载永远 BUSY。
 *
 *  (4) 【为什么卸载是真回收】
 *      每个插件一个 pmr 池, 工厂从 ctx->pool 分配。卸载最后一步 pool_delete
 *      整池销毁 —— State 载荷、Object 实例、handle 描述符、队列缓冲全在里面。
 *      v1 在这里什么都没做 (State 载荷永远留在全局池里), 那才是"卸载不回收"。
 * ==========================================================================*/
#include "loader.h"

#include "json.h"
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <windows.h>

namespace mdpsr {

/* UTF-8 -> 宽字符 */
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
 *  符号名推导
 * ==========================================================================*/
std::string sanitize_entry(const std::string& n) {
    std::string s;
    s.reserve(n.size());
    for (char c : n) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        s.push_back(ok ? c : '_');
    }
    return s;
}
std::string symbol_for_state(const std::string& n)  { return "mdpsr_state_"  + sanitize_entry(n); }
std::string symbol_for_object(const std::string& n) { return "mdpsr_object_" + sanitize_entry(n); }
std::string symbol_for_handle(const std::string& n) { return "mdpsr_handle_" + sanitize_entry(n); }
std::string symbol_for_queue(const std::string& n)  { return "mdpsr_queue_"  + sanitize_entry(n); }

static std::string dir_of(const std::string& p) {
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? std::string() : p.substr(0, k);
}

/* ==========================================================================
 *  清单解析
 * ==========================================================================*/
static int read_tags(const Json& node, std::vector<TagEntry>* out, const char* what,
                     std::string* err) {
    if (node.is_null()) return MDPSR_OK;              /* 字段可以整个不写 */
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

static int read_strings(const Json& node, std::vector<std::string>* out, const char* what,
                        std::string* err) {
    if (node.is_null()) return MDPSR_OK;
    if (node.is_string()) { out->push_back(node.as_string()); return MDPSR_OK; }
    if (!node.is_array()) {
        if (err) *err = std::string(what) + " 必须是字符串或字符串数组";
        return MDPSR_ERR_BAD_CONFIG;
    }
    for (const Json& x : node.elements()) {
        if (!x.is_string()) {
            if (err) *err = std::string(what) + " 里的元素必须是字符串";
            return MDPSR_ERR_BAD_CONFIG;
        }
        out->push_back(x.as_string());
    }
    return MDPSR_OK;
}

int load_manifest(const std::string& rel_json, Manifest* out, std::string* err) {
    if (!out) return MDPSR_ERR_NULLPTR;

    /* rel_json 是"相对运行时根目录"的路径; Runtime::init 已经把当前目录
     * 设成根目录, 所以直接打开即可 (和 v1 同一套约定)。 */
    Json j;
    if (!Json::load_file(to_w(rel_json), &j, err)) return MDPSR_ERR_PLUGIN_NOT_FOUND;
    if (!j.is_object()) {
        if (err) *err = "清单的顶层必须是对象";
        return MDPSR_ERR_BAD_CONFIG;
    }

    Manifest m;
    m.rel_path    = rel_json;
    m.dir         = dir_of(rel_json);
    m.plugin_name = j["name"].as_string();
    m.init_handle = j["Init"].as_string();
    m.kernel      = j["kernel"].as_bool(false);

    if (m.plugin_name.empty()) { if (err) *err = "清单缺少 name"; return MDPSR_ERR_BAD_CONFIG; }
    if (m.plugin_name.size() >= MDPSR_NAME_MAX) {
        if (err) *err = "name 太长 (上限 " + std::to_string(MDPSR_NAME_MAX - 1) + " 字节)";
        return MDPSR_ERR_NAME_TOO_LONG;
    }

    /* dll_path: 相对本清单所在目录 */
    {
        std::vector<std::string> raw;
        int r = read_strings(j["dll_path"], &raw, "dll_path", err);
        if (r != MDPSR_OK) return r;
        if (raw.empty()) { if (err) *err = "清单缺少 dll_path"; return MDPSR_ERR_BAD_CONFIG; }
        for (const std::string& one : raw) {
            if (one.empty()) continue;
            std::string full = m.dir.empty() ? one : (m.dir + "/" + one);
            if (full.size() >= MDPSR_PATH_MAX) {
                if (err) *err = "dll_path 太长: " + one;
                return MDPSR_ERR_NAME_TOO_LONG;
            }
            m.dlls.push_back(full);
        }
        if (m.dlls.empty()) { if (err) *err = "dll_path 是空的"; return MDPSR_ERR_BAD_CONFIG; }
    }

    int r = MDPSR_OK;
    if ((r = read_tags(j["State"],  &m.states,  "State",  err)) != MDPSR_OK) return r;
    if ((r = read_tags(j["Object"], &m.objects, "Object", err)) != MDPSR_OK) return r;
    if ((r = read_tags(j["Handle"], &m.handles, "Handle", err)) != MDPSR_OK) return r;
    if ((r = read_tags(j["Queue"],  &m.queues,  "Queue",  err)) != MDPSR_OK) return r;
    if ((r = read_strings(j["list"], &m.list, "list", err)) != MDPSR_OK) return r;

    if (m.handles.empty()) {
        if (err) *err = "清单至少要声明一个 Handle (消息入口)";
        return MDPSR_ERR_BAD_CONFIG;
    }
    if (!m.init_handle.empty() && !m.has_handle(m.init_handle)) {
        if (err) *err = "Init 指向的 Handle '" + m.init_handle + "' 没有在 Handle 里声明";
        return MDPSR_ERR_BAD_CONFIG;
    }

    /* ---- 清单自查: 空名 / 超长 / 撞名 / 去符号后撞名 ----
     * v1 只把这件事写在注释里 ("规范化之后必须仍然唯一"), 没有检查,
     * 于是 A.B 和 A_B 会静默命中其中一个。这里改成硬失败。 */
    {
        std::vector<std::string> names;
        auto collect = [&](const std::vector<TagEntry>& v, const char* what) -> int {
            for (const TagEntry& e : v) {
                if (e.name.empty()) {
                    if (err) *err = std::string(what) + " 里有空名字";
                    return MDPSR_ERR_BAD_CONFIG;
                }
                if (e.name.size() >= MDPSR_NAME_MAX) {
                    if (err) *err = std::string(what) + " 里的名字太长: " + e.name;
                    return MDPSR_ERR_NAME_TOO_LONG;
                }
                names.push_back(e.name);
            }
            return MDPSR_OK;
        };
        if ((r = collect(m.states,  "State"))  != MDPSR_OK) return r;
        if ((r = collect(m.objects, "Object")) != MDPSR_OK) return r;
        if ((r = collect(m.handles, "Handle")) != MDPSR_OK) return r;
        if ((r = collect(m.queues,  "Queue"))  != MDPSR_OK) return r;

        for (size_t i = 0; i < names.size(); ++i) {
            for (size_t k = i + 1; k < names.size(); ++k) {
                if (names[i] == names[k]) {
                    if (err) *err = "清单里条目名重复: " + names[i];
                    return MDPSR_ERR_BAD_CONFIG;
                }
                if (sanitize_entry(names[i]) == sanitize_entry(names[k])) {
                    if (err) {
                        *err = "条目名 '" + names[i] + "' 和 '" + names[k] +
                               "' 会推出同一个导出符号, 必须改名";
                    }
                    return MDPSR_ERR_BAD_CONFIG;
                }
            }
        }
    }

    *out = std::move(m);
    return MDPSR_OK;
}

/* ==========================================================================
 *  PluginRecord
 * ==========================================================================*/
void PluginRecord::rebuild_fctx(const mdpsr_host* host) {
    uint32_t n = 0;
    for (const std::string& s : m.list) {
        if (n >= MDPSR_LIST_MAX) break;
        std::snprintf(list_buf[n], MDPSR_NAME_MAX, "%s", s.c_str());
        list_ptr[n] = list_buf[n];
        ++n;
    }
    fctx = mdpsr_factory_ctx{};
    fctx.struct_size     = sizeof(mdpsr_factory_ctx);
    fctx.list_count      = n;
    fctx.list            = n ? list_ptr : nullptr;
    fctx.name            = name;
    fctx.plugin_name     = name;
    fctx.manifest_path   = manifest;
    fctx.manifest_dir    = dir;
    fctx.plugin          = key;
    fctx.plugin_gen      = gen;
    fctx.host            = host;
    fctx.pool            = reinterpret_cast<mdpsr_pool*>(pool);
}

static void* get_symbol(HMODULE mod, const std::string& sym) {
    if (!mod || sym.empty()) return nullptr;
    return reinterpret_cast<void*>(::GetProcAddress(mod, sym.c_str()));
}

/* 分步跟踪: 只为排"卡在哪一步"用, 设了 MDPSR_TRACE=1 才输出。
 * 卡死的现场是没法用调试器看的 (这台机器上没有装调试器), 所以留这么一手。 */
static bool trace_on() {
    static const bool on = [] {
        char buf[8] = { 0 };
        return ::GetEnvironmentVariableA("MDPSR_TRACE", buf, sizeof(buf)) > 0;
    }();
    return on;
}
#define MDPSR_STEP(rt_ptr, msg) do { if (trace_on()) (rt_ptr)->log(0, std::string("[trace] ") + (msg)); } while (0)

/* 造一个"某条目专属"的工厂上下文: 复制插件级 fctx, 只改 name。
 * 从插件池分配, 随卸载一起回收 —— 析构要用的就是它。 */
static mdpsr_factory_ctx* make_entry_fctx(PluginRecord* rec, const Pool* pool,
                                          const std::string& entry_name) {
    void* mem = const_cast<Pool*>(pool)->res.allocate(sizeof(mdpsr_factory_ctx),
                                                      alignof(mdpsr_factory_ctx));
    if (!mem) return nullptr;
    auto* f = new (mem) mdpsr_factory_ctx(rec->fctx);
    f->name = nullptr;   /* 下面用调用方那边的稳定字符串填 */
    return f;
}

/* ==========================================================================
 *  装载
 * ==========================================================================*/
int Runtime::plugin_install(const std::string& manifest_rel, const mdpsr_install_opts* opts,
                            uint64_t* out_plugin) {
    if (out_plugin) *out_plugin = 0;

    if (!caller_is_kernel()) {
        log(2, "plugin_install 被拒: 调用者没有 kernel 能力 (manifest 里写 \"kernel\": true)");
        return MDPSR_ERR_NO_PERM;
    }
    Tls& t = tls();
    if (t.in_host_call) {
        /* 在插件调用里再要求装卸: 那会拿 ctrl 锁拿两次, 直接拒绝而不是死等 */
        log(2, "plugin_install 被拒: 已经在一次装卸操作里了");
        return MDPSR_ERR_REENTRANT;
    }
    t.in_host_call++;
    struct LatchGuard { int* p; ~LatchGuard() { --(*p); } } latch{ &t.in_host_call };

    /* ★ 管理面这把锁: 宿主 (depth == 0) 老老实实等, 插件代码 (depth > 0, 也就是
     *   正跑在某条队列线程的 handle 里) 只 try 一次, 拿不到就 BUSY 走人。
     *
     *   为什么: 卸载路径全程持着这把锁去 join 队列线程。如果某条队列线程上的插件
     *   代码在这里**阻塞**等锁, 就形成"我等你放锁, 你等我退出"的环 —— 这是整个
     *   运行时唯一还能闭合的等待环。改成不阻塞之后, 那条线程会立刻回到队列循环
     *   顶部, 看到 _stop 然后退出, join 正常返回; 调用方拿到 BUSY, 下轮再来
     *   (内核插件 sysmgr 就是这么重试的)。 */
    std::unique_lock<std::mutex> ctrl(_ctrl_mtx, std::defer_lock);
    if (t.depth > 0) {
        if (!ctrl.try_lock()) {
            log(3, "plugin_install 推迟: 管理面正忙 (调用者是插件代码, 不阻塞)");
            return MDPSR_ERR_BUSY;
        }
    } else {
        ctrl.lock();
    }
    t_manage.fetch_add(1, std::memory_order_relaxed);

    uint32_t flags = MDPSR_INSTALL_SYNC_INIT;
    if (opts && opts->struct_size >= sizeof(mdpsr_install_opts)) {
        flags = opts->flags ? opts->flags : MDPSR_INSTALL_SYNC_INIT;
    }

    /* ---- 1. 清单 ---- */
    Manifest m;
    std::string err;
    int rc = load_manifest(manifest_rel, &m, &err);
    if (rc != MDPSR_OK) {
        log(2, "清单解析失败: " + manifest_rel + " (" + err + ")");
        return rc;
    }

    const uint64_t pkey = mdpsr_plugin_key(m.plugin_name.c_str());

    /* ---- 2. 记录 ---- */
    PluginRecord* rec = nullptr;
    {
        std::lock_guard<std::mutex> lk(_plg_mtx);
        auto it = _records.find(pkey);
        if (it == _records.end()) {
            auto* r = new PluginRecord();
            r->key = pkey;
            std::snprintf(r->name, sizeof(r->name), "%s", m.plugin_name.c_str());
            _records.emplace(pkey, r);
            rec = r;
        } else {
            rec = it->second;
        }
    }
    if (rec->status.load(std::memory_order_acquire) == MDPSR_PLUGIN_READY) {
        log(1, "插件已经装着了, 跳过: " + m.plugin_name);
        return MDPSR_ERR_ALREADY_EXISTS;
    }
    if (rec->status.load(std::memory_order_acquire) != MDPSR_PLUGIN_EMPTY) {
        log(2, "插件处于中间状态, 拒绝装载: " + m.plugin_name);
        return MDPSR_ERR_NOT_READY;
    }

    /* ---- 3. 条目名全局唯一 (v1 只靠自觉, 这里真的查) ---- */
    {
        std::vector<std::string> all = { };
        auto add_all = [&](const std::vector<TagEntry>& v) {
            for (const TagEntry& e : v) all.push_back(e.name);
        };
        add_all(m.states); add_all(m.objects); add_all(m.handles); add_all(m.queues);
        for (const std::string& n : all) {
            /* 固定队列是宿主建的, 清单里写它们的名字表示"我要用那一条",
             * 不是"我再建一个同名条目" —— 所以不算撞名。 */
            if (n == MDPSR_QUEUE_SYS_NAME || n == MDPSR_QUEUE_DEFAULT_NAME) {
                continue;
            }
            if (_reg.name_taken(n.c_str(), 0)) {
                log(2, "条目名 '" + n + "' 已经被别的插件占用了 (条目名必须全局唯一)");
                return MDPSR_ERR_ALREADY_EXISTS;
            }
        }
    }

    /* ---- 4. 进入 LOADING ---- */
    rec->status.store(MDPSR_PLUGIN_LOADING, std::memory_order_release);
    rec->m = std::move(m);
    rec->kernel = rec->m.kernel;
    rec->gen += 1;
    std::snprintf(rec->manifest, sizeof(rec->manifest), "%s", manifest_rel.c_str());
    std::snprintf(rec->dir, sizeof(rec->dir), "%s", rec->m.dir.c_str());
    rec->pool = _reg.pool_new(pkey);
    if (!rec->pool) {
        rec->status.store(MDPSR_PLUGIN_FAILED, std::memory_order_release);
        return MDPSR_ERR_NO_SPACE;
    }
    rec->rebuild_fctx(&_api);
    rec->entries.clear();
    rec->queue_keys.clear();

    auto fail = [&](int code, const std::string& why) -> int {
        log(2, "装载 " + rec->m.plugin_name + " 失败: " + why);
        rec->fail_count++;
        /* 回滚: 把已经挂上的条目摘掉, 卸模块, 销毁池。
         *
         * ★ 这里可以放心 pool_delete 的原因不是"我们抢到了锁", 而是**这些条目
         *   从来没上线过** (attach 时 published=false): 任何插件都借不到一个还没
         *   READY 的东西, 所以不可能有人手里攥着即将被回收的载荷。抢锁这一步现在
         *   只是防御性动作 (也顺手覆盖"失败发生在 publish 之后"这种未来改动)。 */
        Registry::Hold hold;
        for (int i = 0; i < 50; ++i) {           /* 这几项没上线, 正常一次就拿到 */
            if (_reg.hold_all(rec->entries, &hold) == MDPSR_OK) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (uint64_t k : rec->entries) _reg.detach(k);
        forget_handle_routes(rec->entries);
        hold.release();
        for (uint64_t qk : rec->queue_keys) drop_queue(qk);
        rec->queue_keys.clear();
        for (void* mod : rec->modules) if (mod) ::FreeLibrary(static_cast<HMODULE>(mod));
        rec->modules.clear();
        if (rec->pool) { _reg.pool_delete(rec->pool); rec->pool = nullptr; }
        rec->entries.clear();
        rec->status.store(MDPSR_PLUGIN_FAILED, std::memory_order_release);
        /* FAILED -> EMPTY: 允许下一次重试 */
        rec->status.store(MDPSR_PLUGIN_EMPTY, std::memory_order_release);
        return code;
    };

    /* ---- 5. 装 dll ---- */
    for (const std::string& d : rec->m.dlls) {
        const std::wstring full = resolve(d);
        HMODULE mod = ::LoadLibraryW(full.c_str());
        if (!mod) mod = ::LoadLibraryW(to_w(d).c_str());
        if (!mod) {
            return fail(MDPSR_ERR_PLUGIN_LOAD, "LoadLibrary 失败 (" +
                        std::to_string(::GetLastError()) + "): " + d);
        }
        auto abi = reinterpret_cast<uint32_t (*)(void)>(::GetProcAddress(mod, "mdpsr_abi_version"));
        if (!abi) {
            ::FreeLibrary(mod);
            return fail(MDPSR_ERR_PLUGIN_EXPORT, "没有导出 mdpsr_abi_version: " + d);
        }
        const uint32_t v = abi();
        if (v != MDPSR_ABI_VERSION) {
            ::FreeLibrary(mod);
            return fail(MDPSR_ERR_PLUGIN_ABI, "ABI 不匹配 (插件 " + std::to_string(v) +
                        " != 宿主 " + std::to_string(MDPSR_ABI_VERSION) + "): " + d);
        }
        rec->modules.push_back(mod);

        if (auto init = reinterpret_cast<mdpsr_module_init_fn>(
                ::GetProcAddress(mod, "mdpsr_module_init"))) {
            int r0 = MDPSR_OK;
            {
                CallScope scope(pkey, 0, "module_init");
                try { r0 = init(&rec->fctx); }
                catch (...) { r0 = MDPSR_ERR_CREATE_FAILED; }
                if (scope.leaked()) {
                    t_leaked.fetch_add(scope.leaked(), std::memory_order_relaxed);
                    log(1, std::string("module_init 漏还了 ") +
                           std::to_string(scope.leaked()) + " 个资源 (宿主已兜底)");
                }
            }
            if (r0 != MDPSR_OK) {
                return fail(r0, "mdpsr_module_init 返回 " + std::to_string(r0));
            }
        }
        log(0, "  dll    " + d);
    }

    HMODULE first = rec->modules.empty() ? nullptr : static_cast<HMODULE>(rec->modules.front());

    /* ---- 6. 队列 (清单里声明的, 固定队列跳过) ---- */
    for (const TagEntry& qe : rec->m.queues) {
        if (qe.name == MDPSR_QUEUE_SYS_NAME || qe.name == MDPSR_QUEUE_DEFAULT_NAME) {
            continue;
        }
        Queue::Desc desc;
        if (void* sym = get_symbol(first, symbol_for_queue(qe.name))) {
            mdpsr_queue_desc qd{};
            qd.struct_size = sizeof(qd);
            mdpsr_factory_ctx* f = make_entry_fctx(rec, rec->pool, qe.name);
            if (f) f->name = qe.name.c_str();
            int qr = MDPSR_OK;
            {
                CallScope scope(pkey, 0, "queue factory");
                try { qr = reinterpret_cast<mdpsr_queue_fn>(sym)(f ? f : &rec->fctx, &qd); }
                catch (...) { qr = MDPSR_ERR_CREATE_FAILED; }
                if (scope.leaked()) t_leaked.fetch_add(scope.leaked(), std::memory_order_relaxed);
            }
            if (qr != MDPSR_OK) {
                log(1, "队列 '" + qe.name + "' 的工厂拒绝建立 (" + std::to_string(qr) + "), 跳过");
                continue;
            }
            if (qd.capacity) desc.capacity = qd.capacity;
            desc.pace_ms = qd.pace_ms;
        }
        uint64_t qk = 0;
        const int cr = queue_create(qe.name.c_str(), desc, rec->pool, pkey, &qk);
        if (cr != MDPSR_OK) {
            return fail(cr, "建立队列 '" + qe.name + "' 失败 (" + std::to_string(cr) + ")");
        }
        rec->queue_keys.push_back(qk);
        rec->entries.push_back(qk);
        log(0, "  queue  " + qe.name);
    }

    /* ---- 7. State ---- */
    for (const TagEntry& se : rec->m.states) {
        void* sym = get_symbol(first, symbol_for_state(se.name));
        if (!sym) return fail(MDPSR_ERR_PLUGIN_EXPORT, "State '" + se.name + "' 找不到导出 " +
                              symbol_for_state(se.name));
        mdpsr_factory_ctx* f = make_entry_fctx(rec, rec->pool, se.name);
        if (!f) return fail(MDPSR_ERR_NO_SPACE, "State '" + se.name + "' 上下文分配失败");
        f->name = se.name.c_str();

        void* payload = nullptr;
        {
            CallScope scope(pkey, 0, "state factory");
            try { payload = reinterpret_cast<mdpsr_state_fn>(sym)(f); }
            catch (...) { payload = nullptr; }
            if (scope.leaked()) t_leaked.fetch_add(scope.leaked(), std::memory_order_relaxed);
        }
        if (!payload) return fail(MDPSR_ERR_CREATE_FAILED, "State '" + se.name + "' 工厂返回空");

        auto destroy = reinterpret_cast<mdpsr_destroy_fn>(
            get_symbol(first, symbol_for_state(se.name) + "_destroy"));

        const uint64_t sk = mdpsr_hash64(se.name.c_str());
        Entry* e = nullptr;
        /* published = false: 挂上去了, 但"上线"要等整个插件 READY —— 见 Entry::published */
        const int ar = _reg.attach(sk, MDPSR_KIND_STATE, pkey, rec->gen, se.name.c_str(),
                                   payload, destroy, f, false, &e);
        if (ar != MDPSR_OK) return fail(ar, "State '" + se.name + "' 注册失败");
        rec->entries.push_back(sk);
        log(0, "  state  " + se.name);
    }

    /* ---- 8. Object ---- */
    for (const TagEntry& oe : rec->m.objects) {
        void* sym = get_symbol(first, symbol_for_object(oe.name));
        if (!sym) return fail(MDPSR_ERR_PLUGIN_EXPORT, "Object '" + oe.name + "' 找不到导出 " +
                              symbol_for_object(oe.name));
        mdpsr_factory_ctx* f = make_entry_fctx(rec, rec->pool, oe.name);
        if (!f) return fail(MDPSR_ERR_NO_SPACE, "Object '" + oe.name + "' 上下文分配失败");
        f->name = oe.name.c_str();

        void* inst = nullptr;
        {
            CallScope scope(pkey, 0, "object factory");
            try { inst = reinterpret_cast<mdpsr_object_fn>(sym)(f); }
            catch (...) { inst = nullptr; }
            if (scope.leaked()) t_leaked.fetch_add(scope.leaked(), std::memory_order_relaxed);
        }
        if (!inst) return fail(MDPSR_ERR_CREATE_FAILED, "Object '" + oe.name + "' 工厂返回空");

        auto destroy = reinterpret_cast<mdpsr_destroy_fn>(
            get_symbol(first, symbol_for_object(oe.name) + "_destroy"));

        const uint64_t ok = mdpsr_hash64(oe.name.c_str());
        Entry* e = nullptr;
        const int ar = _reg.attach(ok, MDPSR_KIND_OBJECT, pkey, rec->gen, oe.name.c_str(),
                                   inst, destroy, f, false, &e);
        if (ar != MDPSR_OK) return fail(ar, "Object '" + oe.name + "' 注册失败");
        rec->entries.push_back(ok);
        log(0, "  object " + oe.name);
    }

    /* ---- 9. Handle ---- */
    for (const TagEntry& he : rec->m.handles) {
        void* sym = get_symbol(first, symbol_for_handle(he.name));
        if (!sym) return fail(MDPSR_ERR_PLUGIN_EXPORT, "Handle '" + he.name + "' 找不到导出 " +
                              symbol_for_handle(he.name));

        void* mem = rec->pool->res.allocate(sizeof(HandleDesc), alignof(HandleDesc));
        if (!mem) return fail(MDPSR_ERR_NO_SPACE, "Handle '" + he.name + "' 描述符分配失败");
        auto* hd = new (mem) HandleDesc();
        hd->fn = reinterpret_cast<mdpsr_handle_fn>(sym);
        hd->plugin = pkey;
        hd->plugin_gen = rec->gen;
        std::snprintf(hd->name, sizeof(hd->name), "%s", he.name.c_str());

        const uint64_t hk = mdpsr_hash64(he.name.c_str());
        Entry* e = nullptr;
        const int ar = _reg.attach(hk, MDPSR_KIND_HANDLE, pkey, rec->gen, he.name.c_str(),
                                   hd, nullptr, nullptr, false, &e);
        if (ar != MDPSR_OK) return fail(ar, "Handle '" + he.name + "' 注册失败");
        rec->entries.push_back(hk);

        /* 默认去向: 本插件声明的第一条真的存在的队列 (三条固定队列也算),
         * 一条都没有就用 Queue_default。规则简单 = 可预测。 */
        uint64_t qk = mdpsr_hash64(MDPSR_QUEUE_DEFAULT_NAME);
        for (const TagEntry& qe : rec->m.queues) {
            const uint64_t cand = mdpsr_hash64(qe.name.c_str());
            if (queue_exists(cand)) { qk = cand; break; }
        }
        hd->queue_key = qk;
        set_handle_route(hk, qk);
        log(0, "  handle " + he.name);
    }

    /* ---- 10. 上线 ---- */
    rec->install_count++;
    rec->last_install_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    /* ★ 先上线, 再宣布 READY。
     *   这两个动作之间不能有"别人已经看到 READY 但资源还借不到"的窗口 ——
     *   先 publish 正好把这个窗口挤没 (反过来的顺序会让轮询 gen_of 的人错过)。 */
    _reg.publish(rec->entries);
    rec->status.store(MDPSR_PLUGIN_READY, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(_plg_mtx);
        _load_order.push_back(rec);
    }
    if (out_plugin) *out_plugin = pkey;
    log(0, "插件装载完成: " + rec->m.plugin_name + " (gen " + std::to_string(rec->gen) +
           ", 条目 " + std::to_string(rec->entries.size()) + " 项, " + manifest_rel + ")");

    /* ---- 11. 点火 ---- */
    if (!(flags & MDPSR_INSTALL_NO_INIT)) {
        const int ir = run_init_handle(rec, (flags & MDPSR_INSTALL_SYNC_INIT) != 0);
        if (ir != MDPSR_OK) {
            log(1, "Init handle 点火返回 " + std::to_string(ir) + " (" + rec->m.plugin_name + ")");
        }
    }
    return MDPSR_OK;
}

/* ==========================================================================
 *  初始化点火
 * ==========================================================================*/
int Runtime::run_init_handle(PluginRecord* rec, bool sync) {
    if (!rec || rec->m.init_handle.empty()) return MDPSR_OK;
    const uint64_t hk = mdpsr_hash64(rec->m.init_handle.c_str());

    Entry* e = _reg.find(hk);
    if (!e || e->kind != MDPSR_KIND_HANDLE || e->gen.load(std::memory_order_acquire) == 0) {
        return MDPSR_ERR_NOT_FOUND;
    }

    int32_t cmd = MDPSR_CMD_INIT;
    uint8_t body[4];
    std::memcpy(body, &cmd, 4);

    if (!sync) {
        /* 异步: 走它自己的队列, 在它自己的线程上跑 (和普通消息一样) */
        return emit(hk, 0, MDPSR_CMD_INIT, body, 4);
    }

    /* 同步: 就在调用者线程上跑一次。
     * 这样 plugin_install 返回的时候, 插件已经初始化完了 —— 自测和内核都
     * 好写很多。代价是 Init handle 必须做到线程无关: 不要在 cmd=0 里假设
     * "我在自己的队列线程上"; 需要线程亲和的初始化请投一条 cmd=1 给自己。
     * 这是一条明确的契约, 写在 doc/插件规范.md 里。 */
    /* 有界等待: 同步初始化会在调用者线程上跑插件代码, 而队列线程可能正在
     * 分发同一个 handle。等满了上界就报 BUSY, 绝不无限期卡住装载者线程。 */
    const int lr = _reg.lock_bounded(e);
    if (lr != MDPSR_OK) return lr;
    const uint32_t g = e->gen.load(std::memory_order_acquire);
    if (g == 0) { e->mtx.unlock(); return MDPSR_ERR_ENTRY_INVALID; }

    auto* hd = static_cast<HandleDesc*>(e->ptr.load(std::memory_order_acquire));
    if (!hd || !hd->fn) { e->mtx.unlock(); return MDPSR_ERR_BAD_STATE; }

    mdpsr_ctx ctx{};
    ctx.handle = hk;
    ctx.plugin = rec->key;
    ctx.plugin_gen = g;
    ctx.host = &_api;
    mdpsr_msg msg{};
    msg.dst = hk;
    msg.src = 0;
    msg.cmd = MDPSR_CMD_INIT;
    msg.len = static_cast<int32_t>(MDPSR_MSG_HEADER + 4);

    int rc = MDPSR_OK;
    {
        CallScope scope(rec->key, hk, e->name);
        try { rc = hd->fn(&msg, body, 4, &ctx); }
        catch (const std::exception& ex) {
            log(2, std::string("Init handle '") + e->name + "' 抛异常: " + ex.what());
            rc = MDPSR_ERR_BAD_STATE;
        } catch (...) {
            log(2, std::string("Init handle '") + e->name + "' 抛未知异常");
            rc = MDPSR_ERR_BAD_STATE;
        }
        if (scope.leaked()) {
            t_leaked.fetch_add(scope.leaked(), std::memory_order_relaxed);
            log(1, std::string("Init handle '") + e->name + "' 漏还了 " +
                   std::to_string(scope.leaked()) + " 个资源 (宿主已兜底)");
        }
    }
    e->mtx.unlock();
    return rc;
}

/* ==========================================================================
 *  卸载
 * ==========================================================================*/
int Runtime::plugin_uninstall(uint64_t plugin, uint32_t flags) {
    if (!caller_is_kernel()) {
        log(2, "plugin_uninstall 被拒: 调用者没有 kernel 能力");
        return MDPSR_ERR_NO_PERM;
    }
    Tls& t = tls();
    if (t.in_host_call) return MDPSR_ERR_REENTRANT;
    if (t.cur_plugin == plugin) {
        /* 自己卸自己 = 一边跑自己的代码一边把自己的 dll 卸掉 */
        log(2, "拒绝: 插件不能卸载自己 (" + key_name(plugin) + ")");
        return MDPSR_ERR_REENTRANT;
    }

    t.in_host_call++;
    struct LatchGuard { int* p; ~LatchGuard() { --(*p); } } latch{ &t.in_host_call };

    /* 和 plugin_install 同一条纪律: 插件代码不阻塞等管理面 (否则和 join 成环)。 */
    std::unique_lock<std::mutex> ctrl(_ctrl_mtx, std::defer_lock);
    if (t.depth > 0) {
        if (!ctrl.try_lock()) {
            log(3, "plugin_uninstall 推迟: 管理面正忙 (调用者是插件代码, 不阻塞)");
            return MDPSR_ERR_BUSY;
        }
    } else {
        ctrl.lock();
    }
    t_manage.fetch_add(1, std::memory_order_relaxed);

    PluginRecord* rec = record_of(plugin);
    if (!rec) return MDPSR_ERR_PLUGIN_NOT_FOUND;
    if (rec->status.load(std::memory_order_acquire) != MDPSR_PLUGIN_READY) {
        return MDPSR_ERR_NOT_READY;
    }

    const bool try_only = (flags & MDPSR_UNINSTALL_TRY_ONLY) != 0;

    /* ★ 状态翻转和快照必须在同一个临界区里完成:
     * 这样 queue_create (插件运行时自己建队列) 要么已经把新队列登记进了
     * entries (于是我们看到它), 要么看到 UNLOADING 直接失败 ——
     * 不会出现"队列建出来了却没人收"的漏网之鱼。 */
    std::vector<uint64_t> keys, qkeys;
    {
        std::lock_guard<std::mutex> lk(_plg_mtx);
        if (rec->status.load(std::memory_order_acquire) != MDPSR_PLUGIN_READY) {
            return MDPSR_ERR_NOT_READY;
        }
        rec->status.store(MDPSR_PLUGIN_UNLOADING, std::memory_order_release);
        keys = rec->entries;
        qkeys = rec->queue_keys;
    }

    /* ---- 1. 先独占全部 handle ----
     * ★ 顺序很讲究: 先拿 handle 锁, 再置无效, 最后拿其余条目。
     *
     * handle 锁一拿到, 新的分发就进不来了。而这一步是在**什么都还没改**的前提下
     * 做的 —— 万一拿不到 (有消息正在跑), 直接返回 BUSY 就完事, 在途消息看不到
     * 任何"暂时失效"的资源, 不会有任何一次 handle 调用观察到"这个插件卸了一半"。
     *
     * v1 那种"先全部置无效再抢锁"的写法会留下一个窗口: 正在跑的 handle 会发现
     * 自己的 State 突然没了, 于是返回一个莫名其妙的错误码。 */
    Registry::Hold handles;
    int rc = _reg.hold_kind(keys, MDPSR_KIND_HANDLE, &handles);
    if (rc != MDPSR_OK && !try_only) {
        for (int i = 0; i < 400 && rc != MDPSR_OK; ++i) {
            if (i < 50) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            rc = _reg.hold_kind(keys, MDPSR_KIND_HANDLE, &handles);
        }
    }
    if (rc != MDPSR_OK) {
        rec->status.store(MDPSR_PLUGIN_READY, std::memory_order_release);
        log(1, "卸载推迟: " + std::string(rec->name) + " 还有 handle 正在跑");
        return MDPSR_ERR_PLUGIN_BUSY;
    }

    /* ---- 2. 全部条目置无效: 挡住所有新的 acquire ---- */
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) _reg.invalidate(*it);

    /* ---- 3. 再独占其余条目 (State / Object / Queue) ----
     * 走到这里 handle 已经锁死, 所以没人能开始新的调用; 万一某个 State 正被
     * 别的插件借走, 我们就恢复 gen、放掉 handle 锁, 干净地返回 BUSY。 */
    Registry::Hold rest;
    rc = _reg.hold_except_kind(keys, MDPSR_KIND_HANDLE, &rest);
    if (rc != MDPSR_OK && !try_only) {
        for (int i = 0; i < 400 && rc != MDPSR_OK; ++i) {
            if (i < 50) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            rc = _reg.hold_except_kind(keys, MDPSR_KIND_HANDLE, &rest);
        }
    }
    if (rc != MDPSR_OK) {
        for (uint64_t k : keys) _reg.revalidate(k, rec->gen);
        handles.release();
        rec->status.store(MDPSR_PLUGIN_READY, std::memory_order_release);
        log(1, "卸载推迟: " + std::string(rec->name) + " 有资源正在被使用");
        return MDPSR_ERR_PLUGIN_BUSY;
    }

    /* ---- 3b. 放掉 State/Object 的锁, 只留 handle 锁 ----
     * 它们已经 gen=0, 谁也借不到了, 所以放掉是安全的; 而必须放掉是因为析构函数
     * 可能要去借别的插件的东西, 拿着一堆锁去借会违反"按键升序"。
     * handle 锁留到最后: 它保证"再也没有别的线程能走进这个插件的代码"。 */
    rest.release();

    /* ---- 3. 停掉并回收它自己的队列 ----
     * drop_queue 内部自己分三步做 (停 -> join -> 独占摘除), 全程不持有
     * _plg_mtx, 所以 join 不会和"等 _plg_mtx 的线程"成环。 */
    for (uint64_t qk : qkeys) {
        MDPSR_STEP(this, "3 drop_queue " + key_name(qk));
        drop_queue(qk);
    }
    MDPSR_STEP(this, "3 队列收完");

    /* ---- 4. 只留 handle 锁这一条已经在上面做过了 (rest.release()) ---- */
    MDPSR_STEP(this, "4 只留 handle 锁");

    /* ---- 5. 析构 (跑插件代码, 必须在 FreeLibrary 之前) ---- */
    for (uint64_t k : keys) {
        Entry* e = _reg.find(k);
        if (!e || !e->destroy) continue;
        void* inst = e->ptr.load(std::memory_order_acquire);
        if (!inst) continue;
        {
            CallScope scope(rec->key, 0, e->name);
            try { e->destroy(inst, e->fctx); }
            catch (...) { log(2, std::string("析构 '") + e->name + "' 抛异常"); }
            if (scope.leaked()) {
                t_leaked.fetch_add(scope.leaked(), std::memory_order_relaxed);
                log(1, std::string("析构 '") + e->name + "' 漏还了 " +
                       std::to_string(scope.leaked()) + " 个资源 (宿主已兜底)");
            }
        }
    }

    /* ---- 6. 摘表 ---- */
    MDPSR_STEP(this, "5 析构完毕, 开始摘表");
    for (uint64_t k : keys) _reg.detach(k);
    forget_handle_routes(keys);
    MDPSR_STEP(this, "6 摘表完毕");

    /* ---- 7. 卸模块 (此时 handle 锁还握着, 插件代码进不来) ---- */
    for (void* mod : rec->modules) {
        if (!mod) continue;
        auto fini = reinterpret_cast<mdpsr_module_fini_fn>(
            ::GetProcAddress(static_cast<HMODULE>(mod), "mdpsr_module_fini"));
        if (fini) {
            CallScope scope(rec->key, 0, "module_fini");
            try { fini(); } catch (...) {}
            if (scope.leaked()) t_leaked.fetch_add(scope.leaked(), std::memory_order_relaxed);
        }
        ::FreeLibrary(static_cast<HMODULE>(mod));
    }
    rec->modules.clear();

    /* ---- 8. 整池回收 ★ 卸载在这里才真的把内存还回去 ---- */
    MDPSR_STEP(this, "7 模块卸完, 开始回收池");
    if (rec->pool) {
        _reg.pool_delete(rec->pool);
        rec->pool = nullptr;
    }

    rec->entries.clear();
    rec->queue_keys.clear();
    rec->status.store(MDPSR_PLUGIN_EMPTY, std::memory_order_release);

    /* ---- 9. 最后才放 handle 锁 ---- */
    handles.release();
    MDPSR_STEP(this, "9 handle 锁已放, 卸载结束");

    {
        std::lock_guard<std::mutex> lk(_plg_mtx);
        _load_order.erase(std::remove(_load_order.begin(), _load_order.end(), rec),
                          _load_order.end());
    }

    log(0, "插件已卸载: " + std::string(rec->name) + " (gen " + std::to_string(rec->gen) + ")");
    return MDPSR_OK;
}

/* ==========================================================================
 *  重载 = 卸载 + 装载
 * ==========================================================================*/
int Runtime::plugin_reload(uint64_t plugin, const mdpsr_install_opts* opts) {
    if (!caller_is_kernel()) return MDPSR_ERR_NO_PERM;

    PluginRecord* rec = record_of(plugin);
    if (!rec) return MDPSR_ERR_PLUGIN_NOT_FOUND;
    std::string rel;
    {
        std::lock_guard<std::mutex> lk(_plg_mtx);
        rel = rec->manifest;
    }
    if (rel.empty()) return MDPSR_ERR_NOT_READY;

    /* 注意: reload 不能整场持有 _ctrl_mtx —— uninstall/install 自己会去拿。
     * 两端各自串行化就够了: 卸完到装回之间, 别人可能插进来也很难受, 但那
     * 只会表现为一次 ALREADY_EXISTS, 不会破坏一致性。 */
    const int rc = plugin_uninstall(plugin, 0);
    if (rc != MDPSR_OK) {
        log(2, "重载失败 (卸载这一步): " + std::string(rec->name) + " rc=" + std::to_string(rc));
        return rc;
    }
    return plugin_install(rel, opts, nullptr);
}

/* ==========================================================================
 *  查询
 * ==========================================================================*/
PluginRecord* Runtime::record_of(uint64_t key) {
    std::lock_guard<std::mutex> lk(_plg_mtx);
    auto it = _records.find(key);
    return it == _records.end() ? nullptr : it->second;
}

bool Runtime::caller_is_kernel() {
    const uint64_t cp = tls().cur_plugin;
    if (cp == 0) return true;            /* 宿主自己 */
    PluginRecord* rec = record_of(cp);
    return rec && rec->kernel && rec->ready();
}

int Runtime::plugin_find(const char* name, uint64_t* out_plugin, uint32_t* out_gen) {
    if (!name || !*name) return MDPSR_ERR_NULLPTR;
    PluginRecord* rec = record_of(mdpsr_plugin_key(name));
    if (!rec || !rec->ready()) return MDPSR_ERR_PLUGIN_NOT_FOUND;
    if (out_plugin) *out_plugin = rec->key;
    if (out_gen) *out_gen = rec->gen;
    return MDPSR_OK;
}

int Runtime::plugin_info(uint64_t plugin, mdpsr_plugin_info* out) {
    if (!out) return MDPSR_ERR_NULLPTR;
    PluginRecord* rec = record_of(plugin);
    if (!rec) return MDPSR_ERR_PLUGIN_NOT_FOUND;
    std::memset(out, 0, sizeof(*out));
    out->struct_size   = sizeof(*out);
    out->status        = static_cast<uint32_t>(rec->status.load(std::memory_order_acquire));
    out->key           = rec->key;
    out->gen           = rec->gen;
    out->entry_count   = static_cast<uint32_t>(rec->entries.size());
    out->module_count  = static_cast<uint32_t>(rec->modules.size());
    out->install_count = rec->install_count;
    out->fail_count    = rec->fail_count;
    std::snprintf(out->name, sizeof(out->name), "%s", rec->name);
    std::snprintf(out->manifest, sizeof(out->manifest), "%s", rec->manifest);
    return MDPSR_OK;
}

int Runtime::plugin_list(uint64_t* out, uint32_t cap, uint32_t* out_count) {
    std::lock_guard<std::mutex> lk(_plg_mtx);
    uint32_t n = 0;
    for (PluginRecord* rec : _load_order) {
        if (!rec->ready()) continue;
        if (out && n < cap) out[n] = rec->key;
        ++n;
    }
    if (out_count) *out_count = n;
    return MDPSR_OK;
}

std::string Runtime::key_name(uint64_t key) {
    PluginRecord* rec = record_of(key);
    if (rec && rec->name[0]) return rec->name;
    char buf[MDPSR_NAME_MAX] = { 0 };
    if (_reg.name_of(key, buf, sizeof(buf)) == MDPSR_OK && buf[0]) return buf;
    char hex[32];
    std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(key));
    return hex;
}

} /* namespace mdpsr */
