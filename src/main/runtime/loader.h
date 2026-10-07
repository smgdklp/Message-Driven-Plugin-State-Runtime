#pragma once
/* ============================================================================
 *  mdpsr/runtime/loader.h
 *  清单 (plugin.json) 的解析结果与装载 / 卸载
 * ==========================================================================*/

#include "mdpsr/abi.h"


#include <string>
#include <vector>

namespace mdpsr {

/* 清单里的一个"标签"条目: name 是条目名, 由它推出导出符号名。 */
struct TagEntry {
    std::string name;
};

/* 一个插件的清单 */
struct Manifest {
    uint64_t    key = 0;             /* hash(rel_path), 也是这个插件在 Map 里的键 */
    std::string rel_path;            /* 相对运行时根目录 */
    std::string dir;                 /* 本清单所在目录 */
    std::string plugin_name;         /* 清单的 "name" */
    std::string init_handle;         /* 清单的 "Init" */

    std::vector<std::string> dlls;   /* 相对运行时根目录的 dll 路径 */
    std::vector<TagEntry>    states;
    std::vector<TagEntry>    objects;
    std::vector<TagEntry>    handles;
    std::vector<TagEntry>    queues;

    bool has_tag(const std::vector<TagEntry>& v, const std::string& n) const {
        for (const auto& e : v) if (e.name == n) return true;
        return false;
    }
};

/* 已加载的一个插件 */
struct PluginInfo {
    uint64_t                   key = 0;
    Manifest                   m;
    std::vector<void*>         modules;    /* HMODULE, 但头文件不引 windows.h */
    bool                       ready = false;
    bool                       unloading = false;
    std::vector<uint64_t>      keys;       /* 这个插件在 MainMap 里占的全部键 */
};

/* 清单解析: 相对运行时根目录的路径 -> Manifest。
 * 解析成功返回 MDPSR_OK 并填好 out。 */
int load_manifest(const std::string& rel_json, Manifest* out, std::string* err);

/* 导出符号名的推导规则 (与 abi.h 的命名规范一致):
 *      State  : mdpsr_state_<名称>
 *      Object : mdpsr_object_<名称>  (+ _destroy)
 *      Handle : mdpsr_handle_<名称>
 *      Queue  : mdpsr_queue_<名称>
 */
std::string symbol_for_state(const std::string& name);
std::string symbol_for_object(const std::string& name);
std::string symbol_for_handle(const std::string& name);
std::string symbol_for_queue(const std::string& name);

} /* namespace mdpsr */
