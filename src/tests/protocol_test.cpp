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

// Round-trip and robustness tests for the calculator/PC message framing.
// No test framework: this runs in the build and returns non-zero on failure.

#include "link/protocol.h"
#include "os_input/os_input.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL  %s\n", what.c_str());
        ++failures;
    } else {
        std::printf("ok    %s\n", what.c_str());
    }
}

std::vector<std::uint8_t> bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

void testRoundTrip() {
    using namespace chatlink::protocol;

    Message sent{42, "what is the derivative of x^3?"};
    const DecodeResult got = decode(encode(sent));

    check(got.ok(), "round-trip decodes");
    if (got.ok()) {
        check(got.message->sequence == 42, "round-trip preserves sequence");
        check(got.message->payload == sent.payload, "round-trip preserves payload");
    }
}

void testEmptyPayload() {
    using namespace chatlink::protocol;

    const DecodeResult got = decode(encode(Message{7, ""}));
    check(got.ok() && got.message->payload.empty(), "empty payload is a valid message");
}

void testUtf8Payload() {
    using namespace chatlink::protocol;

    // Length is in bytes, not characters - a multi-byte payload must survive.
    const std::string text = "naive pi test: \xCF\x80 and an em dash \xE2\x80\x94";
    const DecodeResult got = decode(encode(Message{1, text}));
    check(got.ok() && got.message->payload == text, "multi-byte UTF-8 survives framing");
}

void testTruncationRetries() {
    using namespace chatlink::protocol;

    const auto full = encode(Message{3, "a reasonably long prompt body"});

    // Every proper prefix must ask for a retry rather than yield a short read.
    bool all_retry = true;
    for (std::size_t cut = 1; cut < full.size(); ++cut) {
        const std::vector<std::uint8_t> partial(full.begin(), full.begin() + cut);
        const DecodeResult got = decode(partial);
        if (got.ok() || !got.shouldRetry()) {
            std::printf("      truncation at %zu bytes: ok=%d retry=%d\n",
                        cut, static_cast<int>(got.ok()), static_cast<int>(got.shouldRetry()));
            all_retry = false;
        }
    }
    check(all_retry, "every truncated prefix asks for a retry, never a short read");
}

void testStaleTrailingBytes() {
    using namespace chatlink::protocol;

    // A short message overwriting a longer one leaves trailing junk behind.
    auto shorter = encode(Message{9, "hi"});
    const auto leftovers = bytes("...trailing bytes from a previous, longer message...");
    shorter.insert(shorter.end(), leftovers.begin(), leftovers.end());

    const DecodeResult got = decode(shorter);
    check(got.ok() && got.message->payload == "hi",
          "trailing bytes from a longer previous message are ignored");
}

void testRejectsForeignData() {
    using namespace chatlink::protocol;

    check(decode(bytes("PK\x03\x04 some .tns zip")).error == DecodeError::BadMagic,
          "a real .tns file is rejected as foreign");
    check(decode({}).error == DecodeError::Empty, "empty buffer reports Empty");
    check(decode(bytes("CHATLINK/1 notanumber 5\nhello")).error == DecodeError::BadHeader,
          "non-numeric sequence is a bad header");
    check(decode(bytes("CHATLINK/1 1 notanumber\nhello")).error == DecodeError::BadHeader,
          "non-numeric length is a bad header");
}

void testKeySpecParsing() {
    using chatlink::osinput::Mod;
    using chatlink::osinput::has;
    using chatlink::osinput::parseKeySpec;

    const auto chord = parseKeySpec("ctrl+shift+t");
    check(chord.has_value(), "parses ctrl+shift+t");
    if (chord) {
        check(has(chord->mods, Mod::Ctrl) && has(chord->mods, Mod::Shift),
              "ctrl+shift+t carries both modifiers");
        check(!has(chord->mods, Mod::Alt), "ctrl+shift+t does not carry alt");
    }

    check(parseKeySpec("enter").has_value(), "parses a named key");
    check(parseKeySpec("f12").has_value(), "parses a function key");
    check(!parseKeySpec("").has_value(), "rejects an empty spec");
    check(!parseKeySpec("nonsensekey").has_value(), "rejects an unknown key name");
    check(!parseKeySpec("f99").has_value(), "rejects an out-of-range function key");
}

} // namespace

int main() {
    testRoundTrip();
    testEmptyPayload();
    testUtf8Payload();
    testTruncationRetries();
    testStaleTrailingBytes();
    testRejectsForeignData();
    testKeySpecParsing();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
