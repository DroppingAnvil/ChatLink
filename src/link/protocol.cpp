#include "protocol.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace chatlink::protocol {
namespace {

// Running out of buffer while scanning a header field means the writer has not
// finished yet, which is a retry - not the same as a field that is present but
// not a number. Conflating the two would report a half-written file as corrupt.
enum class Field { Ok, Incomplete, Malformed };

// Parses a decimal field terminated by `delimiter`, advancing `cursor` past it.
Field takeDecimal(const char* data, std::size_t size, std::size_t& cursor,
                  char delimiter, std::uint64_t& out) {
    const std::size_t start = cursor;
    while (cursor < size && data[cursor] != delimiter) ++cursor;

    // No delimiter anywhere in what we have: the rest is still in flight.
    if (cursor >= size) return Field::Incomplete;
    if (cursor == start) return Field::Malformed;   // empty field

    const auto result = std::from_chars(data + start, data + cursor, out);
    if (result.ec != std::errc() || result.ptr != data + cursor) {
        return Field::Malformed;                    // non-numeric, or overflow
    }

    ++cursor;  // step over the delimiter
    return Field::Ok;
}

} // namespace

std::vector<std::uint8_t> encode(const Message& message) {
    std::string header = std::string(kMagic) + " " + std::to_string(message.sequence)
                       + " " + std::to_string(message.payload.size()) + "\n";

    std::vector<std::uint8_t> out;
    out.reserve(header.size() + message.payload.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), message.payload.begin(), message.payload.end());
    return out;
}

DecodeResult decode(const std::vector<std::uint8_t>& raw) {
    DecodeResult result;
    if (raw.empty()) {
        result.error = DecodeError::Empty;
        return result;
    }

    const auto* data = reinterpret_cast<const char*>(raw.data());
    const std::size_t size = raw.size();

    // Compare only the bytes actually present. A short buffer whose prefix
    // already disagrees is a foreign file, not a half-written one - reporting it
    // as Incomplete would make the caller retry a file that can never parse.
    const std::size_t magic_len = std::strlen(kMagic);
    const std::size_t comparable = std::min(size, magic_len);
    if (std::memcmp(data, kMagic, comparable) != 0) {
        result.error = DecodeError::BadMagic;
        return result;
    }
    if (size <= magic_len) {
        result.error = DecodeError::Incomplete;   // prefix matches, more to come
        return result;
    }
    if (data[magic_len] != ' ') {
        result.error = DecodeError::BadMagic;
        return result;
    }

    std::size_t cursor = magic_len + 1;
    std::uint64_t sequence = 0;
    std::uint64_t length = 0;

    for (const auto& [delimiter, target] :
         {std::pair{' ', &sequence}, std::pair{'\n', &length}}) {
        const Field field = takeDecimal(data, size, cursor, delimiter, *target);
        if (field == Field::Incomplete) {
            result.error = DecodeError::Incomplete;
            return result;
        }
        if (field == Field::Malformed) {
            result.error = DecodeError::BadHeader;
            return result;
        }
    }

    if (size - cursor < length) {
        // The writer has not finished. Retrying is correct; consuming would
        // hand a truncated prompt to the model.
        result.error = DecodeError::Incomplete;
        return result;
    }

    Message message;
    message.sequence = sequence;
    // Bytes past the declared length are leftovers from a longer previous
    // message and are deliberately ignored.
    message.payload.assign(data + cursor, data + cursor + length);
    result.message = std::move(message);
    return result;
}

const char* describe(DecodeError error) {
    switch (error) {
        case DecodeError::Empty:      return "file is empty";
        case DecodeError::BadMagic:   return "not a ChatLink message";
        case DecodeError::BadHeader:  return "malformed header";
        case DecodeError::Incomplete: return "message still being written";
    }
    return "unknown";
}

} // namespace chatlink::protocol
