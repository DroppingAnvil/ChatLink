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

#include "nspire_link.h"

// libnspire's public headers carry no extern "C" guards of their own, so the
// declarations must be wrapped here or C++ mangles them and linking fails.
// libusb.h comes first and outside the block: it pulls in system headers and
// already declares its own C linkage, so wrapping it would be both redundant
// and risky.
#ifndef NSP_BACKEND_TI
// Only the legacy WinUSB transport pulls in libusb. It comes first and outside
// the block below: it drags in system headers and already declares its own C
// linkage, so wrapping it would be both redundant and risky.
#include <libusb.h>
#endif
extern "C" {
#include <nspire.h>
}

#include <cstdlib>
#include <cstring>

namespace chatlink::link {
namespace {

// libnspire has no "how big is this file" call separate from reading it, so
// reads are sized from the directory entry and this cap keeps a bogus size from
// turning into an enormous allocation. Nspire flash is a few hundred MB total.
constexpr std::size_t kMaxFileBytes = 64u * 1024u * 1024u;

std::string describeHardware(int hw_type) {
    switch (hw_type) {
        case NSPIRE_CAS:      return "Nspire CAS";
        case NSPIRE_NONCAS:   return "Nspire";
        case NSPIRE_CASCX:    return "Nspire CX CAS";
        case NSPIRE_NONCASCX: return "Nspire CX";
        default:              return "unknown (0x" + std::to_string(hw_type) + ")";
    }
}

std::string describeBattery(int status) {
    switch (status) {
        case NSPIRE_BATT_POWERED: return "external power";
        case NSPIRE_BATT_LOW:     return "low";
        case NSPIRE_BATT_OK:      return "ok";
        default:                  return "unknown";
    }
}

// The device reports fixed-width char arrays that are not guaranteed to be
// NUL-terminated when full.
std::string boundedString(const char* data, std::size_t capacity) {
    const std::size_t len = ::strnlen(data, capacity);
    return std::string(data, len);
}

} // namespace

Status Status::from(int code, const std::string& context) {
    Status s;
    if (code != NSPIRE_ERR_SUCCESS) {
        // libnspire returns its error enum negated (nspire_strerror indexes its
        // table with -error), so the raw value must be decoded before it is
        // normalised, and normalised before it is compared to an enum constant.
        const char* detail = nspire_strerror(code);
        s.message = context + ": " + (detail ? detail : "unknown error");
    }
    s.code = (code < 0) ? -code : code;
    return s;
}

bool isNoDevice(const Status& s) {
    return s.code == NSPIRE_ERR_NODEVICE;
}

Device::~Device() {
    close();
}

Device::Device(Device&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

Status Device::open() {
    if (handle_) return Status::success();

    nspire_handle_t* raw = nullptr;
    const int ret = nspire_init(&raw);
    if (ret != NSPIRE_ERR_SUCCESS) {
        return Status::from(ret, "opening calculator");
    }
    handle_ = raw;
    return Status::success();
}

void Device::close() {
    if (handle_) {
        nspire_free(handle_);
        handle_ = nullptr;
    }
}

Status Device::info(DeviceInfo& out) {
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "reading device info");

    struct nspire_devinfo raw {};
    const int ret = nspire_device_info(handle_, &raw);
    if (ret != NSPIRE_ERR_SUCCESS) return Status::from(ret, "reading device info");

    out.name = boundedString(raw.device_name, sizeof(raw.device_name));
    out.electronic_id = boundedString(raw.electronic_id, sizeof(raw.electronic_id));

    const auto& os = raw.versions[NSPIRE_VER_OS];
    out.os_version = std::to_string(os.major) + "." + std::to_string(os.minor)
                   + "." + std::to_string(os.build);

    out.hardware = describeHardware(raw.hw_type);
    out.storage_free = raw.storage.free;
    out.storage_total = raw.storage.total;
    out.ram_free = raw.ram.free;
    out.ram_total = raw.ram.total;
    out.lcd_width = raw.lcd.width;
    out.lcd_height = raw.lcd.height;
    out.lcd_bpp = raw.lcd.bbp;
    out.battery = describeBattery(raw.batt.status);
    out.charging = raw.batt.is_charging != 0;
    out.file_extension = boundedString(raw.extensions.file, sizeof(raw.extensions.file));
    return Status::success();
}

Status Device::list(const std::string& path, std::vector<DirEntry>& out) {
    out.clear();
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "listing " + path);

    struct nspire_dir_info* dir = nullptr;
    const int ret = nspire_dirlist(handle_, path.c_str(), &dir);
    if (ret != NSPIRE_ERR_SUCCESS) return Status::from(ret, "listing " + path);

    out.reserve(dir->num);
    for (std::uint32_t i = 0; i < dir->num; ++i) {
        const auto& item = dir->items[i];
        DirEntry entry;
        entry.name = boundedString(item.name, sizeof(item.name));
        entry.size = item.size;
        entry.date = item.date;
        entry.is_directory = (item.type == NSPIRE_DIR);
        out.push_back(std::move(entry));
    }

    nspire_dirlist_free(dir);
    return Status::success();
}

Status Device::stat(const std::string& path, DirEntry& out) {
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "stat " + path);

    struct nspire_dir_item item {};
    const int ret = nspire_attr(handle_, path.c_str(), &item);
    if (ret != NSPIRE_ERR_SUCCESS) return Status::from(ret, "stat " + path);

    out.name = boundedString(item.name, sizeof(item.name));
    out.size = item.size;
    out.date = item.date;
    out.is_directory = (item.type == NSPIRE_DIR);
    return Status::success();
}

Status Device::readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "reading " + path);

    // nspire_file_read fills a caller-supplied buffer, so the size has to come
    // from the directory entry first.
    DirEntry entry;
    if (const Status s = stat(path, entry); !s) return s;

    if (entry.is_directory) {
        return Status::from(NSPIRE_ERR_INVALID, "reading " + path + " (it is a directory)");
    }
    if (entry.size > kMaxFileBytes) {
        return Status::from(NSPIRE_ERR_NOMEM,
                            "reading " + path + " (reported size "
                                    + std::to_string(entry.size) + " bytes exceeds the cap)");
    }
    if (entry.size == 0) return Status::success();

    out.resize(entry.size);
    std::size_t read_bytes = 0;
    const int ret = nspire_file_read(handle_, path.c_str(), out.data(), out.size(), &read_bytes);
    if (ret != NSPIRE_ERR_SUCCESS) {
        out.clear();
        return Status::from(ret, "reading " + path);
    }

    // A file rewritten by the calculator between the stat and the read can come
    // back shorter; trust the byte count the transfer reports.
    out.resize(read_bytes);
    return Status::success();
}

Status Device::writeFile(const std::string& path, const std::vector<std::uint8_t>& data) {
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "writing " + path);

    // The C API takes a non-const pointer but does not modify the buffer.
    void* buffer = const_cast<std::uint8_t*>(data.data());
    const int ret = nspire_file_write(handle_, path.c_str(), buffer, data.size());
    return Status::from(ret, "writing " + path);
}

Status Device::deleteFile(const std::string& path) {
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "deleting " + path);
    return Status::from(nspire_file_delete(handle_, path.c_str()), "deleting " + path);
}

Status Device::makeDirectory(const std::string& path) {
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "creating " + path);
    return Status::from(nspire_dir_create(handle_, path.c_str()), "creating " + path);
}

Status Device::removeDirectory(const std::string& path) {
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "removing " + path);
    return Status::from(nspire_dir_delete(handle_, path.c_str()), "removing " + path);
}

Status Device::capture(Screenshot& out) {
    if (!handle_) return Status::from(NSPIRE_ERR_NODEVICE, "capturing screen");

    struct nspire_image* image = nullptr;
    const int ret = nspire_screenshot(handle_, &image);
    if (ret != NSPIRE_ERR_SUCCESS) return Status::from(ret, "capturing screen");

    out.width = image->width;
    out.height = image->height;
    out.bits_per_pixel = image->bbp;

    const std::size_t bytes =
            (static_cast<std::size_t>(image->width) * image->height * image->bbp + 7u) / 8u;
    out.pixels.assign(image->data, image->data + bytes);

    free(image);   // allocated with malloc inside libnspire
    return Status::success();
}

} // namespace chatlink::link
