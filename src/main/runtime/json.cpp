/* ============================================================================
 *  mdpsr/runtime/json.cpp
 * ==========================================================================*/
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mdpsr {

static const Json kEmpty;

const std::string& Json::as_string() const {
    static const std::string empty;
    return _t == STR ? _s : empty;
}

const Json& Json::operator[](const char* key) const {
    if (_t != OBJ || !key) return kEmpty;
    for (const auto& kv : _obj) {
        if (kv.first == key) return kv.second;
    }
    return kEmpty;
}

/* -------------------------------------------------------------------------- */
/* 这个解析器和 Json 处在同一个名字空间里, 因为 Json 里那句 friend 认的就是
 * mdpsr::JsonParser —— 把它塞进匿名名字空间会让 friend 指向另一个类型。 */
struct JsonParser {
    const char* p = nullptr;
    const char* e = nullptr;
    std::string* err = nullptr;
    int depth = 0;

    bool fail(const char* m) {
        if (err && err->empty()) *err = m;
        return false;
    }
    void ws() {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    bool lit(const char* s) {
        const size_t n = std::strlen(s);
        if (static_cast<size_t>(e - p) < n || std::memcmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }

    static void utf8_append(std::string* out, unsigned cp) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(unsigned* out) {
        if (e - p < 4) return fail("\\u 后面不足 4 位");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = p[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("\\u 后面不是十六进制");
        }
        p += 4;
        *out = v;
        return true;
    }

    bool str(std::string* out) {
        if (p >= e || *p != '"') return fail("期望字符串");
        ++p;
        out->clear();
        for (;;) {
            if (p >= e) return fail("字符串没有收尾的引号");
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') { ++p; return true; }
            if (c < 0x20) return fail("字符串里有裸的控制字符");
            if (c != '\\') { out->push_back(static_cast<char>(c)); ++p; continue; }
            ++p;
            if (p >= e) return fail("转义符后面没东西了");
            const char x = *p++;
            switch (x) {
            case '"':  out->push_back('"');  break;
            case '\\': out->push_back('\\'); break;
            case '/':  out->push_back('/');  break;
            case 'b':  out->push_back('\b'); break;
            case 'f':  out->push_back('\f'); break;
            case 'n':  out->push_back('\n'); break;
            case 'r':  out->push_back('\r'); break;
            case 't':  out->push_back('\t'); break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(&cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    const char* save = p;
                    p += 2;
                    unsigned lo = 0;
                    if (!hex4(&lo)) return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p = save;      /* 不是合法的一对, 就当单码位处理 */
                    }
                }
                utf8_append(out, cp);
                break;
            }
            default: return fail("不认识的转义字符");
            }
        }
    }

    bool number(Json* out) {
        const char* s = p;
        if (p < e && (*p == '-' || *p == '+')) ++p;
        bool digits = false;
        while (p < e && *p >= '0' && *p <= '9') { ++p; digits = true; }
        if (p < e && *p == '.') {
            ++p;
            while (p < e && *p >= '0' && *p <= '9') { ++p; digits = true; }
        }
        if (!digits) return fail("数字格式非法");
        if (p < e && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < e && (*p == '-' || *p == '+')) ++p;
            bool ed = false;
            while (p < e && *p >= '0' && *p <= '9') { ++p; ed = true; }
            if (!ed) return fail("指数部分非法");
        }
        const std::string tmp(s, static_cast<size_t>(p - s));
        out->_t = Json::NUM;
        out->_num = std::strtod(tmp.c_str(), nullptr);
        return true;
    }

    bool value(Json* out) {
        if (++depth > 64) return fail("嵌套太深");
        ws();
        if (p >= e) return fail("还没读到值就到了文件末尾");
        bool ok = false;
        switch (*p) {
        case '{': {
            ++p;
            out->_t = Json::OBJ;
            ws();
            if (p < e && *p == '}') { ++p; ok = true; break; }
            for (;;) {
                ws();
                std::string key;
                if (!str(&key)) { ok = false; break; }
                ws();
                if (p >= e || *p != ':') { fail("对象里缺少 ':'"); ok = false; break; }
                ++p;
                Json v;
                if (!value(&v)) { ok = false; break; }
                out->_obj.emplace_back(std::move(key), std::move(v));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; ok = true; break; }
                fail("对象里期望 ',' 或 '}'");
                ok = false;
                break;
            }
            break;
        }
        case '[': {
            ++p;
            out->_t = Json::ARR;
            ws();
            if (p < e && *p == ']') { ++p; ok = true; break; }
            for (;;) {
                Json v;
                if (!value(&v)) { ok = false; break; }
                out->_arr.push_back(std::move(v));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; ok = true; break; }
                fail("数组里期望 ',' 或 ']'");
                ok = false;
                break;
            }
            break;
        }
        case '"':
            out->_t = Json::STR;
            ok = str(&out->_s);
            break;
        case 't':
            if (lit("true")) { out->_t = Json::BOOL; out->_b = true; ok = true; }
            else ok = fail("期望 true");
            break;
        case 'f':
            if (lit("false")) { out->_t = Json::BOOL; out->_b = false; ok = true; }
            else ok = fail("期望 false");
            break;
        case 'n':
            if (lit("null")) { out->_t = Json::NUL; ok = true; }
            else ok = fail("期望 null");
            break;
        default:
            ok = number(out);
            break;
        }
        --depth;
        return ok;
    }
};

bool Json::parse(const std::string& text, Json* out, std::string* err) {
    if (!out) return false;
    if (err) err->clear();

    JsonParser ps;
    ps.p = text.data();
    ps.e = text.data() + text.size();
    ps.err = err;

    /* 跳过 UTF-8 BOM */
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        ps.p += 3;
    }

    Json v;
    if (!ps.value(&v)) return false;
    ps.ws();
    if (ps.p != ps.e) return ps.fail("顶层值之后还有多余内容 (严格模式不允许)");
    *out = std::move(v);
    return true;
}

bool Json::load_file(const std::wstring& path, Json* out, std::string* err) {
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (err) *err = "打不开文件";
        return false;
    }
    std::string buf;
    char tmp[8192];
    size_t n = 0;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) buf.append(tmp, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        if (err) *err = "读文件出错";
        return false;
    }
    if (!parse(buf, out, err)) {
        if (err) *err = "解析失败: " + *err;
        return false;
    }
    return true;
}

} /* namespace mdpsr */
