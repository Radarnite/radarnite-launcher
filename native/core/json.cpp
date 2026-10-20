#include "json.h"
#include "util.h"
#include <stdio.h>

namespace rn {
namespace json {

static void u4(std::string& o, unsigned c) {
    char b[8];
    snprintf(b, sizeof b, "\\u%04X", c & 0xFFFF);
    o += b;
}

static std::string encode(const std::wstring& s, bool relaxedMode) {
    std::string o = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        switch (c) {
        case L'\b': o += "\\b"; continue;
        case L'\f': o += "\\f"; continue;
        case L'\n': o += "\\n"; continue;
        case L'\r': o += "\\r"; continue;
        case L'\t': o += "\\t"; continue;
        case L'\\': o += "\\\\"; continue;
        case L'"': if (relaxedMode) o += "\\\""; else u4(o, c); continue;
        }
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029) { u4(o, c); continue; }
        if (c < 0x80) {
            if (!relaxedMode && (c == L'&' || c == L'\'' || c == L'+' || c == L'<' || c == L'>' || c == L'`')) { u4(o, c); continue; }
            o += (char)c;
            continue;
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            // surrogates (characters outside the BMP) are always escaped; a lone one becomes U+FFFD
            bool pair = c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF;
            if (pair) { u4(o, c); u4(o, s[++i]); } else u4(o, 0xFFFD);
            continue;
        }
        if (!relaxedMode) { u4(o, c); continue; }
        o += narrow(std::wstring(1, c));
    }
    return o + "\"";
}

std::string relaxed(const std::wstring& s) { return encode(s, true); }
std::string strict(const std::wstring& s) { return encode(s, false); }

std::wstring wstr(yyjson_val* v) {
    if (!v || !yyjson_is_str(v)) return L"";
    return widen(std::string(yyjson_get_str(v), yyjson_get_len(v)));
}

static std::string pad(int depth) { return std::string((size_t)depth * 2, ' '); }

std::string indented(yyjson_val* v, int depth) {
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: return "null";
    case YYJSON_TYPE_BOOL: return yyjson_get_bool(v) ? "true" : "false";
    case YYJSON_TYPE_RAW: return std::string(yyjson_get_raw(v), yyjson_get_len(v));
    case YYJSON_TYPE_NUM: {
        char b[64];
        if (yyjson_is_uint(v)) snprintf(b, sizeof b, "%llu", (unsigned long long)yyjson_get_uint(v));
        else if (yyjson_is_sint(v)) snprintf(b, sizeof b, "%lld", (long long)yyjson_get_sint(v));
        else snprintf(b, sizeof b, "%.17g", yyjson_get_real(v));
        return b;
    }
    case YYJSON_TYPE_STR: return relaxed(wstr(v));
    case YYJSON_TYPE_ARR: {
        if (!yyjson_arr_size(v)) return "[]";
        std::string o = "[";
        size_t i, n;
        yyjson_val* it;
        yyjson_arr_foreach(v, i, n, it) { o += i ? ",\r\n" : "\r\n"; o += pad(depth + 1) + indented(it, depth + 1); }
        return o + "\r\n" + pad(depth) + "]";
    }
    case YYJSON_TYPE_OBJ: {
        if (!yyjson_obj_size(v)) return "{}";
        std::string o = "{";
        size_t i, n;
        yyjson_val *k, *val;
        yyjson_obj_foreach(v, i, n, k, val) {
            o += i ? ",\r\n" : "\r\n";
            o += pad(depth + 1) + relaxed(wstr(k)) + ": " + indented(val, depth + 1);
        }
        return o + "\r\n" + pad(depth) + "}";
    }
    default: return "null";
    }
}

yyjson_doc* parse(const std::string& text, yyjson_read_flag extra) {
    return yyjson_read(text.data(), text.size(), YYJSON_READ_ALLOW_BOM | YYJSON_READ_ALLOW_INVALID_UNICODE | extra);
}

} // namespace json
} // namespace rn
