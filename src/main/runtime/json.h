#pragma once
/* ============================================================================
 *  mdpsr/runtime/json.h
 *  清单 / 配置用的极简 JSON 读取器 (只读, 不写)
 *
 *  刻意做得"严格": 解析完一个顶层值之后, 后面只允许空白 —— 这样
 *  "{...} 这不是 json" 会直接报错, 而不是静默接受半个文件。
 *  另外不做任何"注释 / 尾随逗号 / 单引号"的宽容 (清单是机器生成的契约,
 *  不需要人类手写便利; 严格一点能在编译期之外最早暴露拼错)。
 *
 *  存储策略: 字符串与数组元素全部寄存在 Json 自己的容器里, 所以
 *  as_string() / elements() 返回的引用在 Json 对象存活期间一直有效。
 * ==========================================================================*/

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace mdpsr {

class Json {
public:
    enum class Type { Null, Bool, Num, Str, Arr, Obj };

    Json() = default;

    Type type() const { return _t; }
    bool is_null()   const { return _t == Type::Null; }
    bool is_bool()   const { return _t == Type::Bool; }
    bool is_num()    const { return _t == Type::Num; }
    bool is_string() const { return _t == Type::Str; }
    bool is_array()  const { return _t == Type::Arr; }
    bool is_object() const { return _t == Type::Obj; }

    bool        as_bool(bool def = false) const { return _t == Type::Bool ? _b : def; }
    double      as_num(double def = 0) const { return _t == Type::Num ? _n : def; }
    int64_t     as_int(int64_t def = 0) const;
    std::string as_string(const std::string& def = std::string()) const {
        return _t == Type::Str ? _s : def;
    }

    size_t size() const {
        if (_t == Type::Arr) return _arr.size();
        if (_t == Type::Obj) return _obj.size();
        return 0;
    }

    /* 数组元素 */
    const std::vector<Json>& elements() const { return _arr; }

    /* 对象成员: 顺序保留 */
    const std::vector<std::pair<std::string, Json>>& items() const { return _obj; }

    /* 取对象成员; 不存在返回一个 Null 单例 */
    const Json& operator[](const char* k) const;

    /* 从文件读; 出错时填 err 并返回 false */
    static bool load_file(const std::wstring& path, Json* out, std::string* err);

    /* 从内存解析 */
    static bool parse(const std::string& text, Json* out, std::string* err);

private:
    struct Parser;

    Type        _t = Type::Null;
    bool        _b = false;
    double      _n = 0;
    std::string _s;
    std::vector<Json>                          _arr;
    std::vector<std::pair<std::string, Json>>  _obj;
};

} /* namespace mdpsr */
