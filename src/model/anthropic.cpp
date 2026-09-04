/*
 * ChatLink - link a TI-Nspire CX to a PC over USB.
 * Copyright (C) 2026 Christopher Willett / AnvilDevelopment.US
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. See the LICENSE file for the full text.
 */

#include "anthropic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <cstdlib>
#include <sstream>
#include <vector>

namespace chatlink::model {
namespace {

// ---------------------------------------------------------------------------
// Minimal JSON
//
// Only what this file needs: build a request object, and walk the response to
// pull out content[].text, stop_reason and any error message. Deliberately a
// real parser rather than string matching - JSON strings can contain braces,
// quotes and escapes, so regex-style extraction breaks on exactly the replies
// we care about (code, quotes, newlines).
// ---------------------------------------------------------------------------

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (const unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void encodeUtf8(unsigned code_point, std::string& out) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

class Scanner {
public:
    explicit Scanner(const std::string& text) : s_(text) {}

    void skipWhitespace() {
        while (pos_ < s_.size()
               && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) {
            ++pos_;
        }
    }

    bool atEnd() { skipWhitespace(); return pos_ >= s_.size(); }
    char peek() { skipWhitespace(); return pos_ < s_.size() ? s_[pos_] : '\0'; }
    bool consume(char c) {
        skipWhitespace();
        if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    // Reads a JSON string, resolving escapes (including surrogate pairs).
    bool readString(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (pos_ >= s_.size()) return false;

            const char esc = s_[pos_++];
            switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) return false;
                    unsigned cp = std::strtoul(s_.substr(pos_, 4).c_str(), nullptr, 16);
                    pos_ += 4;
                    // Surrogate pair: the API escapes astral characters this way.
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size()
                            && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        const unsigned low =
                                std::strtoul(s_.substr(pos_ + 2, 4).c_str(), nullptr, 16);
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            pos_ += 6;
                        }
                    }
                    encodeUtf8(cp, out);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    // Skips any value, so we can walk past keys we do not care about.
    bool skipValue() {
        skipWhitespace();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '"') { std::string ignored; return readString(ignored); }
        if (c == '{' || c == '[') {
            const char close = (c == '{') ? '}' : ']';
            ++pos_;
            int depth = 1;
            while (pos_ < s_.size() && depth > 0) {
                const char d = s_[pos_];
                if (d == '"') { std::string ignored; if (!readString(ignored)) return false; continue; }
                if (d == c) ++depth;
                else if (d == close) --depth;
                ++pos_;
            }
            return depth == 0;
        }
        while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' && s_[pos_] != ']') ++pos_;
        return true;
    }

    std::size_t position() const { return pos_; }
    void seek(std::size_t p) { pos_ = p; }

private:
    const std::string& s_;
    std::size_t pos_ = 0;
};

// Walks an object, invoking `visit(key, scanner)` per member. The visitor must
// consume exactly one value; returning false means "I did not, skip it".
template <typename Visitor>
bool forEachMember(Scanner& sc, Visitor visit) {
    if (!sc.consume('{')) return false;
    if (sc.consume('}')) return true;
    for (;;) {
        std::string key;
        if (!sc.readString(key)) return false;
        if (!sc.consume(':')) return false;
        if (!visit(key, sc)) {
            if (!sc.skipValue()) return false;
        }
        if (sc.consume(',')) continue;
        return sc.consume('}');
    }
}

// ---------------------------------------------------------------------------
// HTTPS
// ---------------------------------------------------------------------------

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string winErrorText(const char* what) {
    const DWORD code = GetLastError();
    std::ostringstream out;
    out << what << " failed (error " << code << ")";
    if (code == ERROR_WINHTTP_TIMEOUT)          out << ": timed out";
    else if (code == ERROR_WINHTTP_CANNOT_CONNECT)  out << ": cannot connect";
    else if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) out << ": DNS lookup failed";
    else if (code == ERROR_WINHTTP_SECURE_FAILURE)  out << ": TLS failure";
    return out.str();
}

struct HttpResponse {
    bool        ok = false;
    DWORD       status = 0;
    std::string body;
    std::string error;
};

HttpResponse post(const std::string& body, const Config& config) {
    HttpResponse result;

    HINTERNET session = WinHttpOpen(L"ChatLink/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { result.error = winErrorText("WinHttpOpen"); return result; }

    const DWORD ms = static_cast<DWORD>(config.timeout_seconds) * 1000;
    WinHttpSetTimeouts(session, static_cast<int>(ms), static_cast<int>(ms),
                       static_cast<int>(ms), static_cast<int>(ms));

    HINTERNET connection = WinHttpConnect(session, L"api.anthropic.com",
                                          INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connection) {
        result.error = winErrorText("WinHttpConnect");
        WinHttpCloseHandle(session);
        return result;
    }

    HINTERNET request = WinHttpOpenRequest(connection, L"POST", L"/v1/messages",
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if (!request) {
        result.error = winErrorText("WinHttpOpenRequest");
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return result;
    }

    const std::wstring headers =
            L"Content-Type: application/json\r\n"
            L"anthropic-version: 2023-06-01\r\n"
            L"anthropic-beta: server-side-fallback-2026-07-01\r\n"
            L"x-api-key: " + widen(config.api_key) + L"\r\n";

    BOOL sent = WinHttpAddRequestHeaders(request, headers.c_str(),
                                         static_cast<DWORD>(-1),
                                         WINHTTP_ADDREQ_FLAG_ADD)
             && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()), 0)
             && WinHttpReceiveResponse(request, nullptr);

    if (!sent) {
        result.error = winErrorText("HTTPS request");
    } else {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        result.status = status;

        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
            result.body.append(chunk.data(), read);
        }
        result.ok = true;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

} // namespace

std::string apiKeyFromEnvironment() {
    // _dupenv_s avoids the deprecated getenv on MSVC; plain getenv is fine on
    // MinGW and keeps this portable.
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    return key ? std::string(key) : std::string();
}

Result ask(const Config& config, const std::string& prompt) {
    Result result;

    if (config.api_key.empty()) {
        result.error = "no API key (set ANTHROPIC_API_KEY)";
        return result;
    }

    std::ostringstream body;
    body << "{"
         << R"("model":")" << jsonEscape(config.model) << R"(",)"
         << R"("max_tokens":)" << config.max_tokens << ","
         << R"("system":")" << jsonEscape(config.system_prompt) << R"(",)"
         << R"("output_config":{"effort":")" << jsonEscape(config.effort) << R"("},)"
         // Server-side fallback: on a policy decline the API retries the same
         // request on a fallback model within the same call, instead of just
         // stopping. Recommended default for Opus 5.
         << R"("fallbacks":"default",)"
         << R"("messages":[{"role":"user","content":")" << jsonEscape(prompt) << R"("}])"
         << "}";

    const HttpResponse http = post(body.str(), config);
    if (!http.ok) {
        result.error = http.error;
        return result;
    }

    // Pull out what we need: content[].text, stop_reason, and error.message.
    std::string text, stop_reason, api_error, error_type;
    {
        Scanner sc(http.body);
        forEachMember(sc, [&](const std::string& key, Scanner& s) {
            if (key == "stop_reason") {
                if (s.peek() == '"') return s.readString(stop_reason);
                return false;
            }
            if (key == "error") {
                return forEachMember(s, [&](const std::string& ek, Scanner& es) {
                    if (ek == "message") return es.readString(api_error);
                    if (ek == "type") return es.readString(error_type);
                    return false;
                });
            }
            if (key == "content") {
                if (!s.consume('[')) return false;
                if (s.consume(']')) return true;
                for (;;) {
                    std::string block_type, block_text;
                    if (!forEachMember(s, [&](const std::string& bk, Scanner& bs) {
                            if (bk == "type") return bs.readString(block_type);
                            if (bk == "text") return bs.readString(block_text);
                            return false;
                        })) {
                        return false;
                    }
                    // Thinking blocks also carry "text"; only keep real output.
                    if (block_type == "text") text += block_text;
                    if (s.consume(',')) continue;
                    return s.consume(']');
                }
            }
            return false;
        });
    }

    if (http.status != 200) {
        std::ostringstream why;
        why << "HTTP " << http.status;
        if (!error_type.empty()) why << " (" << error_type << ")";
        if (!api_error.empty()) why << ": " << api_error;
        else if (text.empty()) why << ": " << http.body.substr(0, 200);
        result.error = why.str();
        return result;
    }

    if (stop_reason == "refusal") {
        result.error = "the model declined this request";
        return result;
    }

    if (text.empty()) {
        result.error = "empty reply from the API";
        return result;
    }

    result.ok = true;
    result.text = text;
    return result;
}

} // namespace chatlink::model
