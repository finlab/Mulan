// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// muc command-line entry point
#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/wait.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <typeinfo>
#include <vector>

#include "codegen_llvm.hpp"
#include "common.hpp"
#include "directive.hpp"
#include "loader.hpp"
#include "parser.hpp"
#include "sema.hpp"

namespace fs = std::filesystem;

#ifndef MUC_VERSION
#define MUC_VERSION "0.0.0"
#endif
#ifndef MUC_BUILD
#define MUC_BUILD "0"
#endif

static const char* USAGE = R"(muc - Mulan language compiler (LLVM backend)

Usage:
  muc check <entry.mu>            run semantic checks only
  muc dump  <entry.mu>            dump the AST (debug)
  muc build <entry.mu> [options]  compile and link
  muc run   <entry.mu> [options]  compile, link and run
  muc dump-config [options]       print the build constants `#if` conditions read
  muc -V | --version              print version

build options:
  -o <path>        output executable (default: build/<entry>.exe)
  --backend <b>    backend to use (only llvm in this version; kept for compatibility)
  --emit-ir <path> also write the generated LLVM IR to <path>
  -g, --debug      emit DWARF debug info (line tables / function scopes)
  --bounds=on|off  subscript bounds checks for managed sequences (default: on)
  --single-thread  reference counting without atomics: `para`, `para for` and
                   `std.thread` / `std.task` become compile errors (default: off)
  --strict         warn about a generic that is never instantiated (its body is never checked)
  --cc <name>      linker driver (default: $CC, else gcc)
  --link <obj>     extra object file for the link step (repeatable), e.g. a C shim
                   compiled with the same compiler
  --os <os>        resolve `platform/<os>/` units for this platform (windows/linux/macos)
  --platform <spec>  resolve units for `os[-arch[-abi]]` (e.g. windows-x86_64-ucrt); supersedes --os;
                   the architecture and ABI must match the build target
  --target <triple>  LLVM target triple (default: host); a cross target emits the object/IR and needs
                   `--cc <cross driver>` to link
  -I <dir>         extra unit search directory (repeatable)
  -v, --verbose    print the resolved file of every unit
  --export <name>  keep this symbol externally visible (repeatable); by default every definition
                   except `main` is internalized so unused code is removed
  --exports=<mode> export surface: `export` (default) keeps only declarations marked `export`,
                   `public` keeps every `public` declaration
  --run            run the executable after a successful build

check options:
  --os <os>        check the program against another platform's units (any host)
  --platform <spec>  check against `os[-arch[-abi]]` (e.g. windows-x86_64-ucrt)
  --strict         warn about a generic that is never instantiated (its body is never checked)
  -I <dir>         extra unit search directory (repeatable)
  -v, --verbose    print the resolved file of every unit

Environment:
  MUC_BACKEND      default backend (llvm only)
)";

// ---------------------------------------------------------------- console encoding
//
// The Windows console defaults to the local code page (936/GBK for Simplified Chinese), while Chinese
// messages in the source are stored as UTF-8. When written straight to stdout, the console interprets those bytes as
// GBK and Chinese turns into mojibake. So switch the console output code page to UTF-8 at startup and restore the
// original value on exit, to avoid affecting other commands in the same window; when output is redirected to a file the bytes are unchanged and still UTF-8.
#if defined(_WIN32)
static UINT g_saved_console_cp = 0;

static void restoreConsoleOutputCp() {
    if (g_saved_console_cp != 0 && g_saved_console_cp != CP_UTF8)
        SetConsoleOutputCP(g_saved_console_cp);
}

static void enableUtf8Console() {
    g_saved_console_cp = GetConsoleOutputCP();
    if (g_saved_console_cp == 0 || g_saved_console_cp == CP_UTF8) return;
    SetConsoleOutputCP(CP_UTF8);
    std::atexit(restoreConsoleOutputCp);
}
#else
static void enableUtf8Console() {}
#endif

static std::string quote(const std::string& s) { return "\"" + s + "\""; }

// `std::system` returns the process exit code on Windows, but a wait status on POSIX (exit code in
// bits 8-15, or the signal number). Normalize so `--run` forwards a real exit code either way.
static int shell_status(int rc) {
#ifdef _WIN32
    return rc;
#else
    if (rc == -1) return rc;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
    return rc;
#endif
}

// Whether the link driver should be given the POSIX-only libraries. An empty target means "host", so
// the host decides; an explicit triple decides by its OS, which matters when cross-linking.
static bool posix_libc_target(const std::string& target) {
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

// On Windows std::system is parsed by cmd.exe: forward-slash paths are treated as argument separators and cannot execute,
// so convert them to backslashes before passing to the shell (gcc accepts both forms).
static std::string native_path(std::string p) {
#ifdef _WIN32
    for (char& c : p)
        if (c == '/') c = '\\';
#endif
    return p;
}

// ---------------------------------------------------------------- dump

static void dump(const Node* n, int indent);

template <class T>
static const T* as(const Node* n) {
    return dynamic_cast<const T*>(n);
}

static void dump_expr(const Expr* e, int ind);

static void dump_init(const Init* i, int ind) {
    std::string pad(ind * 2, ' ');
    if (!i) return;
    if (auto ie = dynamic_cast<const InitExpr*>(i)) {
        std::cout << pad << "InitExpr\n";
        dump_expr(ie->expr.get(), ind + 1);
    } else if (auto ia = dynamic_cast<const InitAggregate*>(i)) {
        std::cout << pad << "InitAggregate\n";
        for (auto& [nm, e] : ia->items) {
            std::cout << pad << "  " << (nm.empty() ? "positional" : "field " + nm) << "\n";
            dump_init(e.get(), ind + 2);
        }
    }
}

static void dump_expr(const Expr* e, int ind) {
    if (!e) return;
    std::string pad(ind * 2, ' ');
    std::string name = typeid(*e).name();
    // gcc's typeid names carry a prefix; keep only the readable part
    auto pos = name.find_last_of("0123456789");
    if (pos != std::string::npos && pos + 1 < name.size()) name = name.substr(pos + 1);

    std::cout << pad << name;
    if (e->ty) std::cout << " :" << e->ty->str();
    if (auto v = dynamic_cast<const IntLit*>(e)) std::cout << " " << v->value;
    if (auto v = dynamic_cast<const CharLit*>(e)) std::cout << " " << v->value;
    if (auto v = dynamic_cast<const FloatLit*>(e)) std::cout << " " << v->value;
    if (auto v = dynamic_cast<const StringLit*>(e)) std::cout << " \"" << v->value << "\"";
    if (auto v = dynamic_cast<const BoolLit*>(e)) std::cout << " " << (v->value ? "true" : "false");
    if (auto v = dynamic_cast<const Name*>(e)) std::cout << " " << v->ident;
    if (auto v = dynamic_cast<const Member*>(e)) std::cout << " ." << v->name;
    if (auto v = dynamic_cast<const Unary*>(e)) std::cout << " " << v->op;
    if (auto v = dynamic_cast<const Binary*>(e)) std::cout << " " << v->op;
    if (dynamic_cast<const Ternary*>(e)) std::cout << " ?:";
    std::cout << "\n";

    if (dynamic_cast<const Name*>(e)) return;
    if (dynamic_cast<const IntLit*>(e)) return;
    if (dynamic_cast<const CharLit*>(e)) return;
    if (dynamic_cast<const FloatLit*>(e)) return;
    if (dynamic_cast<const StringLit*>(e)) return;
    if (dynamic_cast<const BoolLit*>(e)) return;
    if (dynamic_cast<const NullLit*>(e)) return;

    if (auto v = dynamic_cast<const Member*>(e)) { dump_expr(v->obj.get(), ind + 1); return; }
    if (auto v = dynamic_cast<const Index*>(e)) {
        dump_expr(v->base.get(), ind + 1);
        for (auto& ix : v->indices) dump_expr(ix.get(), ind + 1);
        return;
    }
    if (auto v = dynamic_cast<const Unary*>(e)) { dump_expr(v->operand.get(), ind + 1); return; }
    if (auto v = dynamic_cast<const NotExpr*>(e)) { dump_expr(v->operand.get(), ind + 1); return; }
    if (auto v = dynamic_cast<const Binary*>(e)) {
        dump_expr(v->left.get(), ind + 1);
        dump_expr(v->right.get(), ind + 1);
        return;
    }
    if (auto v = dynamic_cast<const Cast*>(e)) { dump_expr(v->expr.get(), ind + 1); return; }
    if (auto v = dynamic_cast<const Ternary*>(e)) {
        dump_expr(v->cond.get(), ind + 1);
        dump_expr(v->then_e.get(), ind + 1);
        dump_expr(v->else_e.get(), ind + 1);
        return;
    }
    if (auto v = dynamic_cast<const Call*>(e)) {
        dump_expr(v->callee.get(), ind + 1);
        for (auto& a : v->args) dump_expr(a.get(), ind + 1);
        for (auto& [nm, a] : v->named) dump_expr(a.get(), ind + 1);
        return;
    }
    if (auto v = dynamic_cast<const Len*>(e)) { dump_expr(v->seq.get(), ind + 1); return; }
    if (auto v = dynamic_cast<const RangeExpr*>(e)) {
        dump_expr(v->lo.get(), ind + 1);
        dump_expr(v->hi.get(), ind + 1);
        return;
    }
}

static void dump(const Node* n, int ind) {
    std::string pad(ind * 2, ' ');
    if (!n) return;
    if (auto e = dynamic_cast<const Expr*>(n)) {
        dump_expr(e, ind);
        return;
    }
    if (auto u = dynamic_cast<const Unit*>(n)) {
        std::cout << pad << "unit " << u->name << "\n";
        for (auto& d : u->decls) dump(d.get(), ind + 1);
        return;
    }
    if (auto f = dynamic_cast<const FuncDecl*>(n)) {
        std::cout << pad << "FuncDecl " << f->name;
        if (f->rty) std::cout << " :" << f->rty->str();
        std::cout << "\n";
        for (auto& p : f->params) {
            std::cout << pad << "  Param " << p->name << "\n";
        }
        if (f->body) dump(f->body.get(), ind + 1);
        return;
    }
    if (auto s = dynamic_cast<const StructDecl*>(n)) {
        std::cout << pad << "StructDecl " << s->name << "\n";
        for (auto& [nm, ts] : s->fields) std::cout << pad << "  field " << nm << "\n";
        return;
    }
    if (auto g = dynamic_cast<const GlobalVar*>(n)) {
        std::cout << pad << "GlobalVar " << g->name;
        if (g->rty) std::cout << " :" << g->rty->str();
        std::cout << "\n";
        dump_init(g->init.get(), ind + 1);
        return;
    }
    if (auto b = dynamic_cast<const Block*>(n)) {
        std::cout << pad << "Block\n";
        for (auto& s : b->stmts) dump(s.get(), ind + 1);
        return;
    }
    if (auto lv = dynamic_cast<const LocalVar*>(n)) {
        std::cout << pad << "LocalVar " << lv->name;
        if (lv->rty) std::cout << " :" << lv->rty->str();
        std::cout << "\n";
        dump_init(lv->init.get(), ind + 1);
        return;
    }
    if (auto es = dynamic_cast<const ExprStmt*>(n)) {
        std::cout << pad << "ExprStmt\n";
        dump_expr(es->expr.get(), ind + 1);
        return;
    }
    if (auto as = dynamic_cast<const AssignStmt*>(n)) {
        std::cout << pad << "Assign " << as->op << "\n";
        dump_expr(as->target.get(), ind + 1);
        dump_expr(as->value.get(), ind + 1);
        return;
    }
    if (auto is = dynamic_cast<const IfStmt*>(n)) {
        std::cout << pad << "If\n";
        dump_expr(is->cond.get(), ind + 1);
        if (is->then) dump(is->then.get(), ind + 1);
        if (is->els) {
            std::cout << pad << "else\n";
            dump(is->els.get(), ind + 1);
        }
        return;
    }
    if (auto ws = dynamic_cast<const WhileStmt*>(n)) {
        std::cout << pad << "While\n";
        dump_expr(ws->cond.get(), ind + 1);
        if (ws->body) dump(ws->body.get(), ind + 1);
        return;
    }
    if (auto fs = dynamic_cast<const ForInStmt*>(n)) {
        std::cout << pad << "ForIn " << (fs->by_ref ? "ref " : "") << fs->var
                  << " (" << fs->loop_kind << ")\n";
        dump_expr(fs->seq.get(), ind + 1);
        if (fs->body) dump(fs->body.get(), ind + 1);
        return;
    }
    if (dynamic_cast<const BreakStmt*>(n)) { std::cout << pad << "Break\n"; return; }
    if (dynamic_cast<const ContinueStmt*>(n)) { std::cout << pad << "Continue\n"; return; }
    if (auto rs = dynamic_cast<const ReturnStmt*>(n)) {
        std::cout << pad << "Return\n";
        dump_expr(rs->value.get(), ind + 1);
        return;
    }
    std::cout << pad << "(node)\n";
}

// ---------------------------------------------------------------- commands

// ---------------------------------------------------------------- target and platform
//
// A build produces one object file, for the target named by `--target` (the host by default).
// The platform (`--platform os[-arch[-abi]]`) selects the unit set that goes into it. Both have
// to describe the same machine: units written for another architecture or another C runtime
// describe a program that cannot be the object file, so `build` refuses them instead of
// producing a binary that only looks right.
struct TargetInfo {
    std::string os;    // windows / linux / macos ("" = unknown)
    std::string arch;  // x86_64 / aarch64 ("" = unknown)
    std::string abi;   // gnu / musl / msvc / ucrt ("" = unknown)
};

// The CPU architecture of a triple, in the platform spec spelling. LLVM spells the same CPU
// differently per vendor (`amd64`, `arm64`), so the spellings are folded here.
static std::string normalize_arch(const std::string& arch) {
    if (arch == "amd64") return "x86_64";
    if (arch == "arm64" || arch == "arm64e" || arch == "aarch64") return "aarch64";
    if (arch == "i386" || arch == "i486" || arch == "i586" || arch == "i686" || arch == "x86") return "x86";
    if (arch == "armv7" || arch == "armv7l" || arch == "armv7a" || arch == "armv8" || arch == "armv8l") return "arm";
    return arch;
}

// The C runtime family of an ABI or environment name (`gnu` / `musl` / `msvc` / `ucrt`); empty
// when the name does not identify a runtime this compiler knows about.
static std::string normalize_abi(const std::string& abi) {
    if (abi == "gnu" || abi == "gnuabi64" || abi == "gnueabi" || abi == "gnueabihf") return "gnu";
    if (abi == "musl") return "musl";
    if (abi == "msvc") return "msvc";
    if (abi == "ucrt") return "ucrt";
    return "";
}

// The Windows CRT families: msvcrt, the Universal CRT and MinGW provide the same Windows ABI,
// and which one is in use is decided by the link driver rather than by the triple.
static bool windows_abi(const std::string& abi) {
    return abi == "msvc" || abi == "ucrt" || abi == "gnu";
}

// Split a `-` separated list (an LLVM triple, a platform spec).
static std::vector<std::string> split_dash(const std::string& s) {
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
static TargetInfo parse_triple(const std::string& triple) {
    TargetInfo info;
    const std::vector<std::string> parts = split_dash(triple);
    if (parts.empty()) return info;
    info.arch = normalize_arch(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        if (!info.os.empty()) continue;
        if (p.find("windows") != std::string::npos || p.find("mingw") != std::string::npos) info.os = "windows";
        else if (p == "linux" || p == "android") info.os = "linux";
        else if (p == "macos" || p == "ios" || p.rfind("darwin", 0) == 0) info.os = "macos";
    }
    // The environment is the last component, but only when it is an environment: in
    // `x86_64-unknown-linux` the last component is the OS.
    const std::string& last = parts.back();
    if (last != info.os && last != "windows" && last != "linux" && last != "macos") {
        info.abi = normalize_abi(last);
    }
    return info;
}

// The target the object file is generated for when no `--target` is given.
static TargetInfo host_target() {
    TargetInfo info;
    info.os = host_platform_os();
    info.arch = host_platform_arch();
    info.abi = host_platform_abi();
    return info;
}

// Whether a triple names a target other than the host: another OS, another CPU architecture or
// another C runtime (musl vs glibc) cannot be linked with the host driver.
static bool is_cross_target(const std::string& triple) {
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
static std::string suggest_target(const std::string& os, const std::string& arch, const std::string& abi) {
    if (os == "windows") return arch + "-pc-windows-" + (abi.empty() ? "msvc" : abi);
    if (os == "macos") return arch + "-apple-darwin";
    if (os == "linux") return arch + "-unknown-linux-" + (abi.empty() ? "gnu" : abi);
    return arch + "-unknown-unknown";
}

// The link step must produce the object format the target asks for. A host driver that does not know
// the format still exits 0 and writes a broken file, so verify the magic instead of trusting `rc`.
static void check_output_format(const fs::path& out, const std::string& target) {
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
    const bool want_pe = target.find("windows") != std::string::npos;
    const bool want_elf = !want_pe && target.find("darwin") == std::string::npos;
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
static std::string elf_interpreter(const fs::path& path) {
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
static void check_linked_abi(const fs::path& out, const std::string& target) {
    if (target.empty()) return;
    const std::string want = normalize_abi(parse_triple(target).abi);
    if (want != "gnu" && want != "musl") return;
    fs::path path = out;
    if (!fs::exists(path)) {
        const fs::path alt = fs::path(out.string() + ".exe");
        if (fs::exists(alt)) path = alt;
    }
    const std::string interp = elf_interpreter(path);
    std::string got;
    if (interp.find("musl") != std::string::npos) got = "musl";
    else if (interp.find("ld-linux") != std::string::npos) got = "gnu";
    if (got.empty() || got == want) return;
    throw MuError("the link driver produced a binary for the " + got + " runtime, but the target `" + target +
                  "` asks for " + want + "; pass a matching `--cc <driver>`");
}

// The C runtime and the OS of a triple have to fit together: musl and glibc are POSIX runtimes,
// the MSVC and the Universal CRT are Windows ones. A triple that mixes them cannot be produced
// by any driver, so it fails here instead of at the link step. The `gnu` environment is the
// exception: on a Windows target it names the MinGW runtime, not glibc.
static void check_target_abi_os(const std::string& triple) {
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
static void check_platform_matches_target(const std::string& platform_spec, const std::string& option,
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

struct BuildArgs {
    std::string entry;
    std::string out;
    std::string emit_ir;
    std::string backend = "llvm";
    std::string cc = "gcc";
    bool cc_explicit = false;  // set by $CC or --cc
    std::string target;        // LLVM triple (`--target`); empty = host
    std::string os;        // platform shadow directory to resolve units against (empty = host)
    std::string platform;  // `os[-arch[-abi]]`; supersedes --os
    std::vector<std::string> dirs;
    std::vector<std::string> exports;  // --export: keep these symbol names externally visible
    std::vector<std::string> link;     // --link: extra object files handed to the link step
    ExportMode export_mode = ExportMode::ExportOnly;  // --exports=public|export
    bool run = false;
    bool debug = false;
    bool bounds = true;  // emit subscript bounds checks for managed sequences (--bounds=off disables)
    bool single_thread = false;  // --single-thread: non-atomic reference counting
    bool strict = false;  // --strict: warn about generics that are never instantiated
    bool verbose = false;
};

static BuildArgs parse_build_args(const std::vector<std::string>& args) {
    BuildArgs b;
    if (const char* cc = std::getenv("CC")) { b.cc = cc; b.cc_explicit = true; }
    if (const char* bk = std::getenv("MUC_BACKEND")) b.backend = bk;
    size_t i = 0;
    if (args.empty()) throw MuError("missing entry file");
    b.entry = args[0];
    i = 1;
    while (i < args.size()) {
        const std::string& a = args[i];
        auto need = [&](const char* what) {
            if (i + 1 >= args.size()) throw MuError(std::string("missing argument for ") + what);
            return args[++i];
        };
        if (a == "-o") b.out = need("-o");
        else if (a == "--emit-ir") b.emit_ir = need("--emit-ir");
        else if (a == "--backend") b.backend = need("--backend");
        else if (a == "--cc") { b.cc = need("--cc"); b.cc_explicit = true; }
        else if (a == "--target") b.target = need("--target");
        else if (a == "--link") b.link.push_back(need("--link"));
        else if (a == "--os") b.os = need("--os");
        else if (a == "--platform") b.platform = need("--platform");
        else if (a == "-I") b.dirs.push_back(need("-I"));
        else if (a == "--export") b.exports.push_back(need("--export"));
        else if (a == "--exports=public") b.export_mode = ExportMode::PublicOnly;
        else if (a == "--exports=export") b.export_mode = ExportMode::ExportOnly;
        else if (a == "--run") b.run = true;
        else if (a == "-v" || a == "--verbose") b.verbose = true;
        else if (a == "-g" || a == "--debug") b.debug = true;
        else if (a == "--bounds=on") b.bounds = true;
        else if (a == "--bounds=off") b.bounds = false;
        else if (a == "--single-thread") b.single_thread = true;
        else if (a == "--strict") b.strict = true;
        else throw MuError("unknown option `" + a + "`");
        ++i;
    }
    return b;
}

// `#if` conditions read exactly these values, so they are fixed before any unit is parsed. The
// platform spec decides the unit set; `--target` decides the object file, and its architecture
// gives `ptr_bits` when the platform spec leaves it out.
static void init_build_config(const std::string& platform_spec, const TargetInfo& target, bool debug,
                              bool bounds, bool single_thread) {
    BuildConfig cfg;
    PlatformSpec spec;
    if (!platform_spec.empty()) {
        spec = parse_platform_spec(platform_spec);
    } else {
        spec.os = target.os;
        spec.arch = target.arch;
        spec.abi = target.abi;
    }
    cfg.os = spec.os;
    cfg.arch = spec.arch.empty() ? target.arch : spec.arch;
    cfg.abi = spec.abi.empty() ? target.abi : spec.abi;
    if (cfg.arch == "aarch64" || cfg.arch == "x86_64") cfg.ptr_bits = 64;
    else if (cfg.arch == "x86") cfg.ptr_bits = 32;
    else cfg.ptr_bits = static_cast<int>(sizeof(void*)) * 8;
    cfg.big_endian = false;  // every target this compiler emits for is little-endian
    cfg.debug = debug;
    cfg.bounds = bounds;
    cfg.single_thread = single_thread;
    set_build_config(cfg);
}

static int cmd_dump_config(const std::vector<std::string>& args) {
    std::string os;
    std::string platform;
    std::string target_triple;
    bool debug = false;
    bool bounds = true;
    bool single_thread = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](const char* what) {
            if (i + 1 >= args.size()) throw MuError(std::string("missing argument for ") + what);
            return args[++i];
        };
        if (a == "--os") os = need("--os");
        else if (a == "--platform") platform = need("--platform");
        else if (a == "--target") target_triple = need("--target");
        else if (a == "-g" || a == "--debug") debug = true;
        else if (a == "--bounds=on") bounds = true;
        else if (a == "--bounds=off") bounds = false;
        else if (a == "--single-thread") single_thread = true;
        else throw MuError("unknown option `" + a + "`");
    }
    if (!os.empty() && !platform.empty()) {
        throw MuError("`--os` and `--platform` select the same thing; use one of them");
    }
    const TargetInfo work = target_triple.empty() ? host_target() : parse_triple(target_triple);
    init_build_config(platform.empty() ? os : platform, work, debug, bounds, single_thread);
    std::cout << describe_build_config();
    return 0;
}

// `check` accepts the same resolution options as `build` but nothing that produces an artifact.
static int cmd_check(const std::vector<std::string>& args) {
    std::string entry;
    std::string os;
    std::string platform;
    std::vector<std::string> dirs;
    bool verbose = false;
    bool single_thread = false;
    bool strict = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](const char* what) {
            if (i + 1 >= args.size()) throw MuError(std::string("missing argument for ") + what);
            return args[++i];
        };
        if (a == "-I") dirs.push_back(need("-I"));
        else if (a == "--os") os = need("--os");
        else if (a == "--platform") platform = need("--platform");
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--single-thread") single_thread = true;
        else if (a == "--strict") strict = true;
        else if (!a.empty() && a[0] == '-') throw MuError("unknown option `" + a + "`");
        else if (entry.empty()) entry = a;
        else throw MuError("unexpected argument `" + a + "`");
    }
    if (entry.empty()) { std::fputs(USAGE, stderr); return 2; }
    if (!os.empty() && !platform.empty()) {
        throw MuError("`--os` and `--platform` select the same thing; use one of them");
    }
    std::string entry_name;
    init_build_config(platform.empty() ? os : platform, host_target(), false, true, single_thread);
    auto units = load_units(entry, dirs, entry_name, platform.empty() ? os : platform, verbose);
    size_t n = units.size();
    auto prog = analyze(std::move(units), entry_name, single_thread, strict);
    std::cout << "ok: " << n << " units, semantic checks passed\n";
    return 0;
}

static int cmd_dump(const std::vector<std::string>& args) {
    if (args.empty()) { std::fputs(USAGE, stderr); return 2; }
    std::string entry_name;
    auto units = load_units(args[0], {}, entry_name);
    for (auto& u : units) dump(u.get(), 0);
    return 0;
}

static int cmd_build(const std::vector<std::string>& args) {
    BuildArgs ba = parse_build_args(args);
    if (!ba.os.empty() && !ba.platform.empty()) {
        throw MuError("`--os` and `--platform` select the same thing; use one of them");
    }
    const std::string target_spec = ba.platform.empty() ? ba.os : ba.platform;
    const std::string option = ba.platform.empty() ? "--os" : "--platform";
    // The platform selects the unit set while the object file is generated for the codegen target
    // (`--target`, else the host): a platform for another machine describes a program that is not
    // the object file, so it is rejected here. Cross-compilation is a separate (later) feature.
    if (!ba.target.empty()) check_target_abi_os(ba.target);
    const TargetInfo target = ba.target.empty() ? host_target() : parse_triple(ba.target);
    if (!target_spec.empty()) {
        const PlatformSpec spec = parse_platform_spec(target_spec);
        if (spec.os != target.os) {
            if (ba.target.empty()) {
                throw MuError("`" + option + " " + spec.os + "` is not supported by `build` yet (it would mix "
                              "another platform's units with the host object format); use `muc check " + option +
                              " " + spec.os + "` to verify that platform's units");
            }
            throw MuError("`" + option + " " + target_spec + "` selects the " + spec.os +
                          " units, but `--target " + ba.target + "` builds for " + target.os +
                          "; the unit set would not match the object file");
        }
        check_platform_matches_target(target_spec, option, spec, target);
    }
    std::string entry_name;
    init_build_config(target_spec, target, ba.debug, ba.bounds, ba.single_thread);
    auto units = load_units(ba.entry, ba.dirs, entry_name, target_spec, ba.verbose);
    auto prog = analyze(std::move(units), entry_name, ba.single_thread, ba.strict);

    fs::path entry_p(ba.entry);
    std::string base = entry_p.stem().string();
    fs::path build_dir = fs::current_path() / "build";
    fs::create_directories(build_dir);
    const bool target_windows =
        ba.target.empty() ? (host_platform_os() == "windows") : (ba.target.find("windows") != std::string::npos);
    fs::path out = ba.out.empty() ? build_dir / (base + (target_windows ? ".exe" : "")) : fs::path(ba.out);
    if (!ba.cc_explicit && !ba.target.empty()) {
        if (ba.target.find("darwin") != std::string::npos) ba.cc = "clang";
        else if (!target_windows) ba.cc = "cc";
    }

    if (ba.backend != "llvm") {
        throw MuError("unknown backend `" + ba.backend + "` (only llvm in this version)");
    }

    fs::path ir_path = ba.emit_ir.empty() ? build_dir / (base + ".ll") : fs::path(ba.emit_ir);
    if (ir_path.has_parent_path()) fs::create_directories(ir_path.parent_path());
    EmitOptions eo;
    eo.debug = ba.debug;
    eo.bounds = ba.bounds;
    eo.export_mode = ba.export_mode;
    eo.extra_exports = ba.exports;
    eo.target = ba.target;
    eo.single_thread = ba.single_thread;
    {
        std::ofstream f(ir_path, std::ios::binary);
        if (!f) throw MuError("cannot write " + ir_path.string());
        f << llvm_emit_text(prog, eo);
    }
    fs::path obj = build_dir / (base + ".o");
    llvm_emit_object(prog, obj.string(), eo);
    // The object and IR are on disk; only the link needs a cross driver.
    if (!ba.cc_explicit && !ba.target.empty() && is_cross_target(ba.target)) {
        throw MuError("target `" + ba.target + "` is not the host; pass `--cc <cross driver>` to link it "
                      "(the object file and IR are written: " + obj.string() + ")");
    }
    std::string cmd = ba.cc + " -o " + quote(native_path(out.string())) + " " + quote(native_path(obj.string()));
    for (const auto& extra : ba.link) cmd += " " + quote(native_path(extra));
    // POSIX keeps the math and thread entry points in separate libraries (`-lm`, `-lpthread`), while
    // the Windows CRTs have both in the default set: without these the link fails on `pow`,
    // `fegetround`, `pthread_create`, ...
    if (posix_libc_target(ba.target)) cmd += " -lm -lpthread";
    std::cout << "+ " << cmd << "\n";
    std::cout.flush();
    int rc = shell_status(std::system(cmd.c_str()));
    if (rc != 0) {
        std::cerr << "error: link failed (object file: " << obj.string()
                  << ", IR: " << ir_path.string() << ")\n";
        if (!ba.target.empty() && !ba.cc_explicit) {
            std::cerr << "hint: cross target `" << ba.target << "` needs a cross driver: --cc <compiler>\n";
        }
        return rc;
    }
    check_output_format(out, ba.target);
    check_linked_abi(out, ba.target);
    std::cout << "ok: " << out.string() << "\n";
    if (ba.run) {
        std::cout.flush();
        return shell_status(std::system(quote(native_path(out.string())).c_str()));
    }
    return 0;
}

int main(int argc, char** argv) {
    enableUtf8Console();
    // Default to the host target so every command (including `dump`) parses with a real configuration;
    // `check` / `build` overwrite it once their options are known.
    init_build_config("", host_target(), false, true, false);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && (args[0] == "-V" || args[0] == "--version")) {
        std::cout << "muc " MUC_VERSION " (build " MUC_BUILD ")\n";
        return 0;
    }
    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        std::cout << USAGE;
        return 0;
    }
    std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    try {
        if (cmd == "check") return cmd_check(rest);
        if (cmd == "dump-config" || cmd == "--dump-config") return cmd_dump_config(rest);
        if (cmd == "dump") return cmd_dump(rest);
        if (cmd == "build") return cmd_build(rest);
        if (cmd == "run") {
            // `muc run a.mu` is `muc build a.mu --run`: same pipeline, run at the end.
            if (rest.empty()) { std::fputs(USAGE, stderr); return 2; }
            rest.push_back("--run");
            return cmd_build(rest);
        }
        std::cerr << "unknown command `" << cmd << "`\n\n" << USAGE;
        return 2;
    } catch (const MuErrorList& e) {
        for (const auto& m : e.messages) std::cerr << m << "\n";
        return 1;
    } catch (const MuError& e) {
        std::cerr << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
