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

// protocol — the file-based message channel between calculator and PC.
//
// The NavNet link offers no stream, only whole-file reads and writes, so the
// channel is a pair of files polled from both ends:
//
//   /chatlink/req.tns   calculator writes, PC reads   (prompts)
//   /chatlink/rsp.tns   PC writes, calculator reads   (replies)
//
// Note the paths: over NavNet the filesystem root IS the documents folder, so
// there is no /documents prefix. Verified against a CX - "ls /" lists the
// documents the calculator shows on its home screen.
//
// Both are plain UTF-8 text despite the .tns extension - the Nspire only shows
// and transfers files ending in .tns, but nothing inspects the contents, and an
// Ndless program reads them as ordinary files.
//
// Framing
// -------
// A whole-file write is not atomic from the reader's point of view: a poll can
// land while the writer is mid-transfer and see a truncated file. Rather than
// lock (there is no locking primitive across this link), each message carries a
// header that makes a partial read detectable and a repeated read idempotent:
//
//   CHATLINK/1 <seq> <byte-length>\n
//   <payload bytes>
//
//   seq          decimal, strictly increasing per direction. The reader ignores
//                any message whose seq it has already handled, which is what
//                makes polling the same file repeatedly safe.
//   byte-length  decimal length of the payload in bytes, excluding this header.
//                A reader that sees fewer bytes than promised treats the file as
//                still being written and retries rather than consuming a
//                truncated prompt.
//
// The payload is opaque UTF-8. Trailing bytes beyond byte-length are ignored,
// which is what lets a shorter message safely overwrite a longer one without
// the writer having to truncate the file first.

#ifndef CHATLINK_PROTOCOL_H
#define CHATLINK_PROTOCOL_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chatlink::protocol {

// Calculator-side paths. The directory must exist before either side writes;
// ChatLink creates it during setup.
inline constexpr const char* kDirectory    = "/chatlink";
inline constexpr const char* kRequestPath  = "/chatlink/req.tns";
inline constexpr const char* kResponsePath = "/chatlink/rsp.tns";

inline constexpr const char* kMagic = "CHATLINK/1";

struct Message {
    std::uint64_t sequence = 0;
    std::string   payload;
};

// Serialises a message with its header, ready to be written as a whole file.
std::vector<std::uint8_t> encode(const Message& message);

// Reason a buffer did not yield a message. Incomplete is the interesting one:
// it means "retry", whereas the others mean "this file is not ours".
enum class DecodeError {
    Empty,          // zero bytes, or a file that has not been written yet
    BadMagic,       // not a ChatLink message
    BadHeader,      // header present but malformed
    Incomplete,     // header promises more payload than the file holds - retry
};

struct DecodeResult {
    std::optional<Message> message;
    DecodeError            error = DecodeError::Empty;

    bool ok() const { return message.has_value(); }
    // True when the right response is to poll again rather than to give up.
    bool shouldRetry() const { return !ok() && error == DecodeError::Incomplete; }
};

DecodeResult decode(const std::vector<std::uint8_t>& raw);

// Human-readable form, for logs and error reporting.
const char* describe(DecodeError error);

} // namespace chatlink::protocol

#endif // CHATLINK_PROTOCOL_H
