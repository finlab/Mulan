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
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <typeinfo>
#include <vector>

#include "codegen_llvm.hpp"
#include "common.hpp"
#include "loader.hpp"
#include "parser.hpp"
#include "sema.hpp"

namespace fs = std::filesystem;

#ifndef MUC_VERSION
#define MUC_VERSION "0.0.0"
#endif

static const char* USAGE = R"(muc - Mulan language compiler (LLVM backend)

Usage:
  muc check <entry.mu>            run semantic checks only
  muc dump  <entry.mu>            dump the AST (debug)
  muc build <entry.mu> [options]  compile and link
  muc -V | --version              print version

build options:
  -o <path>        output executable (default: build/<entry>.exe)
  --backend <b>    backend to use (only llvm in this version; kept for compatibility)
  --emit-ir <path> also write the generated LLVM IR to <path>
  -g, --debug      emit DWARF debug info (line tables / function scopes)
  --bounds=on|off  subscript bounds checks for managed sequences (default: on)
  --cc <name>      linker driver (default: $CC, else gcc)
  --os <os>        resolve `platform/<os>/` units for this platform (windows/linux/macos)
  -I <dir>         extra unit search directory (repeatable)
  -v, --verbose    print the resolved file of every unit
  --export <name>  keep this symbol externally visible (repeatable); by default every definition
                   except `main` is internalized so unused code is removed
  --run            run the executable after a successful build

check options:
  --os <os>        check the program against another platform's units (any host)
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

struct BuildArgs {
    std::string entry;
    std::string out;
    std::string emit_ir;
    std::string backend = "llvm";
    std::string cc = "gcc";
    std::string os;  // platform shadow directory to resolve units against (empty = host)
    std::vector<std::string> dirs;
    std::vector<std::string> exports;  // --export: keep these symbol names externally visible
    bool run = false;
    bool debug = false;
    bool bounds = true;  // emit subscript bounds checks for managed sequences (--bounds=off disables)
    bool verbose = false;
};

static BuildArgs parse_build_args(const std::vector<std::string>& args) {
    BuildArgs b;
    if (const char* cc = std::getenv("CC")) b.cc = cc;
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
        else if (a == "--cc") b.cc = need("--cc");
        else if (a == "--os") b.os = need("--os");
        else if (a == "-I") b.dirs.push_back(need("-I"));
        else if (a == "--export") b.exports.push_back(need("--export"));
        else if (a == "--run") b.run = true;
        else if (a == "-v" || a == "--verbose") b.verbose = true;
        else if (a == "-g" || a == "--debug") b.debug = true;
        else if (a == "--bounds=on") b.bounds = true;
        else if (a == "--bounds=off") b.bounds = false;
        else throw MuError("unknown option `" + a + "`");
        ++i;
    }
    return b;
}

// `check` accepts the same resolution options as `build` but nothing that produces an artifact.
static int cmd_check(const std::vector<std::string>& args) {
    std::string entry;
    std::string os;
    std::vector<std::string> dirs;
    bool verbose = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto need = [&](const char* what) {
            if (i + 1 >= args.size()) throw MuError(std::string("missing argument for ") + what);
            return args[++i];
        };
        if (a == "-I") dirs.push_back(need("-I"));
        else if (a == "--os") os = need("--os");
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (!a.empty() && a[0] == '-') throw MuError("unknown option `" + a + "`");
        else if (entry.empty()) entry = a;
        else throw MuError("unexpected argument `" + a + "`");
    }
    if (entry.empty()) { std::fputs(USAGE, stderr); return 2; }
    std::string entry_name;
    auto units = load_units(entry, dirs, entry_name, os, verbose);
    size_t n = units.size();
    auto prog = analyze(std::move(units), entry_name);
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
    // Selecting another platform's units is only meaningful for checking: codegen and linking still
    // target the host toolchain, so a cross-target `build` would mix a Linux unit set with a
    // Windows object file. Cross-compilation is a separate (later) feature.
    if (!ba.os.empty() && ba.os != host_platform_os()) {
        throw MuError("`--os " + ba.os + "` is not supported by `build` yet (it would mix another "
                      "platform's units with the host object format); use `muc check --os " + ba.os +
                      "` to verify that platform's units");
    }
    std::string entry_name;
    auto units = load_units(ba.entry, ba.dirs, entry_name, ba.os, ba.verbose);
    auto prog = analyze(std::move(units), entry_name);

    fs::path entry_p(ba.entry);
    std::string base = entry_p.stem().string();
    fs::path build_dir = fs::current_path() / "build";
    fs::create_directories(build_dir);
    fs::path out = ba.out.empty() ? build_dir / (base + ".exe") : fs::path(ba.out);

    if (ba.backend != "llvm") {
        throw MuError("unknown backend `" + ba.backend + "` (only llvm in this version)");
    }

    fs::path ir_path = ba.emit_ir.empty() ? build_dir / (base + ".ll") : fs::path(ba.emit_ir);
    if (ir_path.has_parent_path()) fs::create_directories(ir_path.parent_path());
    {
        std::ofstream f(ir_path, std::ios::binary);
        if (!f) throw MuError("cannot write " + ir_path.string());
        f << llvm_emit_text(prog, ba.bounds, ba.exports);
    }
    fs::path obj = build_dir / (base + ".o");
    llvm_emit_object(prog, obj.string(), ba.debug, ba.bounds, ba.exports);
    std::string cmd = ba.cc + " -o " + quote(native_path(out.string())) + " " + quote(native_path(obj.string()));
    std::cout << "+ " << cmd << "\n";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << "error: link failed (object file: " << obj.string()
                  << ", IR: " << ir_path.string() << ")\n";
        return rc;
    }
    std::cout << "ok: " << out.string() << "\n";
    if (ba.run) {
        return std::system(quote(native_path(out.string())).c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    enableUtf8Console();
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && (args[0] == "-V" || args[0] == "--version")) {
        std::cout << "muc " MUC_VERSION "\n";
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
        if (cmd == "dump") return cmd_dump(rest);
        if (cmd == "build") return cmd_build(rest);
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
