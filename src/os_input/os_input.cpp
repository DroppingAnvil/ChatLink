#include "os_input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

namespace chatlink::osinput {
namespace {

void sleepMs(unsigned ms) {
    if (ms) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

INPUT unicodeEvent(wchar_t unit, bool key_up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;                      // must be 0 for KEYEVENTF_UNICODE
    in.ki.wScan = unit;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (key_up ? KEYEVENTF_KEYUP : 0u);
    return in;
}

// Arrows, navigation and the right-hand modifiers live in the extended set.
// Without the flag the OS delivers the numpad twin instead: VK_HOME arrives as
// numpad-7 when NumLock is off.
bool isExtendedKey(WORD vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_INSERT: case VK_DELETE:
        case VK_RCONTROL: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_APPS:
        case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

INPUT vkEvent(WORD vk, bool key_up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (key_up ? KEYEVENTF_KEYUP : 0u)
                  | (isExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
    return in;
}

std::string lastErrorMessage(const char* what, DWORD code) {
    char* buffer = nullptr;
    const DWORD len = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                    | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<char*>(&buffer), 0, nullptr);

    std::string detail = (len && buffer) ? std::string(buffer, len) : "unknown error";
    if (buffer) LocalFree(buffer);
    while (!detail.empty()) {
        const char back = detail.back();
        if (back == 0x0A || back == 0x0D || back == '.' || back == ' ') detail.pop_back();
        else break;
    }
    return std::string(what) + " failed (error " + std::to_string(code) + ": " + detail + ")";
}

// Sends events in batches. A short batch means the OS refused the rest, which
// in practice is almost always UIPI blocking input to an elevated window.
Result submitEvents(std::vector<INPUT>& events, const Pacing& pacing, bool dry_run) {
    Result result;
    if (events.empty()) {
        result.ok = true;
        return result;
    }
    if (dry_run) {
        result.ok = true;
        result.events_sent = events.size();
        return result;
    }

    const std::size_t chunk = std::max<std::size_t>(1, pacing.chunk_size);
    for (std::size_t offset = 0; offset < events.size(); offset += chunk) {
        const auto count = static_cast<UINT>(std::min(chunk, events.size() - offset));
        const UINT sent = SendInput(count, events.data() + offset, sizeof(INPUT));
        result.events_sent += sent;

        if (sent != count) {
            const DWORD code = GetLastError();
            result.error = lastErrorMessage("SendInput", code);
            if (code == ERROR_ACCESS_DENIED) {
                result.error += " - the focused window most likely runs elevated, "
                                "and synthetic input cannot cross that boundary.";
            }
            return result;
        }
        if (offset + chunk < events.size()) sleepMs(pacing.chunk_pause_ms);
    }

    result.ok = true;
    return result;
}

const std::unordered_map<std::string, WORD>& namedKeys() {
    static const std::unordered_map<std::string, WORD> table = {
            {"enter", VK_RETURN},   {"return", VK_RETURN}, {"tab", VK_TAB},
            {"esc", VK_ESCAPE},     {"escape", VK_ESCAPE}, {"space", VK_SPACE},
            {"backspace", VK_BACK}, {"bksp", VK_BACK},     {"delete", VK_DELETE},
            {"del", VK_DELETE},     {"insert", VK_INSERT}, {"home", VK_HOME},
            {"end", VK_END},        {"pageup", VK_PRIOR},  {"pagedown", VK_NEXT},
            {"up", VK_UP},          {"down", VK_DOWN},     {"left", VK_LEFT},
            {"right", VK_RIGHT},    {"printscreen", VK_SNAPSHOT},
    };
    return table;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Builds the down/up pair for one UTF-16 code unit, mapping the two control
// characters that Unicode injection cannot express.
void appendCharacter(std::vector<INPUT>& out, wchar_t unit) {
    if (unit == 0x0A || unit == 0x09) {
        const WORD vk = (unit == 0x0A) ? VK_RETURN : VK_TAB;
        out.push_back(vkEvent(vk, false));
        out.push_back(vkEvent(vk, true));
    } else {
        out.push_back(unicodeEvent(unit, false));
        out.push_back(unicodeEvent(unit, true));
    }
}

} // namespace

std::wstring utf8ToUtf16(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), needed);
    return out;
}

Result Driver::typeText(std::string_view utf8) {
    const std::wstring wide = utf8ToUtf16(utf8);

    // With no inter-key delay the whole string can go out in batches. With a
    // delay we submit per character, so the pause actually falls between
    // keystrokes rather than after the entire block.
    if (pacing_.key_delay_ms == 0) {
        std::vector<INPUT> events;
        events.reserve(wide.size() * 2);
        for (const wchar_t unit : wide) {
            if (unit == 0x0D) continue;          // swallow CR so CRLF is one newline
            appendCharacter(events, unit);
        }
        return submitEvents(events, pacing_, dry_run_);
    }

    Result total;
    total.ok = true;
    for (const wchar_t unit : wide) {
        if (unit == 0x0D) continue;

        std::vector<INPUT> pair;
        appendCharacter(pair, unit);

        const Result step = submitEvents(pair, pacing_, dry_run_);
        total.events_sent += step.events_sent;
        if (!step.ok) {
            total.ok = false;
            total.error = step.error;
            return total;
        }
        sleepMs(pacing_.key_delay_ms);
    }
    return total;
}

Result Driver::tapKey(KeyStroke stroke) {
    if (stroke.vk == 0) {
        Result r;
        r.error = "tapKey called with no virtual-key code";
        return r;
    }

    // Modifiers go down in a fixed order and come back up in reverse, so a
    // chord that fails part-way still unwinds the modifiers it managed to press.
    const std::array<std::pair<Mod, WORD>, 4> modifiers{{
            {Mod::Ctrl, VK_CONTROL},
            {Mod::Alt, VK_MENU},
            {Mod::Shift, VK_SHIFT},
            {Mod::Win, VK_LWIN},
    }};

    std::vector<INPUT> events;
    for (const auto& entry : modifiers) {
        if (has(stroke.mods, entry.first)) events.push_back(vkEvent(entry.second, false));
    }
    events.push_back(vkEvent(stroke.vk, false));
    events.push_back(vkEvent(stroke.vk, true));
    for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) {
        if (has(stroke.mods, it->first)) events.push_back(vkEvent(it->second, true));
    }

    // A chord must not be split across SendInput calls, or the target can
    // observe the key press before the modifier lands.
    Pacing atomic = pacing_;
    atomic.chunk_size = events.size();
    return submitEvents(events, atomic, dry_run_);
}

Result Driver::mouseMoveAbsolute(int x, int y) {
    // MOUSEEVENTF_ABSOLUTE coordinates are normalised to 0..65535 across the
    // whole virtual screen, which is what makes this correct on multi-monitor
    // setups and with monitors placed at negative coordinates.
    const int origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(2, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int height = std::max(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>((static_cast<double>(x - origin_x) * 65535.0)
                                 / static_cast<double>(width - 1));
    in.mi.dy = static_cast<LONG>((static_cast<double>(y - origin_y) * 65535.0)
                                 / static_cast<double>(height - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    std::vector<INPUT> events{in};
    return submitEvents(events, pacing_, dry_run_);
}

Result Driver::mouseMoveRelative(int dx, int dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;

    std::vector<INPUT> events{in};
    return submitEvents(events, pacing_, dry_run_);
}

Result Driver::mouseClick(MouseButton button) {
    DWORD down = MOUSEEVENTF_LEFTDOWN;
    DWORD up = MOUSEEVENTF_LEFTUP;
    switch (button) {
        case MouseButton::Right:  down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP;  break;
        case MouseButton::Middle: down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; break;
        case MouseButton::Left:   break;
    }

    INPUT press{};
    INPUT release{};
    press.type = INPUT_MOUSE;
    release.type = INPUT_MOUSE;
    press.mi.dwFlags = down;
    release.mi.dwFlags = up;

    std::vector<INPUT> events{press, release};
    return submitEvents(events, pacing_, dry_run_);
}

Result Driver::mouseScroll(int wheel_clicks) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = static_cast<DWORD>(wheel_clicks * WHEEL_DELTA);
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;

    std::vector<INPUT> events{in};
    return submitEvents(events, pacing_, dry_run_);
}

std::optional<KeyStroke> parseKeySpec(std::string_view spec) {
    KeyStroke stroke;
    std::string remaining = toLower(spec);
    if (remaining.empty()) return std::nullopt;

    // Peel modifiers off the front until what is left is the key itself. The
    // trailing-token guard means a spec of "ctrl++" still finds the plus key.
    for (;;) {
        const std::size_t plus = remaining.find('+');
        if (plus == std::string::npos || plus + 1 >= remaining.size()) break;

        const std::string token = remaining.substr(0, plus);
        if (token == "ctrl" || token == "control")      stroke.mods = stroke.mods | Mod::Ctrl;
        else if (token == "alt")                        stroke.mods = stroke.mods | Mod::Alt;
        else if (token == "shift")                      stroke.mods = stroke.mods | Mod::Shift;
        else if (token == "win" || token == "meta" || token == "super")
                                                        stroke.mods = stroke.mods | Mod::Win;
        else break;

        remaining.erase(0, plus + 1);
    }

    if (remaining.empty()) return std::nullopt;

    const auto named = namedKeys().find(remaining);
    if (named != namedKeys().end()) {
        stroke.vk = named->second;
        return stroke;
    }

    // Function keys F1..F24.
    if (remaining.size() >= 2 && remaining[0] == 'f') {
        const std::string digits = remaining.substr(1);
        const bool all_digits = !digits.empty()
                && std::all_of(digits.begin(), digits.end(),
                               [](unsigned char c) { return std::isdigit(c) != 0; });
        if (all_digits) {
            const int n = std::stoi(digits);
            if (n >= 1 && n <= 24) {
                stroke.vk = static_cast<WORD>(VK_F1 + (n - 1));
                return stroke;
            }
        }
    }

    // A single printable character: ask the active layout for its virtual key.
    if (remaining.size() == 1) {
        const SHORT scan = VkKeyScanW(static_cast<wchar_t>(remaining[0]));
        if (scan != -1) {
            stroke.vk = static_cast<WORD>(scan & 0xFF);
            // The high byte reports which modifiers the layout needs to reach
            // this character; fold in shift so specs like "?" work anywhere.
            if ((scan >> 8) & 1) stroke.mods = stroke.mods | Mod::Shift;
            return stroke;
        }
    }

    return std::nullopt;
}

} // namespace chatlink::osinput
