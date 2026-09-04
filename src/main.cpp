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

// ChatLink CLI — manual driver for the two layers underneath it.
//
//   Link commands  talk to the calculator over USB (probe, ls, pull, push, rm,
//                  mkdir, shot).
//   Input commands drive the PC's own keyboard and mouse (type, key).
//
// The split is deliberate: each layer is exercisable on its own before the
// bridge that joins them exists.

#include "link/nspire_link.h"
#include "link/protocol.h"
#include "model/anthropic.h"
#include "os_input/os_input.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using chatlink::link::Device;
using chatlink::link::DirEntry;
using chatlink::link::Status;

int usage() {
    std::cout <<
        "ChatLink - TI-Nspire USB link and OS input driver\n"
        "\n"
        "Link commands (talk to the calculator through TI's own driver):\n"
        "  probe                     Show device info and the connection state\n"
        "  ls <path>                 List a calculator directory. The root IS the\n"
        "                            documents folder, so use 'ls /'\n"
        "  pull <remote> <local>     Copy a file off the calculator\n"
        "  push <local> <remote>     Copy a file onto the calculator\n"
        "  rm <remote>               Delete a file on the calculator\n"
        "  mkdir <remote>            Create a directory on the calculator\n"
        "  rmdir <remote>            Remove a directory on the calculator\n"
        "  shot <local.pgm>          Capture the calculator screen\n"
        "\n"
        "Bridge:\n"
        "  serve [--once] [--echo]   Answer requests written by chatlink.tns on\n"
        "                            the calculator, using the Claude API.\n"
        "                            --echo replies without calling a model,\n"
        "                            --once exits after a single reply.\n"
        "                            Needs ANTHROPIC_API_KEY unless --echo.\n"
        "\n"
        "Input commands (drive this PC's keyboard and mouse):\n"
        "  type <text>               Type text into the focused window\n"
        "  key <spec>                Send a chord, e.g. key ctrl+shift+t\n"
        "\n"
        "Options:\n"
        "  --dry-run                 Validate input commands without injecting them\n"
        "  --delay <ms>              Per-keystroke delay for 'type' (default 2)\n";
    return 2;
}

std::string humanBytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB"};
    auto value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << " " << units[unit];
    return out.str();
}

// Turns the common failure into an explanation rather than an error code. On
// Windows, "no device" almost always means the driver, not a missing cable.
void reportLinkFailure(const Status& status) {
    std::cerr << "error: " << status.message << "\n";
    if (chatlink::link::isNoDevice(status)) {
        std::cerr <<
            "\nNo calculator found. Things to check, in order:\n"
            "  1. The calculator is plugged in and powered on. It must be awake,\n"
            "     not just charging.\n"
            "  2. TI's driver is bound to it. Device Manager should show\n"
            "     \"TI-Nspire(TM) Handheld Device\". If the device was previously\n"
            "     rebound to WinUSB, restore TI's driver - this build talks to the\n"
            "     calculator through TI's driver, not through libusb.\n"
            "  3. No other program holds the device open. TI's own software claims\n"
            "     it exclusively; close it and retry.\n";
    }
}

int openDevice(Device& device) {
    const Status status = device.open();
    if (!status) {
        reportLinkFailure(status);
        return 1;
    }
    return 0;
}

int cmdProbe() {
    Device device;
    if (const int rc = openDevice(device); rc != 0) return rc;

    chatlink::link::DeviceInfo info;
    if (const Status s = device.info(info); !s) {
        reportLinkFailure(s);
        return 1;
    }

    std::cout << "Connected\n"
              << "  Name          : " << info.name << "\n"
              << "  Hardware      : " << info.hardware << "\n"
              << "  OS version    : " << info.os_version << "\n"
              << "  Electronic ID : " << info.electronic_id << "\n"
              << "  Storage       : " << humanBytes(info.storage_free) << " free of "
              << humanBytes(info.storage_total) << "\n"
              << "  RAM           : " << humanBytes(info.ram_free) << " free of "
              << humanBytes(info.ram_total) << "\n"
              << "  Screen        : " << info.lcd_width << "x" << info.lcd_height
              << " @ " << static_cast<int>(info.lcd_bpp) << " bpp\n"
              << "  Battery       : " << info.battery
              << (info.charging ? " (charging)" : "") << "\n"
              << "  Doc extension : " << info.file_extension << "\n"
              << "\nThe OS version above decides whether Ndless can be installed;\n"
                 "check it against the Ndless release notes before flashing.\n";
    return 0;
}

int cmdList(const std::string& path) {
    Device device;
    if (const int rc = openDevice(device); rc != 0) return rc;

    std::vector<DirEntry> entries;
    if (const Status s = device.list(path, entries); !s) {
        reportLinkFailure(s);
        return 1;
    }

    if (entries.empty()) {
        std::cout << path << " is empty\n";
        return 0;
    }
    for (const auto& entry : entries) {
        std::cout << (entry.is_directory ? "d " : "- ") << std::setw(10) << std::right
                  << (entry.is_directory ? std::string("-") : std::to_string(entry.size))
                  << "  " << entry.name << "\n";
    }
    return 0;
}

int cmdPull(const std::string& remote, const std::string& local) {
    // Large reads through TI's driver intermittently time out part-way, while a
    // fresh session on the same file succeeds. The cause is still open - see the
    // "Known issues" note in the README - so retry with a new connection rather
    // than on the existing, possibly desynced one.
    constexpr int kAttempts = 4;
    std::vector<std::uint8_t> data;
    Status last;

    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        Device device;
        if (const int rc = openDevice(device); rc != 0) return rc;

        last = device.readFile(remote, data);
        if (last) break;
        if (last.code != 1 /* NSPIRE_ERR_TIMEOUT */) break;   // only retry timeouts

        if (attempt < kAttempts) {
            std::cerr << "  read timed out, retrying (" << attempt << "/"
                      << kAttempts - 1 << ")\n";
        }
    }

    if (!last) {
        reportLinkFailure(last);
        return 1;
    }

    std::ofstream out(local, std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot open " << local << " for writing\n";
        return 1;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out) {
        std::cerr << "error: writing " << local << " failed\n";
        return 1;
    }

    std::cout << "Pulled " << data.size() << " bytes to " << local << "\n";
    return 0;
}

int cmdPush(const std::string& local, const std::string& remote) {
    std::ifstream in(local, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open " << local << " for reading\n";
        return 1;
    }
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());

    Device device;
    if (const int rc = openDevice(device); rc != 0) return rc;

    if (const Status s = device.writeFile(remote, data); !s) {
        reportLinkFailure(s);
        return 1;
    }

    std::cout << "Pushed " << data.size() << " bytes to " << remote << "\n";
    return 0;
}

int cmdSimpleRemote(const std::string& verb, const std::string& path) {
    Device device;
    if (const int rc = openDevice(device); rc != 0) return rc;

    Status s;
    const char* past_tense = "Created ";
    if (verb == "rm") {
        s = device.deleteFile(path);
        past_tense = "Deleted ";
    } else if (verb == "rmdir") {
        s = device.removeDirectory(path);
        past_tense = "Removed ";
    } else {
        s = device.makeDirectory(path);
    }

    if (!s) {
        reportLinkFailure(s);
        return 1;
    }
    std::cout << past_tense << path << "\n";
    return 0;
}

// Writes a binary PGM, which is the simplest format that needs no image library
// and that every viewer understands.
int cmdScreenshot(const std::string& local) {
    Device device;
    if (const int rc = openDevice(device); rc != 0) return rc;

    chatlink::link::Screenshot shot;
    if (const Status s = device.capture(shot); !s) {
        reportLinkFailure(s);
        return 1;
    }

    if (shot.bits_per_pixel != 8 && shot.bits_per_pixel != 16) {
        std::cerr << "error: screen is " << static_cast<int>(shot.bits_per_pixel)
                  << " bpp; only 8 and 16 bpp are supported\n";
        return 1;
    }

    std::ofstream out(local, std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot open " << local << " for writing\n";
        return 1;
    }

    if (shot.bits_per_pixel == 8) {
        // Greyscale (older monochrome models): binary PGM.
        out << "P5\n" << shot.width << " " << shot.height << "\n255\n";
        out.write(reinterpret_cast<const char*>(shot.pixels.data()),
                  static_cast<std::streamsize>(shot.pixels.size()));
    } else {
        // The CX screen is RGB565, little-endian. Expand to 8 bits per channel
        // and write a binary PPM. Replicating the high bits into the low ones
        // (r << 3 | r >> 2) makes full-scale values reach 255 rather than 248.
        out << "P6\n" << shot.width << " " << shot.height << "\n255\n";
        std::string rgb;
        rgb.reserve(static_cast<std::size_t>(shot.width) * shot.height * 3);

        for (std::size_t i = 0; i + 1 < shot.pixels.size(); i += 2) {
            const unsigned value = static_cast<unsigned>(shot.pixels[i])
                                 | (static_cast<unsigned>(shot.pixels[i + 1]) << 8);
            const unsigned r = (value >> 11) & 0x1F;
            const unsigned g = (value >> 5) & 0x3F;
            const unsigned b = value & 0x1F;
            rgb.push_back(static_cast<char>((r << 3) | (r >> 2)));
            rgb.push_back(static_cast<char>((g << 2) | (g >> 4)));
            rgb.push_back(static_cast<char>((b << 3) | (b >> 2)));
        }
        out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    }

    std::cout << "Captured " << shot.width << "x" << shot.height << " to " << local << "\n";
    return 0;
}

int cmdType(const std::string& text, bool dry_run, unsigned delay_ms) {
    chatlink::osinput::Driver driver;
    driver.setDryRun(dry_run);

    chatlink::osinput::Pacing pacing;
    pacing.key_delay_ms = delay_ms;
    driver.setPacing(pacing);

    if (!dry_run) {
        std::cout << "Typing in 3 seconds - focus the target window now.\n";
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    const auto result = driver.typeText(text);
    if (!result) {
        std::cerr << "error: " << result.error << "\n";
        return 1;
    }
    std::cout << (dry_run ? "[dry run] " : "") << "sent " << result.events_sent
              << " input events\n";
    return 0;
}

int cmdKey(const std::string& spec, bool dry_run) {
    const auto stroke = chatlink::osinput::parseKeySpec(spec);
    if (!stroke) {
        std::cerr << "error: cannot parse key spec '" << spec << "'\n"
                  << "       expected something like ctrl+shift+t, alt+F4, enter\n";
        return 1;
    }

    chatlink::osinput::Driver driver;
    driver.setDryRun(dry_run);

    if (!dry_run) {
        std::cout << "Sending in 3 seconds - focus the target window now.\n";
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    const auto result = driver.tapKey(*stroke);
    if (!result) {
        std::cerr << "error: " << result.error << "\n";
        return 1;
    }
    std::cout << (dry_run ? "[dry run] " : "") << "sent " << result.events_sent
              << " input events (vk=0x" << std::hex << stroke->vk << std::dec << ")\n";
    return 0;
}

// Turns a prompt from the calculator into a reply. Kept behind one function so
// the backends (the Claude API, or typing into a focused window via os_input)
// can be swapped without touching the polling loop.
std::string answerEcho(const std::string& prompt) {
    std::ostringstream out;
    out << "echo: " << prompt << "\n\n"
        << "(" << prompt.size() << " bytes received over USB)";
    return out.str();
}

std::string answerClaude(const chatlink::model::Config& config, const std::string& prompt) {
    const auto reply = chatlink::model::ask(config, prompt);
    if (reply) return reply.text;

    // The calculator has no other channel, so failures have to come back as the
    // reply itself rather than only to this console.
    std::cerr << "  model error: " << reply.error << "\n";
    return "[ChatLink] The model call failed:\n" + reply.error;
}

// Watches req.tns for a request the calculator has not been answered for yet,
// and writes the reply to rsp.tns.
//
// Every operation reopens the device. That is deliberate: the link is flaky
// across sessions, and a fresh connection is the one thing that reliably
// recovers it. Holding a device open across a long poll would also keep the
// calculator's own program from being the only USB client.
int cmdServe(bool once, bool use_echo) {
    namespace proto = chatlink::protocol;

    chatlink::model::Config model_config;
    model_config.api_key = chatlink::model::apiKeyFromEnvironment();

    if (!use_echo && model_config.api_key.empty()) {
        std::cerr << "error: ANTHROPIC_API_KEY is not set.\n"
                     "       Set it, or run 'serve --echo' to test the link without a model.\n";
        return 1;
    }

    // Unbuffered: serve is long-running and usually watched through a redirected
    // log, where default full buffering hides every line until the process ends.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::cout << "Serving. Run chatlink.tns on the calculator; Ctrl+C to stop.\n"
              << "  request : " << proto::kRequestPath << "\n"
              << "  response: " << proto::kResponsePath << "\n\n";

    std::uint64_t last_handled = 0;
    int idle_reports = 0;
    long poll_count = 0;

    for (;;) {
        std::vector<std::uint8_t> raw;
        bool read_ok = false;
        std::string why;
        {
            Device device;
            if (const Status o = device.open(); !o) {
                why = "open: " + o.message;
            } else if (const Status r = device.readFile(proto::kRequestPath, raw); !r) {
                why = "read: " + r.message;
            } else {
                read_ok = true;
            }
        }

        // Heartbeat. Whether the link answers while the calculator is running its
        // own program is the open design question, so make it visible rather than
        // guessing from silence.
        if (++poll_count % 1 == 0) {
            std::cout << "  [poll " << poll_count << "] "
                      << (read_ok ? "link OK, req.tns " + std::to_string(raw.size()) + " bytes"
                                  : "link unreachable - " + why)
                      << "\n";
        }

        if (read_ok && !raw.empty()) {
            const auto decoded = proto::decode(raw);
            if (decoded.ok() && decoded.message->sequence > last_handled) {
                const auto& msg = *decoded.message;
                std::cout << "[" << msg.sequence << "] prompt: " << msg.payload << "\n";

                proto::Message reply;
                reply.sequence = msg.sequence;
                // The link is closed for the whole model call. That is
                // deliberate: TI's driver allows a single opener, and holding
                // it while waiting on the API would block the calculator.
                if (use_echo) {
                    reply.payload = answerEcho(msg.payload);
                } else {
                    std::cout << "  asking " << model_config.model << "...\n";
                    reply.payload = answerClaude(model_config, msg.payload);
                }

                const auto encoded = proto::encode(reply);
                Device device;
                if (const Status s = device.open(); !s) {
                    std::cerr << "  cannot reopen to reply: " << s.message << "\n";
                } else if (const Status w = device.writeFile(proto::kResponsePath, encoded); !w) {
                    std::cerr << "  reply write failed: " << w.message << "\n";
                } else {
                    std::cout << "  replied (" << reply.payload.size() << " bytes)\n";
                    last_handled = msg.sequence;
                    if (once) return 0;
                }
                idle_reports = 0;
            } else if (!decoded.ok() && !decoded.shouldRetry()
                       && decoded.error != proto::DecodeError::Empty) {
                if (idle_reports++ % 20 == 0) {
                    std::cout << "  req.tns present but " << proto::describe(decoded.error) << "\n";
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(700));
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    bool dry_run = false;
    unsigned delay_ms = 2;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--delay" && i + 1 < argc) {
            delay_ms = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (arg == "-h" || arg == "--help") {
            return usage();
        } else {
            args.push_back(arg);
        }
    }

    if (args.empty()) return usage();
    const std::string& command = args[0];

    if (command == "probe" && args.size() == 1) return cmdProbe();
    if (command == "ls" && args.size() == 2) return cmdList(args[1]);
    if (command == "pull" && args.size() == 3) return cmdPull(args[1], args[2]);
    if (command == "push" && args.size() == 3) return cmdPush(args[1], args[2]);
    if (command == "rm" && args.size() == 2) return cmdSimpleRemote("rm", args[1]);
    if (command == "mkdir" && args.size() == 2) return cmdSimpleRemote("mkdir", args[1]);
    if (command == "rmdir" && args.size() == 2) return cmdSimpleRemote("rmdir", args[1]);
    if (command == "shot" && args.size() == 2) return cmdScreenshot(args[1]);
    if (command == "serve") {
        bool once = false, echo = false;
        bool bad = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--once") once = true;
            else if (args[i] == "--echo") echo = true;
            else bad = true;
        }
        if (!bad) return cmdServe(once, echo);
    }
    if (command == "key" && args.size() == 2) return cmdKey(args[1], dry_run);

    // 'type' joins the rest so quoting is optional for simple phrases.
    if (command == "type" && args.size() >= 2) {
        std::string text = args[1];
        for (std::size_t i = 2; i < args.size(); ++i) text += " " + args[i];
        return cmdType(text, dry_run, delay_ms);
    }

    return usage();
}
