// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Unit-level initialization and cleanup (SPEC 9.1).
//
// Self-registering model (also the model the library targets will use):
//   * every unit that has work gets `muc$unit$init$<unit>` / `muc$unit$fini$<unit>` plus a
//     `muc$unit$inited$<unit>` guard;
//   * an init calls its direct `use` dependencies first (they recurse), then initializes its own
//     variables, then registers its fini with `atexit` — `atexit` is LIFO, so cleanup is the exact
//     reverse of construction without any global sequencing table;
//   * the entry wrapper calls the init of every unit in loader order (post-order, dependencies
//     first); the guards make repeated calls free, which is what lets a future library entry reuse
//     the same hooks.
#include "codegen_impl.hpp"

namespace muc_detail {

    void Emitter::emitUnitLifecycle() {
        struct Hook {
            Unit* u = nullptr;
            Function* init = nullptr;
            Function* fini = nullptr;
            GlobalVariable* guard = nullptr;
            std::vector<GlobalVar*> runtime_inits;
            std::vector<GlobalVar*> cleanups;
        };

        std::vector<Hook> hooks;
        hooks.reserve(prog.units.size());
        for (auto& u : prog.units) {
            Hook h;
            h.u = u.get();
            for (auto& d : u->decls) {
                auto* g = dynamic_cast<GlobalVar*>(d.get());
                if (!g || g->is_extern || g->is_const) continue;
                if (g->runtime_init) h.runtime_inits.push_back(g);
                // A zero-initialized variable of a cleanup-needing type is registered too: `main`
                // may assign it later.
                if (g->rty && g->rty->needs_cleanup()) h.cleanups.push_back(g);
            }
            hooks.push_back(std::move(h));
        }

        // Phase 1: create the symbols, so every init can name its dependencies.
        for (auto& h : hooks) {
            if (h.runtime_inits.empty() && h.cleanups.empty()) continue;
            auto* voidTy = FunctionType::get(LType::getVoidTy(ctx), false);
            if (!h.cleanups.empty()) {
                h.fini = Function::Create(voidTy, GlobalValue::InternalLinkage,
                                          unit_fini_cname(h.u->name), mod.get());
            }
            h.init = Function::Create(voidTy, GlobalValue::InternalLinkage,
                                      unit_init_cname(h.u->name), mod.get());
            h.guard = new GlobalVariable(*mod, i1(), /*isConstant=*/false,
                                         GlobalValue::InternalLinkage, ConstantInt::get(i1(), 0),
                                         unit_inited_cname(h.u->name));
        }

        auto emitInitBody = [&](Hook& h) {
            resetDebugScope();
            beginBody(h.init);
            BasicBlock* run = BasicBlock::Create(ctx, "init.run", h.init);
            BasicBlock* done = BasicBlock::Create(ctx, "init.done", h.init);
            b.CreateCondBr(b.CreateLoad(i1(), h.guard), done, run);
            b.SetInsertPoint(run);
            b.CreateStore(ConstantInt::get(i1(), 1), h.guard);

            // Dependencies first (they recurse); their guard makes repeated calls free.
            for (auto& use : h.u->uses) {
                for (auto& dep : hooks) {
                    if (dep.init && dep.u->name == use->unit_name) {
                        b.CreateCall(dep.init, {});
                        // A throwing dependency aborts here: the program is half-initialized.
                        emitAbortIfPending();
                        break;
                    }
                }
            }

            for (auto* g : h.runtime_inits) {
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

            // Registered after this unit's own construction: `atexit` is LIFO, so this unit is
            // cleaned up before its dependencies (construction order reversed).
            if (h.fini) {
                FunctionCallee atexit = mod->getOrInsertFunction(
                    "atexit", FunctionType::get(i32(), {ptrTy()}, false));
                b.CreateCall(atexit, {h.fini});
            }
            b.CreateBr(done);
            b.SetInsertPoint(done);
            finishBody(h.init, "unit initializer for `" + h.u->name + "`");
        };

        auto emitFiniBody = [&](Hook& h) {
            resetDebugScope();
            beginBody(h.fini);
            for (auto it = h.cleanups.rbegin(); it != h.cleanups.rend(); ++it) {
                auto* g = *it;
                auto* gv = mod->getNamedGlobal(g->cname);
                if (!gv) continue;
                emitReleaseValue(gv, g->rty);
                emitAbortIfPending();
            }
            finishBody(h.fini, "unit cleanup for `" + h.u->name + "`");
        };

        // Phase 2: emit the bodies (all init symbols exist by now).
        for (auto& h : hooks) {
            if (!h.init) continue;
            curUnit = h.u->name;
            emitInitBody(h);
        }
        for (auto& h : hooks) {
            if (!h.fini) continue;
            curUnit = h.u->name;
            emitFiniBody(h);
        }

        // The entry unit always exists when a program is compiled (a library check never reaches
        // codegen). The wrapper runs every init in loader order: dependencies come first and the
        // guards make the calls idempotent.
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
            // A throwing initializer propagates out of the hook (it returns with the flag set):
            // abort here instead of running `main` on a half-initialized program.
            emitAbortIfPending();
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
