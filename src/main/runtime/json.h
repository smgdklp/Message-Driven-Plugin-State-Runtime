#pragma once
/* ============================================================================
 *  mdpsr/runtime/json.h
 *  一个够用就好的严格 JSON 读取器 (只读, 不写)
 *
 *  "严格"的意思: 顶层值解析完之后不许还有非空白内容; 数字格式非法直接报错;
 *  字符串里的控制字符直接报错。清单文件不该有模糊地带。
 * ==========================================================================*/

#include <string>
#include <utility>
#include <vector>

namespace mdpsr {

class Json {
public:
    enum Type { NUL = 0, BOOL, NUM, STR, ARR, OBJ };

    Json() = default;

    static bool load_file(const std::wstring& path, Json* out, std::string* err);
    static bool parse(const std::string& text, Json* out, std::string* err);

    Type type() const { return _t; }
    bool is_null() const { return _t == NUL; }
    bool is_bool() const { return _t == BOOL; }
    bool is_number() const { return _t == NUM; }
    bool is_string() const { return _t == STR; }
    bool is_array() const { return _t == ARR; }
    bool is_object() const { return _t == OBJ; }

    bool   as_bool(bool d = false) const { return _t == BOOL ? _b : d; }
    double as_number(double d = 0) const { return _t == NUM ? _num : d; }
    const std::string& as_string() const;

    const Json& operator[](const char* key) const;
    const std::vector<Json>& elements() const { return _arr; }
    /* 对象的成员列表 (键, 值); 不是对象时为空。
     * 遍历 "Static_State" 这种"键是数据、不是固定字段"的对象要用它。 */
    const std::vector<std::pair<std::string, Json>>& members() const { return _obj; }
    size_t size() const {
        if (_t == ARR) return _arr.size();
        if (_t == OBJ) return _obj.size();
        return 0;
    }

private:
    Type        _t = NUL;
    bool        _b = false;
    double      _num = 0;
    std::string _s;
    std::vector<Json> _arr;
    std::vector<std::pair<std::string, Json>> _obj;

    friend struct JsonParser;
};

} /* namespace mdpsr */
