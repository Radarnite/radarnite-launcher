// JSON text exactly as System.Text.Json writes it in launcher/*.cs, so files the 2.x launcher and the 3.0 core both
// write (config.json, offline\build.json) stay byte-identical: indented = 2 spaces + CRLF; escaping either
// UnsafeRelaxedJsonEscaping (config.json) or the default encoder (JsonNode.ToJsonString: '+' -> + ...).
#pragma once
#include "vendor/yyjson/yyjson.h"
#include <string>

namespace rn {
namespace json {

std::string relaxed(const std::wstring& s);   // quoted string, UnsafeRelaxedJsonEscaping
std::string strict(const std::wstring& s);    // quoted string, JavaScriptEncoder.Default
// A parsed value re-written indented as .NET does (nested at `depth`); raw numbers (YYJSON_READ_NUMBER_AS_RAW) are
// written verbatim.
std::string indented(yyjson_val* v, int depth);
std::wstring wstr(yyjson_val* v);             // string value as UTF-16 ("" when not a string)
// yyjson_read of a UTF-8 text with the flags the core uses everywhere (BOM allowed, invalid UTF-8 tolerated).
yyjson_doc* parse(const std::string& text, yyjson_read_flag extra = 0);

} // namespace json
} // namespace rn
