#pragma once
/* ============================================================================
 *  mdpsr/json_small.h
 *  极简 JSON 解析器 (header-only), 宿主与插件共用的宽容解析实现。
 *
 *  支持:
 *    * null / true / false / 数字 / 字符串 / 数组 / 对象
 *    * UTF-8 BOM、// 与 /* *\/ 注释、尾随逗号
 *    * 宽容模式下把 ';' 也当作成员分隔符
 *      (Win_cmd/coment.json 就是这种写法)
 *    * 对象保持插入顺序
 *  不支持: 转义 \uXXXX 之外的扩展 (本实现支持完整 \uXXXX)。
 * ==========================================================================*/

#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mdpsr {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : _type(Type::Null), _bool(false), _num(0) {}
    explicit Json(bool b) : _type(Type::Bool), _bool(b), _num(0) {}
    explicit Json(double d) : _type(Type::Number), _bool(false), _num(d) {}
    explicit Json(std::string s) : _type(Type::String), _bool(false), _num(0), _str(std::move(s)) {}

    static Json array() { Json j; j._type = Type::Array; return j; }
    static Json object() { Json j; j._type = Type::Object; return j; }

    Type type() const { return _type; }
    bool is_null()   const { return _type == Type::Null; }
    bool is_bool()   const { return _type == Type::Bool; }
    bool is_number() const { return _type == Type::Number; }
    bool is_string() const { return _type == Type::String; }
    bool is_array()  const { return _type == Type::Array; }
    bool is_object() const { return _type == Type::Object; }

    bool as_bool(bool def = false) const {
        if (_type == Type::Bool) return _bool;
        if (_type == Type::Number) return _num != 0;
        return def;
    }
    double as_double(double def = 0.0) const {
        if (_type == Type::Number) return _num;
        if (_type == Type::Bool) return _bool ? 1.0 : 0.0;
        if (_type == Type::String) return std::atof(_str.c_str());
        return def;
    }
    long long as_int(long long def = 0) const {
        if (_type == Type::Number) return (long long)_num;
        if (_type == Type::Bool) return _bool ? 1 : 0;
        if (_type == Type::String) return std::atoll(_str.c_str());
        return def;
    }
    std::string as_string(const std::string& def = std::string()) const {
        if (_type == Type::String) return _str;
        if (_type == Type::Number) {
            char buf[64];
            if (_num == (double)(long long)_num) std::snprintf(buf, sizeof(buf), "%lld", (long long)_num);
            else std::snprintf(buf, sizeof(buf), "%g", _num);
            return std::string(buf);
        }
        if (_type == Type::Bool) return _bool ? "true" : "false";
        return def;
    }

    size_t size() const {
        if (_type == Type::Array) return _arr.size();
        if (_type == Type::Object) return _obj.size();
        return 0;
    }
    const Json& at(size_t i) const {
        static const Json null_value;
        if (_type == Type::Array && i < _arr.size()) return _arr[i];
        return null_value;
    }
    const Json* find(const std::string& key) const {
        if (_type != Type::Object) return nullptr;
        for (const auto& kv : _obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const Json& operator[](const std::string& key) const {
        const Json* p = find(key);
        static const Json null_value;
        return p ? *p : null_value;
    }
    const std::vector<std::pair<std::string, Json>>& items() const { return _obj; }
    const std::vector<Json>& elements() const { return _arr; }

    void push(Json v) { _type = Type::Array; _arr.push_back(std::move(v)); }
    void set(const std::string& k, Json v) {
        _type = Type::Object;
        for (auto& kv : _obj) if (kv.first == k) { kv.second = std::move(v); return; }
        _obj.emplace_back(k, std::move(v));
    }

    /* ---------------- 解析 ---------------- */
    static Json parse(const std::string& text, bool lenient = true, std::string* err = nullptr) {
        Parser p;
        p.s = text;
        p.i = 0;
        p.lenient = lenient;
        /* 跳过 UTF-8 BOM */
        if (p.s.size() >= 3 && (uint8_t)p.s[0] == 0xEF && (uint8_t)p.s[1] == 0xBB && (uint8_t)p.s[2] == 0xBF) p.i = 3;
        Json v;
        try {
            p.skip_ws();
            v = p.parse_value(0);
            p.skip_ws();
        } catch (const std::string& e) {
            if (err) *err = e;
            return Json();
        }
        return v;
    }

    static bool load_file(const std::wstring& path, Json* out, bool lenient = true, std::string* err = nullptr) {
        std::string bytes;
        if (!read_all(path, &bytes)) {
            if (err) *err = "无法读取文件";
            return false;
        }
        Json v = parse(bytes, lenient, err);
        if (err && !err->empty()) return false;
        *out = std::move(v);
        return true;
    }

    static bool read_all(const std::wstring& path, std::string* out) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n < 0) { std::fclose(f); return false; }
        out->resize((size_t)n);
        size_t got = n > 0 ? std::fread(&(*out)[0], 1, (size_t)n, f) : 0;
        out->resize(got);
        std::fclose(f);
        return true;
    }

private:
    struct Parser {
        std::string s;
        size_t i = 0;
        bool lenient = true;

        [[noreturn]] void fail(const char* what) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "JSON 解析错误 @%zu: %s", i, what);
            throw std::string(buf);
        }
        bool eof() const { return i >= s.size(); }
        char cur() const { return i < s.size() ? s[i] : '\0'; }

        void skip_ws() {
            for (;;) {
                while (!eof() && (cur() == ' ' || cur() == '\t' || cur() == '\r' || cur() == '\n')) i++;
                if (!eof() && cur() == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                    while (!eof() && cur() != '\n') i++;
                    continue;
                }
                if (!eof() && cur() == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                    i += 2;
                    while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) i++;
                    i = (i + 1 < s.size()) ? i + 2 : s.size();
                    continue;
                }
                break;
            }
        }
        bool is_sep(char c) const {
            if (c == ',') return true;
            if (lenient && c == ';') return true;   /* coment.json 风格 */
            return false;
        }

        void expect(char c) {
            skip_ws();
            if (eof() || cur() != c) fail("缺少期望字符");
            i++;
        }

        Json parse_value(int depth) {
            if (depth > 64) fail("嵌套过深");
            skip_ws();
            if (eof()) fail("意外结束");
            char c = cur();
            if (c == '{') return parse_object(depth);
            if (c == '[') return parse_array(depth);
            if (c == '"') return Json(parse_string());
            if (c == '\'') return Json(parse_string_single());
            if (!std::strncmp(s.c_str() + i, "true", 4)) { i += 4; return Json(true); }
            if (!std::strncmp(s.c_str() + i, "false", 5)) { i += 5; return Json(false); }
            if (!std::strncmp(s.c_str() + i, "null", 4)) { i += 4; return Json(); }
            return parse_number();
        }

        Json parse_object(int depth) {
            expect('{');
            Json o = Json::object();
            skip_ws();
            if (!eof() && cur() == '}') { i++; return o; }
            for (;;) {
                skip_ws();
                if (eof()) fail("对象未闭合");
                /* 注意: 键与值必须分两步求值, 不能写进同一个函数调用,
                   否则编译器可以调换实参求值顺序而吃掉后面的值。 */
                if (cur() == '"') {
                    std::string k = parse_string();
                    expect(':');
                    o.set(k, parse_value(depth + 1));
                } else if (cur() == '\'') {
                    std::string k = parse_string_single();
                    expect(':');
                    o.set(k, parse_value(depth + 1));
                } else {
                    fail("对象的键必须是字符串");
                }
                skip_ws();
                if (!eof() && is_sep(cur())) {
                    i++;
                    skip_ws();
                    if (!eof() && cur() == '}') { i++; return o; }   /* 尾随分隔符 */
                    continue;
                }
                if (!eof() && cur() == '}') { i++; return o; }
                fail("对象成员分隔符缺失");
            }
        }

        Json parse_array(int depth) {
            expect('[');
            Json a = Json::array();
            skip_ws();
            if (!eof() && cur() == ']') { i++; return a; }
            for (;;) {
                a.push(parse_value(depth + 1));
                skip_ws();
                if (!eof() && is_sep(cur())) {
                    i++;
                    skip_ws();
                    if (!eof() && cur() == ']') { i++; return a; }
                    continue;
                }
                if (!eof() && cur() == ']') { i++; return a; }
                fail("数组元素分隔符缺失");
            }
        }

        std::string parse_string() {
            expect('"');
            std::string out;
            for (;;) {
                if (eof()) fail("字符串未闭合");
                char c = s[i++];
                if (c == '"') break;
                if (c == '\\') {
                    if (eof()) fail("转义未完成");
                    char e = s[i++];
                    switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case '/': out.push_back('/'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    case 'u': {
                        if (i + 4 > s.size()) fail("\\u 不完整");
                        unsigned cp = 0;
                        for (int k = 0; k < 4; k++) {
                            char h = s[i++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else fail("\\u 非法十六进制");
                        }
                        /* UTF-8 编码 */
                        if (cp < 0x80) out.push_back((char)cp);
                        else if (cp < 0x800) {
                            out.push_back((char)(0xC0 | (cp >> 6)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back((char)(0xE0 | (cp >> 12)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(e); break;
                    }
                } else {
                    out.push_back(c);
                }
            }
            return out;
        }

        std::string parse_string_single() {
            expect('\'');
            std::string out;
            for (;;) {
                if (eof()) fail("字符串未闭合");
                char c = s[i++];
                if (c == '\'') break;
                if (c == '\\' && !eof() && s[i] == '\'') { out.push_back('\''); i++; continue; }
                out.push_back(c);
            }
            return out;
        }

        Json parse_number() {
            size_t start = i;
            if (!eof() && (cur() == '-' || cur() == '+')) i++;
            while (!eof() && ((cur() >= '0' && cur() <= '9') || cur() == '.' ||
                              cur() == 'e' || cur() == 'E' || cur() == '-' || cur() == '+')) i++;
            if (i == start) fail("非法字面量");
            std::string num = s.substr(start, i - start);
            return Json(std::atof(num.c_str()));
        }
    };

    Type _type;
    bool _bool;
    double _num;
    std::string _str;
    std::vector<Json> _arr;
    std::vector<std::pair<std::string, Json>> _obj;
};

} /* namespace mdpsr */
