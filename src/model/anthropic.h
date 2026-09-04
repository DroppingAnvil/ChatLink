// anthropic — asks Claude a question over the Messages API.
//
// Raw HTTPS via WinHTTP rather than an SDK: there is no official Anthropic SDK
// for C++, and WinHTTP ships with MinGW, so this adds no dependency to build.
//
// The calculator is the reason for most of the configuration here. Its console
// is 53 columns by 30 rows of plain monospaced text, with no markdown renderer
// and no scrollback worth speaking of, so the system prompt asks for short
// plain-ASCII answers and max_tokens is deliberately small.

#ifndef CHATLINK_ANTHROPIC_H
#define CHATLINK_ANTHROPIC_H

#include <string>

namespace chatlink::model {

struct Config {
    std::string api_key;

    // Opus 5 is the default. Thinking is on by default on this model, so the
    // request sends no `thinking` field; `effort` controls how much of it runs.
    std::string model = "claude-opus-5";

    // Low effort suits a short-answer chat route and keeps the round trip
    // quick, which matters when someone is waiting at a calculator.
    std::string effort = "low";

    // Small on purpose: replies have to fit a 53x30 character screen.
    int max_tokens = 1024;

    // Seconds to wait for the whole exchange.
    int timeout_seconds = 120;

    std::string system_prompt =
        "You are answering questions shown on a TI-Nspire CX graphing calculator.\n"
        "\n"
        "The screen is 53 characters wide and 30 lines tall, plain monospaced "
        "text. There is no markdown rendering, no styling, and very little room.\n"
        "\n"
        "Rules:\n"
        "- Answer in under 120 words. Usually far less.\n"
        "- Lead with the answer itself, then at most a sentence or two of support.\n"
        "- Plain ASCII only. No markdown, no asterisks, no backticks, no emoji, "
        "no tables, no box drawing.\n"
        "- Prefer short lines. If you list things, use \"- \" and keep each item "
        "to one line.\n"
        "- If a question genuinely needs a long answer, give the short version "
        "and say what you left out.";
};

struct Result {
    bool        ok = false;
    std::string text;      // the reply, when ok
    std::string error;     // why it failed, otherwise

    explicit operator bool() const { return ok; }
};

// Sends one prompt and returns the reply. Blocking.
Result ask(const Config& config, const std::string& prompt);

// Reads ANTHROPIC_API_KEY from the environment. Empty if unset.
std::string apiKeyFromEnvironment();

} // namespace chatlink::model

#endif // CHATLINK_ANTHROPIC_H
