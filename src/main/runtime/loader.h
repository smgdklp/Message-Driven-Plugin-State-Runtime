#pragma once
/* ============================================================================
 *  mdpsr/runtime/loader.h
 *  清单 (plugin.json) 的解析结果、插件记录、装载/卸载
 * ==========================================================================*/

#include "mdpsr/abi.h"
#include "registry.h"

#include <atomic>
#include <string>
#include <vector>

namespace mdpsr {

/* 清单里的一个"标签"条目: name 是条目名, 导出符号名由它推出来。 */
struct TagEntry {
    std::string name;
};

/* 一个插件的清单 */
struct Manifest {
    std::string rel_path;      /* 相对运行时根目录 */
    std::string dir;           /* 本清单所在目录 */
    std::string plugin_name;
    std::string init_handle;   /* "Init": 谁的 cmd=0 是初始化入口 */
    bool        kernel = false;/* "kernel": 有没有装卸别人的权限 */
    std::vector<std::string> dlls;
    std::vector<TagEntry>    states;
    std::vector<TagEntry>    objects;
    std::vector<TagEntry>    handles;
    std::vector<TagEntry>    queues;
    std::vector<std::string> list;   /* "list": 给工厂的附加参数 */

    bool has_handle(const std::string& n) const {
        for (const auto& e : handles) if (e.name == n) return true;
        return false;
    }
};

/* handle 描述符 —— 它自己就是托管资源, 从插件池里分配, 随卸载整池回收 */
struct HandleDesc {
    mdpsr_handle_fn fn = nullptr;
    uint64_t        plugin = 0;
    uint32_t        plugin_gen = 0;
    uint64_t        queue_key = 0;      /* 默认跑在哪条队列上 */
    char            name[MDPSR_NAME_MAX] = { 0 };
};

/* ==========================================================================
 *  插件记录 —— 永不释放 (和 Entry 一个道理)
 *
 *  热插拔会反复用到同一个插件键, 如果记录跟着卸载一起 delete, 任何"迟到的
 *  指针"都会踩到已释放内存。所以记录活到进程结束, 只把 status/gen 和里面
 *  的资源清干净。代价是每个用过的插件名占一份记录, 可以忽略。
 * ==========================================================================*/
struct PluginRecord {
    uint64_t         key = 0;                       /* mdpsr_plugin_key(name) */
    uint32_t         gen = 0;                       /* 每次成功装载 +1 */
    char             name[MDPSR_NAME_MAX] = { 0 };
    char             manifest[MDPSR_PATH_MAX] = { 0 };
    char             dir[MDPSR_PATH_MAX] = { 0 };
    std::atomic<int> status{ MDPSR_PLUGIN_EMPTY };
    bool             kernel = false;

    Manifest              m;
    std::vector<void*>    modules;                  /* HMODULE */
    std::vector<uint64_t> entries;                  /* 本次装载占用的全部键 */
    std::vector<uint64_t> queue_keys;               /* 其中属于 QUEUE 的那些 */
    Pool*                 pool = nullptr;           /* 本插件专属池 */
    uint32_t              install_count = 0;
    uint32_t              fail_count = 0;
    uint64_t              last_install_ms = 0;

    /* 给 mdpsr_factory_ctx 用的稳定存储 (地址一辈子不变) */
    char               list_buf[MDPSR_LIST_MAX][MDPSR_NAME_MAX];
    const char*        list_ptr[MDPSR_LIST_MAX];
    mdpsr_factory_ctx  fctx{};

    bool ready() const { return status.load(std::memory_order_acquire) == MDPSR_PLUGIN_READY; }

    /* 重建 fctx: 装载时调用一次, 供工厂和析构使用 */
    void rebuild_fctx(const mdpsr_host* host);
};

/* 清单解析: 相对运行时根目录的路径 -> Manifest。
 * 解析成功返回 MDPSR_OK 并填好 out; 失败时 err 里是给人看的原因。 */
int load_manifest(const std::string& rel_json, Manifest* out, std::string* err);

/* 条目名 -> 导出符号名。非 [A-Za-z0-9_] 的字符统一变成 '_'。 */
std::string sanitize_entry(const std::string& n);
std::string symbol_for_state(const std::string& n);
std::string symbol_for_object(const std::string& n);
std::string symbol_for_handle(const std::string& n);
std::string symbol_for_queue(const std::string& n);

} /* namespace mdpsr */
