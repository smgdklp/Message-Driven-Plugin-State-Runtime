/* ============================================================================
 *  mdpsr/runtime/json.cpp
 * ==========================================================================*/
#include "json.h"

#include <cmath>

namespace mdpsr {

/* ==========================================================================
 *  Parser
 * ==========================================================================*/
struct Json::Parser {
    const char* b = nullptr;
    const char* e = nullptr;
    const char* p = nullptr;
    std::string err;
    int         depth = 0;

    static constexpr int kMaxDepth = 64;

    bool fail(const std::string& what) {
        if (err.empty()) {
            const size_t off = static_cast<size_t>(p - b);
            size_t line = 1, col = 1;
            for (size_t i = 0; i < off && i < static_cast<size_t>(e - b); ++i) {
                if (b[i] == '\n') { ++line; col = 1; } else { ++col; }
            }
            err = "JSON 错误 @行" + std::to_string(line) + "列" + std::to_string(col) + ": " + what;
        }
        return false;
    }

    bool eof() const { return p >= e; }
    char cur() const { return p < e ? *p : '\0'; }

    bool skip_ws() {
        while (p < e) {
            const char c = *p;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++p; continue; }
            break;
        }
        return true;
    }

    bool expect(char c) {
        if (eof() || *p != c) return fail(std::string("期望 '") + c + "'");
        ++p;
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(uint32_t* out) {
        if (e - p < 4) return fail("\\u 后面需要 4 位十六进制");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = p[i];
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("\\u 后面不是十六进制");
        }
        p += 4;
        *out = v;
        return true;
    }

    bool parse_string(std::string* out) {
        if (!expect('"')) return false;
        for (;;) {
            if (eof()) return fail("字符串未闭合");
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') { ++p; return true; }
            if (c < 0x20) return fail("字符串里出现未转义的控制字符");
            if (c != '\\') { out->push_back(static_cast<char>(c)); ++p; continue; }

            ++p;                                  /* 吃掉反斜杠 */
            if (eof()) return fail("转义字符后意外结束");
            const char esc = *p++;
            switch (esc) {
            case '"':  out->push_back('"');  break;
            case '\\': out->push_back('\\'); break;
            case '/':  out->push_back('/');  break;
            case 'b':  out->push_back('\b'); break;
            case 'f':  out->push_back('\f'); break;
            case 'n':  out->push_back('\n'); break;
            case 'r':  out->push_back('\r'); break;
            case 't':  out->push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(&cp)) return false;
                /* 代理对: 高代理 + \u 低代理 -> 合成一个码点 */
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (e - p >= 2 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo = 0;
                        if (!hex4(&lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            append_utf8(*out, 0xFFFD);   /* 落单的高代理 */
                            cp = lo;
                        }
                    } else {
                        append_utf8(*out, 0xFFFD);
                        break;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;                          /* 落单的低代理 */
                }
                append_utf8(*out, cp);
                break;
            }
            default:
                return fail("不认识的转义字符");
            }
        }
    }

    bool parse_number(Json* out) {
        const char* start = p;
        if (p < e && *p == '-') ++p;
        if (p >= e || *p < '0' || *p > '9') return fail("数字格式非法");
        while (p < e && *p >= '0' && *p <= '9') ++p;
        if (p < e && *p == '.') {
            ++p;
            if (p >= e || *p < '0' || *p > '9') return fail("小数点后没有数字");
            while (p < e && *p >= '0' && *p <= '9') ++p;
        }
        if (p < e && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < e && (*p == '+' || *p == '-')) ++p;
            if (p >= e || *p < '0' || *p > '9') return fail("指数后没有数字");
            while (p < e && *p >= '0' && *p <= '9') ++p;
        }
        const std::string tok(start, static_cast<size_t>(p - start));
        out->_t = Type::Num;
        out->_n = std::strtod(tok.c_str(), nullptr);
        if (!std::isfinite(out->_n)) return fail("数字超出可表示范围");
        return true;
    }

    bool literal(const char* lit, size_t n, Json* out, Json::Type t, bool bv) {
        if (static_cast<size_t>(e - p) < n || std::memcmp(p, lit, n) != 0) return false;
        p += n;
        out->_t = t;
        out->_b = bv;
        return true;
    }

    bool parse_value(Json* out) {
        if (++depth > kMaxDepth) return fail("嵌套过深");
        skip_ws();
        if (eof()) return fail("意外结束");

        bool ok = false;
        switch (cur()) {
        case '{': ok = parse_object(out); break;
        case '[': ok = parse_array(out);  break;
        case '"':
            out->_t = Type::Str;
            ok = parse_string(&out->_s);
            break;
        default:
            if      (literal("true",  4, out, Type::Bool, true))  ok = true;
            else if (literal("false", 5, out, Type::Bool, false)) ok = true;
            else if (literal("null",  4, out, Type::Null, false)) ok = true;
            else                                                  ok = parse_number(out);
            break;
        }
        --depth;
        return ok;
    }

    bool parse_array(Json* out) {
        if (!expect('[')) return false;
        out->_t = Type::Arr;
        skip_ws();
        if (!eof() && cur() == ']') { ++p; return true; }
        for (;;) {
            out->_arr.emplace_back();
            if (!parse_value(&out->_arr.back())) return false;
            skip_ws();
            if (!eof() && cur() == ',') { ++p; continue; }
            if (!eof() && cur() == ']') { ++p; return true; }
            return fail("数组里期望 ',' 或 ']'");
        }
    }

    bool parse_object(Json* out) {
        if (!expect('{')) return false;
        out->_t = Type::Obj;
        skip_ws();
        if (!eof() && cur() == '}') { ++p; return true; }
        for (;;) {
            skip_ws();
            if (eof() || cur() != '"') return fail("对象的键必须是双引号字符串");
            std::string key;
            if (!parse_string(&key)) return false;
            skip_ws();
            if (!expect(':')) return false;
            /* 注意: 先把 key 放进 items 再解析值, 不能写成一个表达式 ——
               否则实参求值顺序可能把值吃掉。 */
            out->_obj.emplace_back(std::move(key), Json{});
            if (!parse_value(&out->_obj.back().second)) return false;
            skip_ws();
            if (!eof() && cur() == ',') { ++p; continue; }
            if (!eof() && cur() == '}') { ++p; return true; }
            return fail("对象里期望 ',' 或 '}'");
        }
    }
};

/* ==========================================================================
 *  对外接口
 * ==========================================================================*/
const Json& Json::operator[](const char* k) const {
    static const Json kNull;
    if (_t != Type::Obj || !k) return kNull;
    for (const auto& kv : _obj) {
        if (kv.first == k) return kv.second;
    }
    return kNull;
}

int64_t Json::as_int(int64_t def) const {
    if (_t != Type::Num) return def;
    /* 显式夹紧, 而不是让 (int64_t)double 走进 UB */
    if (_n >= 9223372036854775807.0) return 9223372036854775807LL;
    if (_n <= -9223372036854775808.0) return (-9223372036854775807LL - 1);
    return static_cast<int64_t>(_n);
}

bool Json::parse(const std::string& text, Json* out, std::string* err) {
    Parser ps;
    ps.b = text.data();
    ps.p = text.data();
    ps.e = text.data() + text.size();

    /* 跳过 UTF-8 BOM */
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        ps.p += 3;
    }

    Json v;
    ps.skip_ws();
    if (ps.eof()) {
        if (err) *err = "文件是空的";
        return false;
    }
    if (!ps.parse_value(&v)) {
        if (err) *err = ps.err.empty() ? "解析失败" : ps.err;
        return false;
    }
    /* 严格: 顶层值之后只允许空白 */
    ps.skip_ws();
    if (!ps.eof()) {
        if (err) *err = "顶层值之后还有多余内容 (第 " +
                        std::to_string(static_cast<size_t>(ps.p - ps.b)) + " 字节起)";
        return false;
    }
    *out = std::move(v);
    return true;
}

bool Json::load_file(const std::wstring& path, Json* out, std::string* err) {
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (err) *err = "打不开文件";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); if (err) *err = "取文件长度失败"; return false; }

    std::string bytes(static_cast<size_t>(n), '\0');
    const size_t got = (n > 0) ? std::fread(&bytes[0], 1, static_cast<size_t>(n), f) : 0;
    std::fclose(f);
    bytes.resize(got);
    return parse(bytes, out, err);
}

} /* namespace mdpsr */
