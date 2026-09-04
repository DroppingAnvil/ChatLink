// nspire_link — an RAII C++ wrapper over libnspire's C API.
//
// The Nspire is not a mass-storage or HID device. It exposes a vendor-specific
// bulk protocol (TI calls it NavNet) whose useful primitives are filesystem
// operations and screen capture. libnspire implements that protocol; this file
// wraps it so the rest of ChatLink deals in std::string and expected-style
// results rather than raw pointers and errno-alikes.
//
// Windows driver note: libusb can only claim the calculator once the device is
// bound to WinUSB. Out of the box Windows binds TI's own driver, and the two
// are mutually exclusive - installing WinUSB (via Zadig or similar) will stop
// TI's Student Software from seeing the calculator until you revert it.
// Device::open reports that case distinctly so the CLI can explain it.

#ifndef CHATLINK_NSPIRE_LINK_H
#define CHATLINK_NSPIRE_LINK_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct nspire_handle;

namespace chatlink::link {

// Outcome of a link operation. Mirrors libnspire's int-returning convention but
// carries the decoded message so callers never re-derive it.
struct Status {
    // Normalised to a positive NSPIRE_ERR_* value. libnspire itself returns
    // these negated, which Status::from undoes so callers can compare against
    // the enum constants directly.
    int         code = 0;              // 0 == NSPIRE_ERR_SUCCESS
    std::string message;

    bool ok() const { return code == 0; }
    explicit operator bool() const { return ok(); }

    static Status success() { return {}; }
    static Status from(int code, const std::string& context);
};

struct DirEntry {
    std::string   name;
    std::uint32_t size = 0;
    std::uint32_t date = 0;            // calculator-local timestamp
    bool          is_directory = false;
};

struct DeviceInfo {
    std::string   name;
    std::string   electronic_id;
    std::string   os_version;          // "major.minor.build"
    std::string   hardware;            // decoded nspire_type
    std::uint64_t storage_free = 0;
    std::uint64_t storage_total = 0;
    std::uint64_t ram_free = 0;
    std::uint64_t ram_total = 0;
    std::uint16_t lcd_width = 0;
    std::uint16_t lcd_height = 0;
    std::uint8_t  lcd_bpp = 0;
    std::string   battery;             // decoded nspire_battery
    bool          charging = false;
    // The extension the calculator expects on documents, without the dot.
    // Practically always "tns", but the device reports it, so we use it.
    std::string   file_extension;
};

struct Screenshot {
    std::uint16_t             width = 0;
    std::uint16_t             height = 0;
    std::uint8_t              bits_per_pixel = 0;
    std::vector<std::uint8_t> pixels;
};

// One connected calculator. Move-only: the underlying handle is exclusive.
class Device {
public:
    Device() = default;
    ~Device();

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Finds and claims the first attached Nspire (CX 0x0451:0xe012 or
    // CX II 0x0451:0xe022). NSPIRE_ERR_NODEVICE means either nothing is plugged
    // in or the device is not bound to WinUSB - see the driver note above.
    Status open();
    void   close();
    bool   isOpen() const { return handle_ != nullptr; }

    Status info(DeviceInfo& out);

    // Path form is calculator-absolute, e.g. "/documents" or
    // "/documents/chatlink/in.tns". The root listing is "/".
    Status list(const std::string& path, std::vector<DirEntry>& out);
    Status stat(const std::string& path, DirEntry& out);

    Status readFile(const std::string& path, std::vector<std::uint8_t>& out);
    Status writeFile(const std::string& path, const std::vector<std::uint8_t>& data);
    Status deleteFile(const std::string& path);
    Status makeDirectory(const std::string& path);
    Status removeDirectory(const std::string& path);

    Status capture(Screenshot& out);

private:
    nspire_handle* handle_ = nullptr;
};

// True when the status means "no calculator found", which the CLI treats as an
// expected condition worth explaining rather than an error worth dumping.
bool isNoDevice(const Status& s);

} // namespace chatlink::link

#endif // CHATLINK_NSPIRE_LINK_H
