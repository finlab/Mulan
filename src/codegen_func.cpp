// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains function code generation and the module wrap-up (run) (Emitter member function definitions).

#include "codegen_impl.hpp"

namespace muc_detail {

    // ------------------------------------------------------------ functions
    // The IRBuilder is reused across functions: before entering a new function, clear the stale !dbg location left over from the previous function,
    // otherwise the new function's instructions would point at the previous function's DISubprogram (which the LLVM verifier rejects).
    void Emitter::resetDebugScope() {
        diSub = nullptr;
        b.SetCurrentDebugLocation(DebugLoc());
        ab.SetCurrentDebugLocation(DebugLoc());
    }

    void Emitter::emitFunction(FuncDecl* f) {
        if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm] emit " << f->name << "\n";
        std::vector<LType*> ptys;
        for (size_t i = 0; i < f->pty.size(); ++i)
            ptys.push_back(i < f->pty_ref.size() && f->pty_ref[i] ? ptrTy() : lt(f->pty[i]));
        auto* ft = FunctionType::get(lt(f->rty), ptys, false);
        Function* fn = cast<Function>(mod->getOrInsertFunction(f->cname, ft).getCallee());
        fn->setLinkage((f->is_extern || f->is_public || f->is_entry)
                           ? GlobalValue::ExternalLinkage
                           : GlobalValue::InternalLinkage);
        if (f->is_extern) return;

        resetDebugScope();
        if (dib) {
            const unsigned line = f->line > 0 ? static_cast<unsigned>(f->line) : 1u;
            auto* spTy = dib->createSubroutineType(dib->getOrCreateTypeArray({}));
            diSub = dib->createFunction(diCu, f->name, f->cname, diFile, line, spTy, line,
                                        DINode::FlagPrototyped, DISubprogram::SPFlagDefinition);
            fn->setSubprogram(diSub);
            const auto fnLoc = DILocation::get(ctx, line, 1, diSub);
            b.SetCurrentDebugLocation(fnLoc);
            ab.SetCurrentDebugLocation(fnLoc);
        }

        curClassTy = nullptr;
        curThis = nullptr;
        thisSlot = nullptr;
        refScopes.clear();
        loopScopeDepth.clear();
        stmtTemps.clear();
        curFn = fn;
        curRetTy = f->rty;
        curIsEntry = f->is_entry;
        inNothrow = false;
        ehStack.clear();
        finallyStack.clear();
        abortBases.clear();
        ehLocals.clear();
        allocaBB = BasicBlock::Create(ctx, "allocas", fn);
        bodyBB = BasicBlock::Create(ctx, "entry", fn);
        ab.SetInsertPoint(allocaBB);

        // parameters
        size_t i = 0;
        for (auto& a : fn->args()) {
            LocalVar* pl = i < f->param_locals.size() ? f->param_locals[i].get() : nullptr;
            if (pl) {
                const bool iref = i < f->pty_ref.size() && f->pty_ref[i];
                LType* slotTy = (pl->by_ref || iref) ? ptrTy() : lt(pl->rty);
                auto* slot = entryAlloca(slotTy, sane(pl->name), alignOf(pl->rty));
                slots[pl] = slot;
                b.SetInsertPoint(bodyBB);
                b.CreateStore(&a, slot);
                a.setName(pl->name);
            }
            ++i;
        }
        b.SetInsertPoint(bodyBB);
        // Lambda bodies: the hidden first argument is the environment; captured locals resolve to its
        // fields (U41). Restored by the caller-visible state below (nested emission is not recursive).
        lambdaEnvIndex.clear();
        lambdaEnvArg = nullptr;
        lambdaEnvTy = nullptr;
        if ((!f->capture_locals.empty() || f->this_capture_index >= 0) && !f->pty.empty()) {
            lambdaEnvArg = fn->getArg(0);
            lambdaEnvTy = f->pty[0];
            for (auto& cl : f->capture_locals) {
                if (cl && cl->env_index >= 0) lambdaEnvIndex[cl.get()] = cl->env_index;
            }
        }
        // `[this]` capture: the body's `this` is the environment field. A class field holds the handle;
        // a struct field holds the object value (so `thisSlot` points at that field) or, with
        // `[ref this]`, the object address.
        if (f->this_capture_ty && lambdaEnvArg && f->this_capture_index >= 0) {
            curClassTy = f->this_capture_ty;
            Value* field = fieldAddr(lambdaEnvArg, f->pty[0], f->this_capture_index);
            thisSlot = entryAlloca(ptrTy(), "this");
            b.SetInsertPoint(bodyBB);
            if (f->this_capture_ty->kind == TypeKind::Struct) {
                if (f->this_capture_ref) {
                    b.CreateStore(b.CreateLoad(ptrTy(), field), thisSlot);  // object address
                } else {
                    b.CreateStore(field, thisSlot);  // address of the captured copy
                }
            } else {
                b.CreateStore(b.CreateLoad(ptrTy(), field), thisSlot);  // the handle
            }
        }
        ownParams(f->param_locals);
        block(f->body.get());
        popRefScope();
        if (!hasTerm(b.GetInsertBlock())) {
            if (is_void(f->rty)) {
                b.CreateRetVoid();
            } else if (f->is_entry) {
                b.CreateRet(ConstantInt::get(i32(), 0));
            } else {
                b.CreateUnreachable();
            }
        }
        ab.SetInsertPoint(allocaBB);
        ab.CreateBr(bodyBB);

        std::string err;
        raw_string_ostream os(err);
        if (verifyFunction(*fn, &os)) {
            throw MuError("LLVM backend: function `" + f->name + "` produced invalid IR: " + os.str());
        }
        curFn = nullptr;
        allocaBB = nullptr;
        bodyBB = nullptr;
        slots.clear();
    }

    void Emitter::emitGlobals() {
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto* g = dynamic_cast<GlobalVar*>(d.get());
                if (!g || g->is_extern) continue;
                LType* ty = lt(g->rty);
                Constant* init = nullptr;
                if (g->runtime_init) {
                    // 9.1: the data area only holds the zero value; the unit init sequence does the work.
                    init = zeroConst(ty, g->rty);
                } else if (g->const_func) {
                    FuncDecl* f = g->const_func;
                    std::vector<LType*> ptys;
                    for (auto& pt : f->pty) ptys.push_back(lt(pt));
                    init = llvm::cast<Constant>(
                        mod->getOrInsertFunction(f->cname,
                                                 FunctionType::get(lt(f->rty), ptys, f->is_varargs))
                            .getCallee());
                } else if (g->const_evaluated) {
                    init = constOfValue(g->rty, g->const_is_float, g->const_ival, g->const_fval);
                } else {
                    init = g->init ? constValue(g->rty, g->init.get())
                                   : zeroConst(ty, g->rty);
                }
                auto linkage = g->is_public ? GlobalValue::ExternalLinkage
                                            : GlobalValue::InternalLinkage;
                auto* gv = new GlobalVariable(*mod, ty, g->is_const, linkage, init, g->cname);
                if (g->is_const) gv->setConstant(true);
                uint64_t ga = alignOf(g->rty);
                if (ga > 1) gv->setAlignment(llvm::Align(ga));
            }
        }
    }

    void Emitter::initDebug() {
        if (!wantDebug) return;
        // use the source file of the entry unit as the compile unit (the first one in units is usually the implicitly imported std.string)
        std::string file;
        for (auto& u : prog.units) {
            if (u->name == prog.entry) {
                file = u->filename;
                break;
            }
        }
        if (file.empty() && !prog.units.empty()) file = prog.units.front()->filename;
        std::string dir = ".";
        std::string base = file;
        const auto slash = file.find_last_of("/\\");
        if (slash != std::string::npos) {
            dir = file.substr(0, slash);
            base = file.substr(slash + 1);
        }
        dib = std::make_unique<DIBuilder>(*mod);
        diFile = dib->createFile(base, dir);
        diCu = dib->createCompileUnit(
            DISourceLanguageName(llvm::dwarf::DW_LANG_C99), diFile, "muc " MUC_VERSION,
            /*isOptimized=*/false, "", 0);
    }

    void Emitter::run() {
        mayThrow = computeMayThrow(prog);
        collectExportSurface();
        // target
        std::string tripleStr = "x86_64-w64-windows-gnu";
        std::string err;
        llvm::Triple triple(tripleStr);
        const Target* target = TargetRegistry::lookupTarget(triple, err);
        if (!target) throw MuError("LLVM backend: cannot find target " + tripleStr + ": " + err);
        TargetOptions opt;
        tm.reset(target->createTargetMachine(triple, "generic", "", opt,
                                             std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_),
                                             std::nullopt, CodeGenOptLevel::Default));
        mod = std::make_unique<Module>("mulan", ctx);
        mod->setTargetTriple(triple);
        if (tm) mod->setDataLayout(tm->createDataLayout());
        mod->addModuleFlag(Module::Warning, "mulan.version",
                           llvm::MDString::get(ctx, MUC_VERSION));
        initDebug();

        sliceTy = ptrTy();  // dynamic array handle (header pointer)
        descTy = LStruct::create(ctx, "mu_desc");
        descTy->setBody({i64(), ptrTy(), ptrTy(), ptrTy(), ptrTy()}, false);
        ifaceTy = LStruct::create(ctx, "mu_iface");
        ifaceTy->setBody({ptrTy(), ptrTy()}, false);
        delegateTy = LStruct::create(ctx, "mu_delegate");
        delegateTy->setBody({ptrTy(), ptrTy()}, false);  // {code, env}
        weakNodeTy = LStruct::create(ctx, "mu_weak_node");
        weakNodeTy->setBody({ptrTy(), ptrTy(), ptrTy()}, false);
        new GlobalVariable(*mod, ptrTy(), /*isConstant=*/false, GlobalValue::InternalLinkage,
                           Constant::getNullValue(ptrTy()), "mu_weak_head");
        new GlobalVariable(*mod, i32(), /*isConstant=*/false, GlobalValue::InternalLinkage,
                           ConstantInt::get(i32(), 0), "mu_weak_lock");

        // build the struct types first (fields may reference each other; pointers are opaque ptr, so they do not recurse)
        for (auto& u : prog.units) {
            for (auto& d : u->decls) {
                if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                    if (sd->ty) structOf(sd->ty);
                }
            }
        }
        // object layout and class descriptors
        buildIfaceSlots();
        // reserve all descs first (a derived class may reference a base class desc), then fill in the initializers
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;
                objStruct(c->ty);
                new GlobalVariable(*mod, descTy, /*isConstant=*/true, GlobalValue::InternalLinkage,
                                   Constant::getNullValue(descTy),
                                   class_desc_name(c->ty->unit, c->name));
            }
        }
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;
                emitClassDesc(c);
            }
        }

        emitGlobals();

        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    if (f->is_extern) {
                        std::vector<LType*> ptys;
                        for (size_t i = 0; i < f->pty.size(); ++i)
                            ptys.push_back(i < f->pty_ref.size() && f->pty_ref[i] ? ptrTy() : lt(f->pty[i]));
                        mod->getOrInsertFunction(
                            f->cname, FunctionType::get(lt(f->rty), ptys, f->is_varargs));
                    }
                }
            }
        }
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) emitFunction(f);
            }
            for (auto& l : u->lambdas) emitFunction(l.get());
        }
        // class member functions, constructors, destructors, destroy functions; interface default methods
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (c && c->ty) {
                    for (auto& m : c->methods) emitMethod(c, m.get());
                    for (auto& ctor : c->ctors) emitCtor(c, ctor.get());
                    emitDtor(c);
                    emitDestroy(c);
                }
                auto itf = dynamic_cast<InterfaceDecl*>(d.get());
                if (itf && itf->ty) {
                    for (auto& m : itf->methods) emitInterfaceMethod(itf, m.get());
                }
                auto sd = dynamic_cast<StructDecl*>(d.get());
                if (sd && sd->ty) {
                    for (auto& m : sd->methods) emitStructMethod(sd, m.get());
                    for (auto& ctor : sd->ctors) emitStructCtor(sd, ctor.get());
                    emitStructDtor(sd);
                }
            }
        }

        // `para` / `para for` bodies are queued during function emission; emit them last (and
        // repeatedly, because a body may itself contain a `para`) so every enclosing function's
        // state has already been torn down.
        for (bool more = true; more;) {
            more = false;
            if (!pendingParas.empty()) {
                std::vector<PendingPara> batch = std::move(pendingParas);
                pendingParas.clear();
                for (auto& q : batch) emitParaSection(q);
                more = true;
            }
            if (!pendingParaFors.empty()) {
                std::vector<PendingParaFor> batch = std::move(pendingParaFors);
                pendingParaFors.clear();
                for (auto& q : batch) emitParaForBody(q);
                more = true;
            }
        }

        // 9.1: build the per-unit init/fini hooks and the `main` wrapper now that every function
        // (constructors, destructors, user functions) has been emitted.
        emitUnitLifecycle();

        // The task runtime saves/restores the pending-exception state across a fiber switch, so it
        // needs the address of the thread-local slot: `extern void* muc_eh_slot()` in std.task.
        if (Function* slotFn = mod->getFunction("muc_eh_slot");
            slotFn && slotFn->isDeclaration()) {
            ensureEHSlot();
            BasicBlock* bb = BasicBlock::Create(ctx, "entry", slotFn);
            b.SetInsertPoint(bb);
            b.CreateRet(b.CreateBitCast(ehSlot, ptrTy()));
        }

        if (dib) dib->finalize();

        internalizeDefinitions();

        std::string errs;
        raw_string_ostream os(errs);
        if (verifyModule(*mod, &os)) {
            throw MuError("LLVM backend: generated module is invalid: " + os.str());
        }

        // standard -O2 pipeline: lets Str's short methods/operators and the RC helper functions be inlined.
        {
            LoopAnalysisManager LAM;
            FunctionAnalysisManager FAM;
            CGSCCAnalysisManager CGAM;
            ModuleAnalysisManager MAM;
            PassBuilder PB(tm.get());
            PB.registerModuleAnalyses(MAM);
            PB.registerCGSCCAnalyses(CGAM);
            PB.registerFunctionAnalyses(FAM);
            PB.registerLoopAnalyses(LAM);
            PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
            ModulePassManager MPM =
                PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
            MPM.run(*mod, MAM);
        }
    }

    // The export surface: the symbols an external consumer may link against. `ExportOnly` keeps
    // only `export` declarations; `PublicOnly` widens it to every `public` one. A type contributes
    // its public member functions plus the metadata a consumer needs to construct and destroy it
    // (class descriptor, vtable, RC destroy entry).
    void Emitter::collectExportSurface() {
        auto keep = [&](const Decl* d) {
            if (exportMode == ExportMode::PublicOnly) return d->is_public || d->is_export;
            return d->is_export;
        };
        for (auto& u : prog.units) {
            for (auto& d : u->decls) {
                if (!keep(d.get())) continue;
                if (auto* f = dynamic_cast<FuncDecl*>(d.get())) {
                    if (!f->is_extern) exports.insert(f->cname);
                } else if (auto* g = dynamic_cast<GlobalVar*>(d.get())) {
                    if (!g->is_extern) exports.insert(g->cname);
                } else if (auto* c = dynamic_cast<ClassDecl*>(d.get())) {
                    if (!c->ty) continue;
                    exports.insert(class_desc_name(c->ty->unit, c->name));
                    exports.insert(class_vtable_name(c->ty->unit, c->name));
                    exports.insert(destroy_cname(c->ty->unit, c->name));
                    if (c->dtor) exports.insert(c->dtor->cname);
                    for (auto& m : c->methods) {
                        if (m->is_public) exports.insert(m->cname);
                    }
                    for (auto& ct : c->ctors) {
                        if (ct->is_public) exports.insert(ct->cname);
                    }
                } else if (auto* sd = dynamic_cast<StructDecl*>(d.get())) {
                    if (sd->dtor) exports.insert(sd->dtor->cname);
                    for (auto& m : sd->methods) {
                        if (m->is_public) exports.insert(m->cname);
                    }
                    for (auto& ct : sd->ctors) {
                        if (ct->is_public) exports.insert(ct->cname);
                    }
                } else if (auto* itf = dynamic_cast<InterfaceDecl*>(d.get())) {
                    for (auto& m : itf->methods) exports.insert(m->cname);
                }
            }
        }
    }

    // Executable target: no external consumer exists, so every definition can be internalized.
    // `main` stays visible for the CRT, `--export` names stay visible for a C shim or a debugger,
    // and declarations (libc, C shims) are untouched. GlobalDCE then removes whatever the entry
    // cannot reach — including whole classes (desc + vtable + methods) and unused library units.
    void Emitter::internalizeDefinitions() {
        for (auto& f : mod->functions()) {
            if (f.isDeclaration() || f.getName() == "main") continue;
            if (exports.count(f.getName().str())) continue;
            f.setLinkage(GlobalValue::InternalLinkage);
        }
        for (auto& g : mod->globals()) {
            if (!g.hasInitializer() || g.getName() == "main") continue;
            if (exports.count(g.getName().str())) continue;
            g.setLinkage(GlobalValue::InternalLinkage);
        }
    }

}  // namespace muc_detail
