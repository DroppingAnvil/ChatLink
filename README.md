# ChatLink

Links a TI-Nspire CX / CX II to a PC over USB, so text typed on the calculator
can reach a model, and so the calculator can drive the PC's own keyboard and
mouse.

## How the calculator link actually works

The Nspire is **not** a mass-storage or HID device. Over USB it speaks a
vendor-specific bulk protocol (TI's "NavNet"), under `VID 0x0451`, `PID 0xe012`
(CX) or `PID 0xe022` (CX II). That protocol offers no keystroke stream. Its
useful primitives are:

* filesystem operations - list, read, write, delete, mkdir
* screen capture

So "capture text from the calculator" has to be built on **files**. A program
running on the calculator writes what you type into a file; the PC polls that
file over USB, and writes replies back into a second file the calculator polls.

That calculator-side program requires **Ndless** (the Nspire jailbreak). Without
it, the calculator can only run sandboxed Lua documents, which cannot write
plain files and require a manual save/reopen for every message. With Ndless, a
native C program reads keys directly and does plain-file I/O, so the round trip
is automatic.

> Ndless supports only specific OS versions. Run `ChatLink probe` to read the
> version off your calculator and check it against the Ndless release notes
> **before** installing.

Note that over NavNet the filesystem root **is** the documents folder - `ls /`
lists what the calculator shows on its home screen. There is no `/documents`
prefix.

## Windows driver setup

**None required.** ChatLink talks to the calculator through TI's own driver,
`tinspusb.sys`, which every Nspire install already has.

That driver is derived from the OSR USB WDM sample: it publishes a device
interface, `{c5b7f228-caff-42d5-a472-6b9eda7982ec}`, whose dispatch routines map
plain `ReadFile` and `WriteFile` onto the bulk IN and OUT endpoints. That is
exactly the surface libnspire's transport needs, so there is:

* no WinUSB rebinding, and no Zadig
* no driver signing, self-signed certificates, or Code Integrity changes
* **no administrator rights** - the interface opens as a normal user
* no disruption to TI's own software, which keeps working

The usual advice for Nspire projects is to rebind the device to WinUSB with
Zadig. That is only necessary because *libusb* cannot use TI's driver, not
because the device is otherwise inaccessible.

`driver/ChatLink_WinUSB.inf` is kept only for the `libusb` backend below. It is
not needed, and installing it requires a signed catalog that this project does
not ship.

### Backends

`NSPIRE_USB_BACKEND` selects the transport:

| Value | Transport | Requirements |
|---|---|---|
| `ti` *(default)* | TI's `tinspusb.sys`, `CreateFile` + overlapped I/O | none |
| `libusb` | upstream's libusb backend | device rebound to WinUSB, admin install |

The vendored libusb only builds when the `libusb` backend is selected.

## Layout

```
src/
  os_input/     Win32 SendInput wrapper - types text, sends chords, moves mouse.
                Knows nothing about calculators or USB.
  link/         C++ wrapper over libnspire: connect, device info, file transfer,
                screen capture. Plus protocol.{h,cpp}, the framing for the
                file-based message channel.
  tests/        Hardware-free tests for the framing and key-spec parsing.
  main.cpp      CLI exercising both layers independently.
third_party/
  libnspire/    NavNet protocol implementation (LGPL-3.0), plus ChatLink's
                TI-driver transport and MinGW fixes.
  libusb/       libusb 1.0.30, Windows/WinUSB backend. Only used by the
                optional 'libusb' transport.
driver/         WinUSB .inf, needed only for the libusb backend.
tools/
  build.sh      Configures and builds using CLion's bundled toolchain.
```

The two layers are deliberately independent. `os_input` is the reusable input
driver that both the model bridge and macropad mode call into; it is testable
with `--dry-run` without any hardware attached.

## Building

CLion bundles CMake, Ninja and MinGW-w64 but does not put them on `PATH`, and
`gcc` needs its own `bin` directory on `PATH` to find `as` and `ld`. The build
script handles both:

```sh
tools/build.sh            # Debug
tools/build.sh Release
```

Output lands in `build/bin/`. The MinGW runtime is statically linked, so
`ChatLink.exe` runs outside the IDE; only `libnspire.dll` sits beside it.

## CLI

```
Link commands:
  probe                     Device info and connection state
  ls <path>                 List a directory, e.g. ls /
  pull <remote> <local>     Copy a file off the calculator
  push <local> <remote>     Copy a file onto the calculator
  rm <remote>               Delete a file
  mkdir <remote>            Create a directory
  shot <local.pgm>          Capture the calculator screen

Input commands (drive this PC):
  type <text>               Type text into the focused window
  key <spec>                Send a chord, e.g. key ctrl+shift+t

Options:
  --dry-run                 Validate input commands without injecting them
  --delay <ms>              Per-keystroke delay for 'type' (default 2)
```

`type` and `key` wait 3 seconds before firing so you can focus the target
window. Both accept `--dry-run`, which validates and reports without injecting.

Text is typed as Unicode codepoints rather than scan codes, so it is
layout-independent and handles non-ASCII. Note that Windows blocks synthetic
input into windows running at a higher integrity level; typing into an elevated
window from a normal process fails, and ChatLink reports that rather than
silently doing nothing.

> Running the link commands from Git Bash needs `MSYS_NO_PATHCONV=1`, or it
> rewrites `/` into a Windows path before ChatLink sees it. cmd and PowerShell
> are unaffected.

## Status

Working, and verified against a real TI-Nspire CX (OS 4.20.532) through TI's own
driver, with no driver change and no admin:

* `probe` - device info, repeatable across runs
* `ls` - real directory listings
* `mkdir`, `push`, `pull`, `rm` - full round trip, byte-identical
* `os_input` - Unicode text, modifier chords, key-spec parsing, mouse
* the message framing - 19 tests in `build/bin/chatlink_tests.exe`, covering
  every truncated prefix of a message, stale trailing bytes left by a longer
  previous message, and rejection of real `.tns` files

Exercised but not working:

* `shot` - transfers correctly but decodes to noise on the CX. It now handles
  16bpp RGB565 and writes a binary PPM; the remaining fault is in libnspire's
  RLE decode. See Known issues.

## Known issues

**Large reads intermittently time out.** Pulling a ~200 KB file fails partway
roughly a third to a half of the time, while a fresh connection on the same file
succeeds; small operations (`probe`, `ls`, small `pull`) are reliable. Draining
the IN endpoint on open (see `usb_ti.c`) helped but did not eliminate it, and a
longer or repeated drain measured no better. The cause is still open - most
likely residual protocol state that TI's driver does not clear because, unlike
libusb, it performs no device reset. `pull` works around it by retrying with a
new connection, which has been reliable in testing. Writes have not shown the
problem.

**`shot` returns a scrambled image on the CX.** The transfer itself is fine -
repeated captures are byte-identical, and the payload is exactly the expected
320x240x2 bytes - but the decoded framebuffer has no row structure at any
stride, while still compressing to ~9%, so it is real data decoded wrongly
rather than a corrupt transfer. Ruled out so far: short reads desyncing
libnspire's screenshot loop (every packet does carry the assumed 253 bytes);
the payload being uncompressed (it is genuinely shorter than the framebuffer);
and the PackBits branches being inverted (swapping them makes it worse, so
upstream's convention is right). The vendored `screenshot.c` has been left
untouched. Not on the critical path, since the message channel uses files.

**`mkdir /ndless` reports "Already exists" while `ls /ndless` reports "Path does
not exist".** Creating it as `/ndless/` - with a trailing slash - works, and the
folder then lists and accepts writes normally. Other names (`/ndlesstest`,
`/chatlink`) behave correctly without the slash, so something treats that exact
path specially. Use the trailing-slash form for it.

## Ndless

Installed and confirmed on the target CX (OS 4.2.0.532) using **Ndless 4.2
r2006**, whose release notes read "For OS 4.2.0 on CX and CX CAS". Both
`ndless_installer_4.2.0_cx.tns` and `ndless_resources.tns` were transferred into
a top-level `ndless` folder with ChatLink's own `push` - no TI software involved
at any point - and verified byte-identical by reading them back.

Confirmed working by running the prebuilt `colors.tns` sample: it launched as
native ARM code rather than the OS refusing the document.

The calculator reports **HW-W**, a newer hardware revision, so Ndless activates
its LCD compatibility mode for programs written before it. ChatLink's own
calculator program should use the newer `lcd_blit` API and avoid relying on that
mode. HW-W is also the most likely reason `shot` decodes to noise: libnspire
predates the revision.

## Not yet built

* the calculator-side Ndless program
* the polling loop that drives the protocol over the link
* the model bridge, with its two selectable backends (direct Anthropic API call,
  and typing into a focused chat window via `os_input`)

## Third-party licences

* **libusb** - LGPL-2.1, `third_party/libusb/COPYING`
* **libnspire** - LGPL-3.0, `third_party/libnspire/COPYING.LESSER`

libnspire is built as a **shared** library so it stays replaceable, per LGPL 4.

Local changes to libnspire:

* `chatlink-mingw.patch` - upstream tests `_WIN32` where it means `_MSC_VER`,
  sending MinGW down an MSVC-only path (`__pragma`, plus a `gettimeofday` shim
  MinGW does not need).
* `src/usb_ti.c` - new transport over TI's driver. `src/usb.h` made
  backend-agnostic and given `extern "C"` guards; `src/cx2.cpp` routed through
  the same `usb_bulk` shim so the CX II path is not tied to libusb.
* `src/init.c` - the handshake waits for one unsolicited "address request" the
  calculator emits when the USB connection is established, then discards it; its
  only purpose is to drain that packet. The libusb backend called
  `libusb_reset_device()`, forcing a re-enumeration and so a fresh request on
  every open. TI's driver performs no reset, so from the second connection
  onward the packet has already been consumed and none is coming. The wait now
  treats a timeout as "already drained" rather than failing.
