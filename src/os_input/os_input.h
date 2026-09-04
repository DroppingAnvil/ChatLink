// os_input — synthesises real OS-level keyboard and mouse input on Windows.
//
// This is deliberately standalone: it knows nothing about calculators, USB or
// models. Both the model bridge (typing a prompt into a chat window) and
// macropad mode (letting the calculator drive the PC directly) call into it.
//
// Everything goes through SendInput, so the events enter the same queue as a
// physical keyboard. Two consequences worth knowing:
//   * Input lands in whatever window currently has focus. There is no target
//     parameter, by design — retargeting is the caller's job.
//   * Windows refuses synthetic input into a process running at a higher
//     integrity level (UIPI). Typing into an elevated window from a normal
//     process silently drops the events; Driver reports that as a failure
//     rather than pretending it worked.

#ifndef CHATLINK_OS_INPUT_H
#define CHATLINK_OS_INPUT_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chatlink::osinput {

// Modifier flags, combinable.
enum class Mod : std::uint8_t {
    None  = 0,
    Ctrl  = 1u << 0,
    Alt   = 1u << 1,
    Shift = 1u << 2,
    Win   = 1u << 3,
};

constexpr Mod operator|(Mod a, Mod b) {
    return static_cast<Mod>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool has(Mod set, Mod flag) {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}

enum class MouseButton { Left, Right, Middle };

// A key press with optional modifiers, e.g. ctrl+shift+Escape.
struct KeyStroke {
    std::uint16_t vk = 0;      // Win32 virtual-key code
    Mod           mods = Mod::None;
};

// Pacing. Real keyboards are not instantaneous, and some targets (Electron
// apps, browser text editors, remote desktops) drop characters delivered with
// no gap at all. Non-zero defaults trade a little speed for reliability.
struct Pacing {
    unsigned key_delay_ms   = 2;    // between individual keystrokes
    unsigned chunk_size     = 64;   // events per SendInput call
    unsigned chunk_pause_ms = 0;    // between batches
};

struct Result {
    bool        ok = false;
    std::size_t events_sent = 0;
    std::string error;              // populated only when ok == false

    explicit operator bool() const { return ok; }
};

class Driver {
public:
    Driver() = default;

    // When enabled, every call validates and logs its work but injects nothing.
    // Useful for testing a pipeline that would otherwise type into your editor.
    void setDryRun(bool on) { dry_run_ = on; }
    bool dryRun() const { return dry_run_; }

    void setPacing(const Pacing& p) { pacing_ = p; }
    const Pacing& pacing() const { return pacing_; }

    // Types UTF-8 text as Unicode keystrokes. Layout-independent: characters
    // are delivered by codepoint, so this produces the same result on a QWERTY,
    // AZERTY or Dvorak layout without any remapping.
    //
    // '\n' is sent as VK_RETURN and '\t' as VK_TAB, because the literal control
    // codes do nothing in most text fields. '\r' is ignored so CRLF does not
    // produce a doubled newline.
    Result typeText(std::string_view utf8);

    // Presses and releases a single key with modifiers held around it.
    Result tapKey(KeyStroke stroke);

    // Convenience for the common chord case, e.g. sendChord(Mod::Ctrl, 'V').
    Result sendChord(Mod mods, std::uint16_t vk) { return tapKey({vk, mods}); }

    Result mouseMoveAbsolute(int x, int y);          // virtual-screen pixels
    Result mouseMoveRelative(int dx, int dy);
    Result mouseClick(MouseButton button);
    Result mouseScroll(int wheel_clicks);             // +up / -down

private:
    // The INPUT batching helper lives in the .cpp so this header stays free of
    // <windows.h>, which would otherwise leak macros like max() into callers.
    Pacing pacing_{};
    bool   dry_run_ = false;
};

// Parses a human-readable key spec into a KeyStroke.
// Accepts: "ctrl+shift+t", "alt+F4", "enter", "esc", "win+d", "a".
// Modifier names: ctrl/control, alt, shift, win/meta/super (case-insensitive).
// Returns nullopt if the spec names no key or an unknown key.
std::optional<KeyStroke> parseKeySpec(std::string_view spec);

// Converts UTF-8 to UTF-16, replacing malformed sequences with U+FFFD.
// Exposed because callers often need the same conversion for window titles.
std::wstring utf8ToUtf16(std::string_view utf8);

} // namespace chatlink::osinput

#endif // CHATLINK_OS_INPUT_H
