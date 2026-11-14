// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Unit-level initialization and cleanup (SPEC 9.1).
//
// The loader already returns the units in DFS post-order (`use` declarations in source order,
// first visit wins), so `prog.units` *is* the initialization order and its reverse is the cleanup
// order. This TU turns that order into code: one `void()` hook per unit that has work to do, an
// exit hook that calls the fini hooks in reverse order, and the C `main` wrapper that runs the
// init hooks, registers the exit hook with `atexit`, and then calls the user entry (`mu_main`).
#include "codegen_impl.hpp"

namespace muc_detail {

    void Emitter::emitUnitLifecycle() {
        struct Hooks {
            Unit* unit = nullptr;
            Function* init = nullptr;
            Function* fini = nullptr;
        };

        // One `void()` hook per unit, internal linkage: a future split-compilation mode can call
        // the same symbols from a sequence assembled at link time.
        auto emitInit = [&](const std::string& unit, const std::vector<GlobalVar*>& gs) {
            Function* fn = Function::Create(FunctionType::get(LType::getVoidTy(ctx), false),
                                            GlobalValue::InternalLinkage, unit_init_cname(unit),
                                            mod.get());
            resetDebugScope();
            beginBody(fn);
            for (auto* g : gs) {
                auto* gv = mod->getNamedGlobal(g->cname);
                if (!gv) continue;
                if (g->init) {
                    const size_t mark = stmtTemps.size();
                    initInto(gv, g->rty, g->init.get());
                    destroyTempsFrom(mark);
                } else {
                    // `S g;` with a parameterless constructor: the same lowering as a local declaration.
                    FunctionCallee ctor = mod->getOrInsertFunction(
                        ctor_cname(g->rty->unit, g->rty->name, {}),
                        FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                    if (g->rty->needs_cleanup()) zeroMem(gv, g->rty);
                    b.CreateCall(ctor, {gv});
                }
                // An exception escaping an initializer aborts without cleanup: the object graph is
                // incomplete and destroying part of it is more dangerous than dying.
                emitAbortIfPending();
            }
            finishBody(fn, "unit initializer for `" + unit + "`");
            return fn;
        };

        auto emitFini = [&](const std::string& unit, const std::vector<GlobalVar*>& gs) {
            Function* fn = Function::Create(FunctionType::get(LType::getVoidTy(ctx), false),
                                            GlobalValue::InternalLinkage, unit_fini_cname(unit),
                                            mod.get());
            resetDebugScope();
            beginBody(fn);
            for (auto it = gs.rbegin(); it != gs.rend(); ++it) {
                auto* g = *it;
                auto* gv = mod->getNamedGlobal(g->cname);
                if (!gv) continue;
                emitReleaseValue(gv, g->rty);
                emitAbortIfPending();
            }
            finishBody(fn, "unit cleanup for `" + unit + "`");
            return fn;
        };

        std::vector<Hooks> hooks;
        for (auto& u : prog.units) {
            curUnit = u->name;
            std::vector<GlobalVar*> inits;
            std::vector<GlobalVar*> finis;
            for (auto& d : u->decls) {
                auto* g = dynamic_cast<GlobalVar*>(d.get());
                if (!g || g->is_extern || g->is_const) continue;
                if (g->runtime_init) inits.push_back(g);
                // A zero-initialized variable of a cleanup-needing type is registered too: `main`
                // may assign it later.
                if (g->rty && g->rty->needs_cleanup()) finis.push_back(g);
            }
            Hooks h{u.get(), nullptr, nullptr};
            if (!inits.empty()) h.init = emitInit(u->name, inits);
            if (!finis.empty()) h.fini = emitFini(u->name, finis);
            hooks.push_back(h);
        }

        bool anyFini = false;
        for (auto& h : hooks) anyFini = anyFini || h.fini != nullptr;

        Function* finiAll = nullptr;
        if (anyFini) {
            finiAll = Function::Create(FunctionType::get(LType::getVoidTy(ctx), false),
                                       GlobalValue::InternalLinkage, unit_fini_all_cname(),
                                       mod.get());
            resetDebugScope();
            beginBody(finiAll);
            for (auto it = hooks.rbegin(); it != hooks.rend(); ++it) {
                if (it->fini) {
                    b.CreateCall(it->fini, {});
                    // A destructor that threw propagated out of the hook: abort the remaining cleanup.
                    emitAbortIfPending();
                }
            }
            finishBody(finiAll, "unit cleanup sequence");
        }

        // The entry unit always exists when a program is compiled (a library check never reaches codegen).
        FuncDecl* entry = nullptr;
        for (auto& u : prog.units) {
            if (u->name != prog.entry) continue;
            for (auto& d : u->decls) {
                auto* f = dynamic_cast<FuncDecl*>(d.get());
                if (f && f->is_entry) entry = f;
            }
        }
        if (!entry) return;

        Function* mainFn = Function::Create(
            FunctionType::get(i32(), {i32(), ptrTy()}, false), GlobalValue::ExternalLinkage,
            "main", mod.get());
        resetDebugScope();
        beginBody(mainFn);
        Value* argc = mainFn->getArg(0);
        Value* argv = mainFn->getArg(1);
        argc->setName("argc");
        argv->setName("argv");

        for (auto& h : hooks) {
            if (!h.init) continue;
            b.CreateCall(h.init, {});
            // A throwing initializer propagates out of the hook (the hook returns with the flag
            // set): abort here instead of running `main` on a half-initialized program.
            emitAbortIfPending();
        }
        // Registered before the user entry so that `exit()` inside it still cleans up; the normal
        // return path reaches the same hook when the CRT calls exit after `main` returns.
        if (finiAll) {
            FunctionCallee atexit = mod->getOrInsertFunction(
                "atexit", FunctionType::get(i32(), {ptrTy()}, false));
            b.CreateCall(atexit, {finiAll});
        }

        std::vector<LType*> ptys;
        for (auto& t : entry->pty) ptys.push_back(lt(t));
        FunctionCallee muMain =
            mod->getOrInsertFunction(entry->cname, FunctionType::get(i32(), ptys, false));
        Value* r = entry->pty.empty() ? b.CreateCall(muMain, {})
                                      : b.CreateCall(muMain, {argc, argv});
        b.CreateRet(r);
        finishBody(mainFn, "entry wrapper");
    }

}  // namespace muc_detail
