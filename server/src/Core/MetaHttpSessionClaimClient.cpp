#include "Core/MetaHttpSessionClaimClient.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
constexpr std::string_view kHttpScheme = "http://";
constexpr std::string_view kClaimPath =
    "/internal/release0/game-session-tokens/claim";
constexpr std::string_view kReleasePath =
    "/internal/release0/game-session-tokens/release";
constexpr std::string_view kRenewPath =
    "/internal/release0/game-session-tokens/renew";
constexpr std::size_t kMaxHttpResponseBytes = 1024 * 1024;
constexpr int kHttpSocketTimeoutMs = 3000;

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}

bool isAsciiDigit(char ch) {
    return ch >= '0' && ch <= '9';
}

bool isOptionalHttpWhitespace(char ch) {
    return ch == ' ' || ch == '\t';
}

bool isJsonWhitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool parsePort(std::string_view value, uint16_t& outPort) {
    if (value.empty()) {
        return false;
    }

    uint32_t port = 0;
    for (const char ch : value) {
        if (!isAsciiDigit(ch)) {
            return false;
        }
        port = port * 10u + static_cast<uint32_t>(ch - '0');
        if (port > 65535u) {
            return false;
        }
    }
    if (port == 0u) {
        return false;
    }

    outPort = static_cast<uint16_t>(port);
    return true;
}

bool containsInvalidAuthorityChar(std::string_view value) {
    return value.find_first_of(" \t\r\n") != std::string_view::npos;
}

bool containsHeaderLineBreak(std::string_view value) {
    return value.find_first_of("\r\n") != std::string_view::npos;
}

bool parseAuthority(
    std::string_view authority,
    std::string& outHost,
    uint16_t& outPort) {
    if (authority.empty() || containsInvalidAuthorityChar(authority) ||
        authority.find('@') != std::string_view::npos) {
        return false;
    }

    uint16_t port = 80;
    std::string_view host = authority;
    if (authority.front() == '[') {
        const std::size_t closeBracket = authority.find(']');
        if (closeBracket == std::string_view::npos || closeBracket == 1u) {
            return false;
        }

        host = authority.substr(1, closeBracket - 1u);
        if (closeBracket + 1u < authority.size()) {
            if (authority[closeBracket + 1u] != ':') {
                return false;
            }
            if (!parsePort(authority.substr(closeBracket + 2u), port)) {
                return false;
            }
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon || colon == 0u) {
                return false;
            }
            host = authority.substr(0, colon);
            if (!parsePort(authority.substr(colon + 1u), port)) {
                return false;
            }
        }
    }

    if (host.empty()) {
        return false;
    }

    outHost = std::string(host);
    outPort = port;
    return true;
}

std::string normalizeBasePath(std::string_view path) {
    std::string basePath(path);
    while (basePath.size() > 1u && basePath.back() == '/') {
        basePath.pop_back();
    }
    if (basePath == "/") {
        basePath.clear();
    }
    return basePath;
}

void appendHexByte(std::string& out, unsigned char value) {
    constexpr char kHex[] = "0123456789abcdef";
    out.push_back(kHex[(value >> 4u) & 0x0Fu]);
    out.push_back(kHex[value & 0x0Fu]);
}

void appendJsonEscaped(std::string& out, std::string_view value) {
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20u) {
                out += "\\u00";
                appendHexByte(out, ch);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
}

std::string jsonString(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2u);
    result.push_back('"');
    appendJsonEscaped(result, value);
    result.push_back('"');
    return result;
}

std::size_t skipJsonWhitespace(std::string_view body, std::size_t pos) {
    while (pos < body.size() && isJsonWhitespace(body[pos])) {
        ++pos;
    }
    return pos;
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

char toAsciiLower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool equalsIgnoreCase(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (toAsciiLower(left[i]) != toAsciiLower(right[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trimHttpWhitespace(std::string_view value) {
    while (!value.empty() && isOptionalHttpWhitespace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && isOptionalHttpWhitespace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

bool containsTransferEncodingToken(
    std::string_view value,
    std::string_view token) {
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        std::string_view part = comma == std::string_view::npos
            ? value.substr(start)
            : value.substr(start, comma - start);
        part = trimHttpWhitespace(part);
        if (equalsIgnoreCase(part, token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        start = comma + 1u;
    }
    return false;
}

bool parseChunkSize(std::string_view line, std::size_t& outSize) {
    line = trimHttpWhitespace(line);
    const std::size_t extensionStart = line.find(';');
    if (extensionStart != std::string_view::npos) {
        line = trimHttpWhitespace(line.substr(0, extensionStart));
    }
    if (line.empty()) {
        return false;
    }

    std::size_t size = 0;
    for (const char ch : line) {
        const int digit = hexValue(ch);
        if (digit < 0) {
            return false;
        }
        if (size > (std::numeric_limits<std::size_t>::max() -
                static_cast<std::size_t>(digit)) / 16u) {
            return false;
        }
        size = size * 16u + static_cast<std::size_t>(digit);
    }

    outSize = size;
    return true;
}

bool skipChunkTrailers(std::string_view body, std::size_t& pos) {
    for (;;) {
        const std::size_t lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string_view::npos) {
            return false;
        }
        if (lineEnd == pos) {
            pos += 2u;
            return pos == body.size();
        }
        pos = lineEnd + 2u;
    }
}

bool decodeChunkedBody(std::string_view body, std::string& outBody) {
    std::string decoded;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t lineEnd = body.find("\r\n", pos);
        if (lineEnd == std::string_view::npos) {
            return false;
        }

        std::size_t chunkSize = 0;
        if (!parseChunkSize(body.substr(pos, lineEnd - pos), chunkSize)) {
            return false;
        }
        pos = lineEnd + 2u;

        if (chunkSize == 0u) {
            if (!skipChunkTrailers(body, pos)) {
                return false;
            }
            outBody = std::move(decoded);
            return true;
        }

        if (chunkSize > body.size() - pos ||
            decoded.size() + chunkSize > kMaxHttpResponseBytes) {
            return false;
        }
        decoded.append(body.substr(pos, chunkSize));
        pos += chunkSize;
        if (pos + 2u > body.size() || body.substr(pos, 2u) != "\r\n") {
            return false;
        }
        pos += 2u;
    }
}

bool parseUnicodeEscape(std::string_view body, std::size_t pos, uint32_t& outValue) {
    if (pos + 4u > body.size()) {
        return false;
    }

    uint32_t value = 0;
    for (std::size_t i = 0; i < 4u; ++i) {
        const int digit = hexValue(body[pos + i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4u) | static_cast<uint32_t>(digit);
    }

    outValue = value;
    return true;
}

bool appendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint <= 0x7Fu) {
        out.push_back(static_cast<char>(codePoint));
        return true;
    }
    if (codePoint <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (codePoint >> 6u)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        return true;
    }
    if (codePoint >= 0xD800u && codePoint <= 0xDFFFu) {
        return false;
    }
    if (codePoint <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (codePoint >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        return true;
    }
    if (codePoint <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (codePoint >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        return true;
    }
    return false;
}

bool appendJsonUnicodeEscape(
    std::string_view body,
    std::size_t& pos,
    std::string& out) {
    uint32_t codePoint = 0;
    if (!parseUnicodeEscape(body, pos, codePoint)) {
        return false;
    }
    pos += 4u;

    if (codePoint >= 0xD800u && codePoint <= 0xDBFFu) {
        if (pos + 6u > body.size() || body[pos] != '\\' || body[pos + 1u] != 'u') {
            return false;
        }

        uint32_t lowSurrogate = 0;
        if (!parseUnicodeEscape(body, pos + 2u, lowSurrogate) ||
            lowSurrogate < 0xDC00u || lowSurrogate > 0xDFFFu) {
            return false;
        }

        codePoint =
            0x10000u +
            (((codePoint - 0xD800u) << 10u) | (lowSurrogate - 0xDC00u));
        pos += 6u;
    } else if (codePoint >= 0xDC00u && codePoint <= 0xDFFFu) {
        return false;
    }

    return appendUtf8(out, codePoint);
}

enum class JsonValueKind {
    kString,
    kUnsigned,
    kNull,
    kOther,
};

struct JsonValue {
    JsonValueKind kind{JsonValueKind::kOther};
    std::string stringValue;
    uint64_t unsignedValue{0};
};

struct JsonObjectField {
    std::string name;
    JsonValue value;
};

bool parseJsonValue(
    std::string_view body,
    std::size_t& pos,
    JsonValue& outValue,
    uint32_t depth);

bool parseJsonStringAt(
    std::string_view body,
    std::size_t& pos,
    std::string& outValue) {
    if (pos >= body.size() || body[pos] != '"') {
        return false;
    }

    ++pos;
    std::string value;
    while (pos < body.size()) {
        const char ch = body[pos++];
        if (ch == '"') {
            outValue = std::move(value);
            return true;
        }
        if (ch == '\\') {
            if (pos >= body.size()) {
                return false;
            }

            const char escaped = body[pos++];
            switch (escaped) {
            case '"':
                value.push_back('"');
                break;
            case '\\':
                value.push_back('\\');
                break;
            case '/':
                value.push_back('/');
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
                if (!appendJsonUnicodeEscape(body, pos, value)) {
                    return false;
                }
                break;
            default:
                return false;
            }
        } else {
            if (static_cast<unsigned char>(ch) < 0x20u) {
                return false;
            }
            value.push_back(ch);
        }
    }

    return false;
}

bool consumeJsonLiteral(
    std::string_view body,
    std::size_t& pos,
    std::string_view literal) {
    if (body.substr(pos, literal.size()) != literal) {
        return false;
    }

    pos += literal.size();
    return true;
}

bool parseJsonNumberAt(
    std::string_view body,
    std::size_t& pos,
    JsonValue& outValue) {
    const std::size_t start = pos;
    bool negative = false;
    if (pos < body.size() && body[pos] == '-') {
        negative = true;
        ++pos;
    }

    const std::size_t digitsStart = pos;
    if (pos >= body.size()) {
        return false;
    }
    if (body[pos] == '0') {
        ++pos;
        if (pos < body.size() && isAsciiDigit(body[pos])) {
            return false;
        }
    } else if (body[pos] >= '1' && body[pos] <= '9') {
        while (pos < body.size() && isAsciiDigit(body[pos])) {
            ++pos;
        }
    } else {
        return false;
    }
    const std::size_t digitsEnd = pos;

    bool integralUnsigned = !negative;
    if (pos < body.size() && body[pos] == '.') {
        integralUnsigned = false;
        ++pos;
        if (pos >= body.size() || !isAsciiDigit(body[pos])) {
            return false;
        }
        while (pos < body.size() && isAsciiDigit(body[pos])) {
            ++pos;
        }
    }
    if (pos < body.size() && (body[pos] == 'e' || body[pos] == 'E')) {
        integralUnsigned = false;
        ++pos;
        if (pos < body.size() && (body[pos] == '+' || body[pos] == '-')) {
            ++pos;
        }
        if (pos >= body.size() || !isAsciiDigit(body[pos])) {
            return false;
        }
        while (pos < body.size() && isAsciiDigit(body[pos])) {
            ++pos;
        }
    }

    outValue = JsonValue{};
    if (!integralUnsigned) {
        return pos > start;
    }

    uint64_t value = 0;
    for (std::size_t i = digitsStart; i < digitsEnd; ++i) {
        const uint64_t digit = static_cast<uint64_t>(body[i] - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }

    outValue.kind = JsonValueKind::kUnsigned;
    outValue.unsignedValue = value;
    return true;
}

bool parseJsonObjectAt(
    std::string_view body,
    std::size_t& pos,
    uint32_t depth) {
    if (depth > 32u || pos >= body.size() || body[pos] != '{') {
        return false;
    }

    ++pos;
    pos = skipJsonWhitespace(body, pos);
    if (pos < body.size() && body[pos] == '}') {
        ++pos;
        return true;
    }

    for (;;) {
        std::string key;
        if (!parseJsonStringAt(body, pos, key)) {
            return false;
        }
        pos = skipJsonWhitespace(body, pos);
        if (pos >= body.size() || body[pos] != ':') {
            return false;
        }
        ++pos;

        JsonValue ignored;
        if (!parseJsonValue(body, pos, ignored, depth + 1u)) {
            return false;
        }

        pos = skipJsonWhitespace(body, pos);
        if (pos >= body.size()) {
            return false;
        }
        if (body[pos] == '}') {
            ++pos;
            return true;
        }
        if (body[pos] != ',') {
            return false;
        }
        ++pos;
        pos = skipJsonWhitespace(body, pos);
    }
}

bool parseJsonArrayAt(
    std::string_view body,
    std::size_t& pos,
    uint32_t depth) {
    if (depth > 32u || pos >= body.size() || body[pos] != '[') {
        return false;
    }

    ++pos;
    pos = skipJsonWhitespace(body, pos);
    if (pos < body.size() && body[pos] == ']') {
        ++pos;
        return true;
    }

    for (;;) {
        JsonValue ignored;
        if (!parseJsonValue(body, pos, ignored, depth + 1u)) {
            return false;
        }

        pos = skipJsonWhitespace(body, pos);
        if (pos >= body.size()) {
            return false;
        }
        if (body[pos] == ']') {
            ++pos;
            return true;
        }
        if (body[pos] != ',') {
            return false;
        }
        ++pos;
        pos = skipJsonWhitespace(body, pos);
    }
}

bool parseJsonValue(
    std::string_view body,
    std::size_t& pos,
    JsonValue& outValue,
    uint32_t depth) {
    if (depth > 32u) {
        return false;
    }

    pos = skipJsonWhitespace(body, pos);
    if (pos >= body.size()) {
        return false;
    }

    if (body[pos] == '"') {
        std::string value;
        if (!parseJsonStringAt(body, pos, value)) {
            return false;
        }
        outValue = JsonValue{};
        outValue.kind = JsonValueKind::kString;
        outValue.stringValue = std::move(value);
        return true;
    }
    if (body[pos] == '{') {
        if (!parseJsonObjectAt(body, pos, depth + 1u)) {
            return false;
        }
        outValue = JsonValue{};
        return true;
    }
    if (body[pos] == '[') {
        if (!parseJsonArrayAt(body, pos, depth + 1u)) {
            return false;
        }
        outValue = JsonValue{};
        return true;
    }
    if (body[pos] == 'n') {
        if (!consumeJsonLiteral(body, pos, "null")) {
            return false;
        }
        outValue = JsonValue{};
        outValue.kind = JsonValueKind::kNull;
        return true;
    }
    if (body[pos] == 't') {
        if (!consumeJsonLiteral(body, pos, "true")) {
            return false;
        }
        outValue = JsonValue{};
        return true;
    }
    if (body[pos] == 'f') {
        if (!consumeJsonLiteral(body, pos, "false")) {
            return false;
        }
        outValue = JsonValue{};
        return true;
    }
    if (body[pos] == '-' || isAsciiDigit(body[pos])) {
        return parseJsonNumberAt(body, pos, outValue);
    }

    return false;
}

bool parseTopLevelJsonObjectFields(
    std::string_view body,
    std::vector<JsonObjectField>& outFields) {
    std::size_t pos = skipJsonWhitespace(body, 0);
    if (pos >= body.size() || body[pos] != '{') {
        return false;
    }

    ++pos;
    std::vector<JsonObjectField> fields;
    pos = skipJsonWhitespace(body, pos);
    if (pos < body.size() && body[pos] == '}') {
        ++pos;
        pos = skipJsonWhitespace(body, pos);
        if (pos != body.size()) {
            return false;
        }
        outFields = std::move(fields);
        return true;
    }

    for (;;) {
        std::string key;
        if (!parseJsonStringAt(body, pos, key)) {
            return false;
        }
        pos = skipJsonWhitespace(body, pos);
        if (pos >= body.size() || body[pos] != ':') {
            return false;
        }
        ++pos;

        JsonValue value;
        if (!parseJsonValue(body, pos, value, 0)) {
            return false;
        }
        fields.push_back(JsonObjectField{std::move(key), std::move(value)});

        pos = skipJsonWhitespace(body, pos);
        if (pos >= body.size()) {
            return false;
        }
        if (body[pos] == '}') {
            ++pos;
            pos = skipJsonWhitespace(body, pos);
            if (pos != body.size()) {
                return false;
            }
            outFields = std::move(fields);
            return true;
        }
        if (body[pos] != ',') {
            return false;
        }
        ++pos;
        pos = skipJsonWhitespace(body, pos);
    }
}

const JsonValue* findJsonField(
    const std::vector<JsonObjectField>& fields,
    std::string_view key) {
    for (const JsonObjectField& field : fields) {
        if (field.name == key) {
            return &field.value;
        }
    }

    return nullptr;
}

bool getJsonStringField(
    const std::vector<JsonObjectField>& fields,
    std::string_view key,
    std::string& outValue) {
    const JsonValue* value = findJsonField(fields, key);
    if (value == nullptr || value->kind != JsonValueKind::kString) {
        return false;
    }

    outValue = value->stringValue;
    return true;
}

bool getJsonUnsignedField(
    const std::vector<JsonObjectField>& fields,
    std::string_view key,
    uint64_t& outValue) {
    const JsonValue* value = findJsonField(fields, key);
    if (value == nullptr || value->kind != JsonValueKind::kUnsigned) {
        return false;
    }

    outValue = value->unsignedValue;
    return true;
}

std::string joinHttpPath(const Core::MetaHttpEndpoint& endpoint, std::string_view path) {
    if (endpoint.basePath.empty()) {
        return std::string(path);
    }
    if (path.empty() || path.front() != '/') {
        return endpoint.basePath + "/" + std::string(path);
    }
    return endpoint.basePath + std::string(path);
}

std::string hostHeaderFor(const Core::MetaHttpEndpoint& endpoint) {
    std::string host = endpoint.host;
    if (host.find(':') != std::string::npos &&
        (host.empty() || host.front() != '[')) {
        host = "[" + host + "]";
    }
    if (endpoint.port != 80u) {
        host += ":" + std::to_string(endpoint.port);
    }
    return host;
}

void configureSocket(int fd) {
    timeval timeout{};
    timeout.tv_sec = kHttpSocketTimeoutMs / 1000;
    timeout.tv_usec = (kHttpSocketTimeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

void closeSocket(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

bool sendAll(int fd, std::string_view request) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        const ssize_t next = send(
            fd,
            request.data() + sent,
            request.size() - sent,
            flags);
        if (next < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (next == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(next);
    }
    return true;
}

bool receiveResponse(int fd, std::string& outResponse) {
    outResponse.clear();
    char buffer[4096];
    for (;;) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return true;
        }

        if (outResponse.size() + static_cast<std::size_t>(count) >
            kMaxHttpResponseBytes) {
            return false;
        }
        outResponse.append(buffer, static_cast<std::size_t>(count));
    }
}

bool parseHttpResponseHeaders(
    std::string_view response,
    int& outStatusCode,
    std::size_t& outBodyStart,
    bool& outChunked) {
    const std::size_t statusLineEnd = response.find("\r\n");
    if (statusLineEnd == std::string_view::npos ||
        !startsWith(response.substr(0, statusLineEnd), "HTTP/")) {
        return false;
    }

    const std::size_t firstSpace = response.find(' ');
    if (firstSpace == std::string_view::npos || firstSpace + 4u > statusLineEnd ||
        !isAsciiDigit(response[firstSpace + 1u]) ||
        !isAsciiDigit(response[firstSpace + 2u]) ||
        !isAsciiDigit(response[firstSpace + 3u])) {
        return false;
    }
    outStatusCode =
        (response[firstSpace + 1u] - '0') * 100 +
        (response[firstSpace + 2u] - '0') * 10 +
        (response[firstSpace + 3u] - '0');

    const std::size_t headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos) {
        return false;
    }

    outChunked = false;
    std::size_t lineStart = statusLineEnd + 2u;
    while (lineStart < headerEnd) {
        const std::size_t lineEnd = response.find("\r\n", lineStart);
        if (lineEnd == std::string_view::npos || lineEnd > headerEnd) {
            return false;
        }
        const std::string_view line =
            response.substr(lineStart, lineEnd - lineStart);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }

        const std::string_view name = trimHttpWhitespace(line.substr(0, colon));
        const std::string_view value = trimHttpWhitespace(line.substr(colon + 1u));
        if (equalsIgnoreCase(name, "Transfer-Encoding") &&
            containsTransferEncodingToken(value, "chunked")) {
            outChunked = true;
        }
        lineStart = lineEnd + 2u;
    }

    outBodyStart = headerEnd + 4u;
    return true;
}

bool setSocketNonBlocking(int fd, int& outOriginalFlags) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }

    outOriginalFlags = flags;
    return true;
}

bool restoreSocketFlags(int fd, int originalFlags) {
    return fcntl(fd, F_SETFL, originalFlags) == 0;
}

int remainingConnectTimeoutMs(
    std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const int remainingMs = static_cast<int>(remaining.count());
    return remainingMs > 0 ? remainingMs : 1;
}

bool waitForConnectReady(int fd, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLOUT;

        const int remainingMs = remainingConnectTimeoutMs(deadline);
        if (remainingMs <= 0) {
            return false;
        }

        const int result = poll(&descriptor, 1, remainingMs);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            return false;
        }

        int socketError = 0;
        socklen_t socketErrorSize = sizeof(socketError);
        if (getsockopt(
                fd,
                SOL_SOCKET,
                SO_ERROR,
                &socketError,
                &socketErrorSize) != 0) {
            return false;
        }

        if (socketError != 0) {
            errno = socketError;
            return false;
        }

        return (descriptor.revents & (POLLOUT | POLLERR | POLLHUP)) != 0;
    }
}

bool connectWithTimeout(
    int fd,
    const sockaddr* address,
    socklen_t addressLength,
    int timeoutMs) {
    int originalFlags = 0;
    if (!setSocketNonBlocking(fd, originalFlags)) {
        return false;
    }

    const int result = connect(fd, address, addressLength);
    if (result == 0) {
        return restoreSocketFlags(fd, originalFlags);
    }

    if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        restoreSocketFlags(fd, originalFlags);
        return false;
    }

    const bool connected = waitForConnectReady(fd, timeoutMs);
    const bool restored = restoreSocketFlags(fd, originalFlags);
    return connected && restored;
}

bool connectToEndpoint(const Core::MetaHttpEndpoint& endpoint, int& outFd) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &addresses) != 0) {
        return false;
    }

    int connectedFd = -1;
    for (addrinfo* candidate = addresses; candidate != nullptr;
         candidate = candidate->ai_next) {
        const int fd = socket(
            candidate->ai_family,
            candidate->ai_socktype,
            candidate->ai_protocol);
        if (fd < 0) {
            continue;
        }

        configureSocket(fd);
        if (connectWithTimeout(
                fd,
                candidate->ai_addr,
                candidate->ai_addrlen,
                kHttpSocketTimeoutMs)) {
            connectedFd = fd;
            break;
        }

        closeSocket(fd);
    }

    freeaddrinfo(addresses);
    if (connectedFd < 0) {
        return false;
    }

    outFd = connectedFd;
    return true;
}

std::string buildHttpPostRequest(
    const Core::MetaHttpEndpoint& endpoint,
    std::string_view internalToken,
    std::string_view path,
    std::string_view body) {
    const std::string requestPath = joinHttpPath(endpoint, path);
    std::string request;
    request.reserve(body.size() + requestPath.size() + internalToken.size() + 192u);
    request += "POST ";
    request += requestPath;
    request += " HTTP/1.1\r\nHost: ";
    request += hostHeaderFor(endpoint);
    request += "\r\nX-Internal-Token: ";
    request += internalToken;
    request += "\r\nContent-Type: application/json\r\nContent-Length: ";
    request += std::to_string(body.size());
    request += "\r\nConnection: close\r\n\r\n";
    request += body;
    return request;
}

}  // namespace

namespace Core {

bool parseMetaHttpEndpoint(std::string_view baseUrl, MetaHttpEndpoint& outEndpoint) {
    if (!startsWith(baseUrl, kHttpScheme)) {
        return false;
    }

    const std::string_view withoutScheme = baseUrl.substr(kHttpScheme.size());
    if (withoutScheme.find_first_of("?# \t\r\n") != std::string_view::npos) {
        return false;
    }

    const std::size_t pathStart = withoutScheme.find('/');
    const std::string_view authority = pathStart == std::string_view::npos
        ? withoutScheme
        : withoutScheme.substr(0, pathStart);
    const std::string_view path = pathStart == std::string_view::npos
        ? std::string_view()
        : withoutScheme.substr(pathStart);

    if (path.find_first_of("?#\r\n") != std::string_view::npos) {
        return false;
    }

    std::string host;
    uint16_t port = 80;
    if (!parseAuthority(authority, host, port)) {
        return false;
    }

    outEndpoint = MetaHttpEndpoint{
        std::move(host),
        port,
        normalizeBasePath(path)};
    return true;
}

std::string buildMetaClaimRequestBody(const MetaSessionClaimRequest& request) {
    std::string body;
    body.reserve(request.gameSessionToken.size() + 64u);
    body += "{\"gameSessionToken\":";
    body += jsonString(request.gameSessionToken);
    body += ",\"connectionId\":";
    body += jsonString(std::to_string(request.connectionId));
    body += "}";
    return body;
}

std::string buildMetaReleaseRequestBody(const MetaSessionReleaseRequest& request) {
    std::string body;
    body.reserve(64u);
    body += "{\"accountId\":";
    body += std::to_string(request.accountId);
    body += ",\"connectionId\":";
    body += jsonString(std::to_string(request.connectionId));
    body += "}";
    return body;
}

std::string buildMetaRenewRequestBody(const MetaSessionRenewRequest& request) {
    std::string body;
    body.reserve(64u);
    body += "{\"accountId\":";
    body += std::to_string(request.accountId);
    body += ",\"connectionId\":";
    body += jsonString(std::to_string(request.connectionId));
    body += "}";
    return body;
}

bool parseMetaClaimResponseBody(
    std::string_view body,
    MetaSessionClaimResult& outResult) {
    MetaSessionClaimResult result;
    std::vector<JsonObjectField> fields;
    if (!parseTopLevelJsonObjectFields(body, fields)) {
        return false;
    }

    std::string status;
    if (!getJsonStringField(fields, "status", status)) {
        return false;
    }

    if (status == "Rejected") {
        outResult = result;
        return true;
    }
    if (status != "Accepted") {
        return false;
    }

    uint64_t accountId = 0;
    uint64_t reservationExpiresAt = 0;
    std::string nickname;
    if (!getJsonUnsignedField(fields, "accountId", accountId) ||
        !getJsonStringField(fields, "nickname", nickname) ||
        !getJsonUnsignedField(fields, "reservationExpiresAt", reservationExpiresAt) ||
        accountId == 0u || nickname.empty() || reservationExpiresAt == 0u) {
        return false;
    }

    result.accepted = true;
    result.profile.accountId = accountId;
    result.profile.nickname = std::move(nickname);
    result.reservationExpiresAtUnixMs = reservationExpiresAt;
    outResult = std::move(result);
    return true;
}

bool parseMetaHttpResponse(
    std::string_view response,
    int& outStatusCode,
    std::string& outBody) {
    outStatusCode = 0;
    outBody.clear();

    std::size_t bodyStart = 0;
    bool chunked = false;
    if (!parseHttpResponseHeaders(response, outStatusCode, bodyStart, chunked)) {
        return false;
    }

    const std::string_view encodedBody = response.substr(bodyStart);
    if (chunked) {
        return decodeChunkedBody(encodedBody, outBody);
    }

    outBody = std::string(encodedBody);
    return true;
}

MetaHttpSessionClaimClient::MetaHttpSessionClaimClient(
    MetaHttpEndpoint endpoint,
    std::string internalToken)
    : endpoint_(std::move(endpoint)),
      internalToken_(std::move(internalToken)) {}

void MetaHttpSessionClaimClient::claimGameSessionAsync(
    const MetaSessionClaimRequest& request,
    ClaimCallback callback) {
    if (!callback) {
        return;
    }

    MetaSessionClaimResult result;
    try {
        int statusCode = 0;
        std::string responseBody;
        if (postJson(
                endpoint_,
                internalToken_,
                kClaimPath,
                buildMetaClaimRequestBody(request),
                statusCode,
                responseBody) &&
            statusCode == 200) {
            MetaSessionClaimResult parsed;
            if (parseMetaClaimResponseBody(responseBody, parsed)) {
                result = std::move(parsed);
            }
        }
    } catch (...) {
        result = MetaSessionClaimResult{};
    }

    callback(std::move(result));
}

void MetaHttpSessionClaimClient::releaseGameSessionAsync(
    const MetaSessionReleaseRequest& request) {
    MetaHttpEndpoint endpoint = endpoint_;
    std::string internalToken = internalToken_;
    std::string body = buildMetaReleaseRequestBody(request);

    try {
        std::thread(
            [endpoint = std::move(endpoint),
             internalToken = std::move(internalToken),
             body = std::move(body)]() mutable {
                int statusCode = 0;
                std::string responseBody;
                MetaHttpSessionClaimClient::postJson(
                    endpoint,
                    internalToken,
                    kReleasePath,
                    body,
                    statusCode,
                    responseBody);
            })
            .detach();
    } catch (...) {
    }
}

void MetaHttpSessionClaimClient::renewGameSessionAsync(
    const MetaSessionRenewRequest& request) {
    MetaHttpEndpoint endpoint = endpoint_;
    std::string internalToken = internalToken_;
    std::string body = buildMetaRenewRequestBody(request);

    try {
        std::thread(
            [endpoint = std::move(endpoint),
             internalToken = std::move(internalToken),
             body = std::move(body)]() mutable {
                int statusCode = 0;
                std::string responseBody;
                MetaHttpSessionClaimClient::postJson(
                    endpoint,
                    internalToken,
                    kRenewPath,
                    body,
                    statusCode,
                    responseBody);
            })
            .detach();
    } catch (...) {
    }
}

bool MetaHttpSessionClaimClient::postJson(
    const MetaHttpEndpoint& endpoint,
    std::string_view internalToken,
    std::string_view path,
    std::string_view body,
    int& outStatusCode,
    std::string& outBody) {
    outStatusCode = 0;
    outBody.clear();

    if (endpoint.host.empty() || containsHeaderLineBreak(endpoint.host) ||
        containsHeaderLineBreak(internalToken) || containsHeaderLineBreak(path)) {
        return false;
    }

    int fd = -1;
    if (!connectToEndpoint(endpoint, fd)) {
        return false;
    }

    const std::string request =
        buildHttpPostRequest(endpoint, internalToken, path, body);
    std::string response;
    const bool ok = sendAll(fd, request) && receiveResponse(fd, response);
    closeSocket(fd);
    if (!ok) {
        return false;
    }

    return parseMetaHttpResponse(response, outStatusCode, outBody);
}

}  // namespace Core
