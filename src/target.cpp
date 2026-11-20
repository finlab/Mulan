// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Target triples, the host target and target/platform consistency (see target.hpp).
//
// owns:      `TargetInfo` and its API: triple parsing and normalization, host detection, the
//            cross-target checks and the post-link checks (output format, ELF interpreter, ABI)
// not:       any compiler phase; argument parsing and the build driver live in main.cpp
// pass:      driver
// concept:   target, platform

#include "target.hpp"

#include <cstdint>
#include <cstring>

#include "loader.hpp"

// Whether the link driver should be given the POSIX-only libraries. An empty target means "host", so
// the host decides; an explicit triple decides by its OS, which matters when cross-linking.
bool posix_libc_target(const std::string& target) {
    // `mingw` has no "windows" substring, so both spellings are checked.
    if (!target.empty()) {
        return target.find("windows") == std::string::npos &&
               target.find("mingw") == std::string::npos;
    }
#ifdef _WIN32
    return false;
#else
    return true;
#endif
}


// The pointer width a target architecture implies (0 = unknown). The layout model in types.cpp is
// 64-bit, so a 32-bit target is rejected up front instead of failing in the LLVM backend (#93, #94).
int arch_ptr_bits(const std::string& arch) {
    if (arch == "x86_64" || arch == "aarch64") return 64;
    if (arch == "x86" || arch == "arm") return 32;
    return 0;
}

// The CPU architecture of a triple, in the platform spec spelling. LLVM spells the same CPU
// differently per vendor (`amd64`, `arm64`), so the spellings are folded here.
std::string normalize_arch(const std::string& arch) {
    if (arch == "amd64") return "x86_64";
    if (arch == "arm64" || arch == "arm64e" || arch == "aarch64") return "aarch64";
    if (arch == "i386" || arch == "i486" || arch == "i586" || arch == "i686" || arch == "x86") return "x86";
    if (arch == "armv7" || arch == "armv7l" || arch == "armv7a" || arch == "armv8" || arch == "armv8l") return "arm";
    return arch;
}

// The C runtime family of an ABI or environment name (`gnu` / `musl` / `msvc` / `ucrt`); empty
// when the name does not identify a runtime this compiler knows about.
std::string normalize_abi(const std::string& abi) {
    if (abi == "gnu" || abi == "gnuabi64" || abi == "gnueabi" || abi == "gnueabihf") return "gnu";
    if (abi == "musl") return "musl";
    // Android is a distinct C runtime (Bionic), not glibc: an Android target must not be linked with a
    // host glibc driver and reported as a successful build (#95).
    if (abi == "android") return "android";
    if (abi == "msvc") return "msvc";
    if (abi == "ucrt") return "ucrt";
    return "";
}

// The Windows CRT families: msvcrt, the Universal CRT and MinGW provide the same Windows ABI,
// and which one is in use is decided by the link driver rather than by the triple.
bool windows_abi(const std::string& abi) {
    return abi == "msvc" || abi == "ucrt" || abi == "gnu";
}

bool is_windows_target(const std::string& triple) {
    return triple.find("windows") != std::string::npos || triple.find("mingw") != std::string::npos ||
           triple.find("cygwin") != std::string::npos || triple.find("msvc") != std::string::npos ||
           triple.find("ucrt") != std::string::npos;
}

// Split a `-` separated list (an LLVM triple, a platform spec).
std::vector<std::string> split_dash(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dash = s.find('-', start);
        parts.push_back(s.substr(start, dash == std::string::npos ? std::string::npos : dash - start));
        if (dash == std::string::npos) break;
        start = dash + 1;
    }
    return parts;
}

// The parts of an LLVM triple the CLI has to reason about. A triple is
// `<arch>-<vendor>-<os>-<env>` in its common forms, but the vendor and the OS version are
// optional, so the parts are recognized by role instead of by position.
TargetInfo parse_triple(const std::string& triple) {
    TargetInfo info;
    const std::vector<std::string> parts = split_dash(triple);
    if (parts.empty()) return info;
    info.arch = normalize_arch(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        if (!info.os.empty()) continue;
        if (p.find("windows") != std::string::npos || p.find("mingw") != std::string::npos) info.os = "windows";
        else if (p == "linux") info.os = "linux";
        else if (p == "android") {
            info.os = "linux";
            info.abi = "android";  // Bionic: a different runtime from glibc (#95)
        }
        else if (p == "macos" || p.rfind("macosx", 0) == 0 || p == "ios" ||
                 p.rfind("darwin", 0) == 0) {
            // `x86_64-apple-macosx14.0.0` is the spelling LLVM and clang use for a macOS target; the
            // version follows the OS name (#83).
            info.os = "macos";
        }
    }
    // The environment is the last component, but only when it is an environment: in
    // `x86_64-unknown-linux` the last component is the OS.
    const std::string& last = parts.back();
    if (last != info.os && last != "windows" && last != "linux" && last != "macos") {
        info.abi = normalize_abi(last);
    }
    return info;
}


// An explicit `--target` must name an OS this compiler can emit for: an unknown one used to be treated
// as the host, which linked a host binary and called it a success (#98).
void check_explicit_target(const std::string& triple) {
    const TargetInfo t = parse_triple(triple);
    if (t.os.empty()) {
        throw MuError("target `" + triple + "` names an OS this compiler does not know; use one of "
                      "`windows`, `linux`, `macos` (for example `" +
                      suggest_target(host_platform_os(), t.arch.empty() ? host_platform_arch() : t.arch, "") +
                      "`)");
    }
    const int bits = arch_ptr_bits(t.arch);
    if (bits == 0) {
        throw MuError("target `" + triple + "` names an architecture this compiler does not know; use "
                      "`x86_64` or `aarch64`");
    }
    if (bits != 64) {
        throw MuError("target `" + triple + "` uses " + std::to_string(bits) +
                      "-bit pointers, but this compiler's layout model is 64-bit; only 64-bit targets "
                      "(`x86_64`, `aarch64`) are supported");
    }
}

// The target the object file is generated for when no `--target` is given.
TargetInfo host_target() {
    TargetInfo info;
    info.os = host_platform_os();
    info.arch = host_platform_arch();
    info.abi = host_platform_abi();
    return info;
}

// Whether a triple names a target other than the host: another OS, another CPU architecture or
// another C runtime (musl vs glibc) cannot be linked with the host driver.
bool is_cross_target(const std::string& triple) {
    const TargetInfo t = parse_triple(triple);
    const TargetInfo host = host_target();
    if (!t.os.empty() && t.os != host.os) return true;
    if (!t.arch.empty() && t.arch != host.arch) return true;
    if (t.abi.empty() || host.abi.empty() || t.abi == host.abi) return false;
    // The Windows CRT families are interchangeable, so another one of them is still the host.
    if (t.os == "windows" && host.os == "windows" && windows_abi(t.abi) && windows_abi(host.abi)) return false;
    return true;
}

// A triple for the same OS but another architecture or ABI, to suggest as the matching `--target`.
std::string suggest_target(const std::string& os, const std::string& arch, const std::string& abi) {
    if (os == "windows") return arch + "-pc-windows-" + (abi.empty() ? "msvc" : abi);
    if (os == "macos") return arch + "-apple-darwin";
    if (os == "linux") return arch + "-unknown-linux-" + (abi.empty() ? "gnu" : abi);
    return arch + "-unknown-unknown";
}

// The link step must produce the object format the target asks for. A host driver that does not know
// the format still exits 0 and writes a broken file, so verify the magic instead of trusting `rc`.
void check_output_format(const std::filesystem::path& out, const std::string& target) {
    if (target.empty()) return;  // host target: the driver is the host toolchain
    std::ifstream f(out, std::ios::binary);
    // Some PE drivers append `.exe` to an output name without an extension.
    if (!f) f.open(out.string() + ".exe", std::ios::binary);
    if (!f) {
        throw MuError("the link driver produced no output at `" + out.string() + "` for target `" +
                      target + "`");
    }
    unsigned char magic[4] = {};
    f.read(reinterpret_cast<char*>(magic), 4);
    const std::streamsize got = f.gcount();
    const bool elf = got >= 4 && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    const bool pe = got >= 2 && magic[0] == 'M' && magic[1] == 'Z';
    const bool macho = got >= 4 && ((magic[0] == 0xFE && magic[1] == 0xED && magic[2] == 0xFA && (magic[3] == 0xCE or magic[3] == 0xCF)) or
                                    (magic[0] == 0xCF && magic[1] == 0xFA && magic[2] == 0xED && magic[3] == 0xFE) or
                                    (magic[0] == 0xCA && magic[1] == 0xFE && magic[2] == 0xBA && magic[3] == 0xBE));
    // Use the same triple parser the rest of the driver uses: `x86_64-w64-mingw32` is Windows even
    // though the string never says "windows", and `x86_64-apple-macosx14.0.0` is Mach-O (#84, #83).
    const std::string want_os = parse_triple(target).os;
    const bool want_pe = want_os == "windows";
    const bool want_elf = want_os == "linux" || want_os.empty();
    const bool ok = want_pe ? pe : (want_elf ? elf : macho);
    if (!ok) {
        throw MuError("the link driver produced the wrong object format for target `" + target +
                      "` (expected " + (want_pe ? "PE/COFF" : (want_elf ? "ELF" : "Mach-O")) +
                      "); pass `--cc <cross driver>`");
    }
}

// The dynamic loader an ELF executable names in its program headers (`PT_INTERP`): a glibc binary
// names `ld-linux-*.so.*`, a musl one `ld-musl-*.so.1`. The ELF magic alone does not tell the two
// runtimes apart, but the interpreter does.
std::string elf_interpreter(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 64) return "";
    const unsigned char* b = reinterpret_cast<const unsigned char*>(buf.data());
    if (b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') return "";
    if (b[5] != 1) return "";               // EI_DATA: only the little-endian layout is parsed below
    if (b[4] != 1 && b[4] != 2) return "";  // EI_CLASS: 32 or 64 bit
    const bool is64 = b[4] == 2;
    auto u16 = [&](size_t off) -> unsigned {
        return static_cast<unsigned>(b[off]) | (static_cast<unsigned>(b[off + 1]) << 8);
    };
    auto u32 = [&](size_t off) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | b[off + i];
        return v;
    };
    auto u64 = [&](size_t off) -> std::uint64_t {
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | b[off + i];
        return v;
    };
    const std::uint64_t phoff = is64 ? u64(32) : u32(28);
    const unsigned phentsize = is64 ? u16(54) : u16(42);
    const unsigned phnum = is64 ? u16(56) : u16(44);
    for (unsigned i = 0; i < phnum; ++i) {
        const std::uint64_t ph = phoff + static_cast<std::uint64_t>(i) * phentsize;
        if (ph + phentsize > buf.size()) break;
        if (u32(static_cast<size_t>(ph)) != 3) continue;  // PT_INTERP
        const std::uint64_t off = is64 ? u64(static_cast<size_t>(ph) + 8) : u32(static_cast<size_t>(ph) + 4);
        const std::uint64_t size = is64 ? u64(static_cast<size_t>(ph) + 32) : u32(static_cast<size_t>(ph) + 16);
        if (off == 0 || size == 0 || off + size > buf.size()) return "";
        std::string s(reinterpret_cast<const char*>(b + off), static_cast<size_t>(size));
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
    return "";
}

// A binary linked against another C runtime than the target asks for is not the target: the
// object format alone does not tell glibc from musl, so the interpreter name decides. A static
// binary has no interpreter and cannot be checked this way.
void check_linked_abi(const std::filesystem::path& out, const std::string& target) {
    if (target.empty()) return;
    const std::string want = normalize_abi(parse_triple(target).abi);
    if (want != "gnu" && want != "musl" && want != "android") return;
    std::filesystem::path path = out;
    if (!std::filesystem::exists(path)) {
        const std::filesystem::path alt = std::filesystem::path(out.string() + ".exe");
        if (std::filesystem::exists(alt)) path = alt;
    }
    const std::string interp = elf_interpreter(path);
    std::string got;
    if (interp.find("musl") != std::string::npos) got = "musl";
    else if (interp.find("ld-linux") != std::string::npos) got = "gnu";
    // Android binaries name `/system/bin/linker*` (or are static); a glibc interpreter means the
    // driver linked for the host, which is not the requested Android target (#95).
    if (want == "android") {
        if (interp.find("linker") != std::string::npos || interp.empty()) return;
        throw MuError("the link driver produced a `" + got + "` binary, but the target `" + target +
                      "` asks for Android (Bionic); pass an Android `--cc <driver>`");
    }
    if (got.empty() || got == want) return;
    throw MuError("the link driver produced a binary for the " + got + " runtime, but the target `" + target +
                  "` asks for " + want + "; pass a matching `--cc <driver>`");
}

// The C runtime and the OS of a triple have to fit together: musl and glibc are POSIX runtimes,
// the MSVC and the Universal CRT are Windows ones. A triple that mixes them cannot be produced
// by any driver, so it fails here instead of at the link step. The `gnu` environment is the
// exception: on a Windows target it names the MinGW runtime, not glibc.
void check_target_abi_os(const std::string& triple) {
    const TargetInfo t = parse_triple(triple);
    if (t.abi.empty() || t.os.empty()) return;
    if (t.abi == "musl" && t.os == "windows") {
        throw MuError("target `" + triple + "` asks for the musl runtime, which does not exist on Windows; "
                      "use a Windows ABI (`--target " + suggest_target("windows", t.arch, "msvc") + "`) or a "
                      "POSIX target");
    }
    if ((t.abi == "msvc" || t.abi == "ucrt") && t.os != "windows") {
        throw MuError("target `" + triple + "` asks for the " + t.abi + " runtime, which is a Windows runtime; "
                      "use `--target " + suggest_target(t.os, t.arch, "gnu") + "` for a POSIX target");
    }
}

// The platform selects the unit set, the codegen target selects the object file: both have to
// describe the same machine. `check` is the tool for looking at a platform that does not match
// the build target, so a mismatch is reported with that hint.
void check_platform_matches_target(const std::string& platform_spec, const std::string& option,
                                           const PlatformSpec& spec, const TargetInfo& target) {
    const std::string where = "`" + option + " " + platform_spec + "`";
    if (!spec.arch.empty() && !target.arch.empty() && spec.arch != target.arch) {
        throw MuError(where + " selects the " + spec.arch + " units, but the build target is " + target.arch +
                      "; use `muc check " + option + " " + platform_spec + "` to check those units, or build for " +
                      spec.arch + " with `--target " + suggest_target(target.os, spec.arch, "") + "`");
    }
    const std::string abi = normalize_abi(spec.abi);
    const bool windows_family = target.os == "windows" && windows_abi(abi) && windows_abi(target.abi);
    if (!abi.empty() && !target.abi.empty() && abi != target.abi && !windows_family) {
        throw MuError(where + " selects the " + abi + " units, but the build target uses the " + target.abi +
                      " runtime; use `muc check " + option + " " + platform_spec + "` to check those units, or "
                      "build with `--target " + suggest_target(target.os, target.arch, abi) + "`");
    }
}
