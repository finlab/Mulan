// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains function code generation and the module wrap-up (run) (Emitter member function definitions).
//
// owns:      function emission, globals, debug info, the export surface, internalization and the optimization pipeline (the Emitter entry point run)
// not:       any per-expression or per-statement decision
// pass:      emission, drives the whole backend
// concept:   codegen-driver, codegen-functions

#include "codegen_impl.hpp"

namespace muc_detail {

    // ------------------------------------------------------------ functions
    // The IRBuilder is reused across functions: before entering a new function, clear the stale !dbg location left over from the previous function,
    // otherwise the new function's instructions would point at the previous function's DISubprogram (which the LLVM verifier rejects).
    void Emitter::resetDebugScope() {
        diSub = nullptr;
        b.SetCurrentDebugLocation(DebugLoc());
        ab.SetCurrentDebugLocation(DebugLoc());
        movedLocals.clear();
    }

    // SysV classifies each eightbyte as INTEGER or SSE; the backend hands a float-bearing aggregate one
    // register per field, which disagrees with C's packing for e.g. `{float, float}`. Windows (MS ABI)
    // coerces the whole aggregate through an integer, so it is fine there. Refuse instead of emitting
    // code that silently passes the wrong register set.
    static bool containsFloatField(const TypePtr& t) {
        if (!t) return false;
        if (t->kind == TypeKind::Prim) return t->name == "float" || t->name == "double";
        if (t->kind == TypeKind::Struct) {
            for (const auto& field : t->fields)
                if (containsFloatField(field.type)) return true;
            return false;
        }
        if (t->kind == TypeKind::Array) return containsFloatField(t->elem);
        return false;
    }

    void Emitter::emitFunction(FuncDecl* f) {
        if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm] emit " << f->name << "\n";
        if (f->c_abi && !isWindowsTarget()) {
            auto check = [&](const TypePtr& t) {
                if (!t || t->kind != TypeKind::Struct) return;
                if (containsFloatField(t)) {
                    throw MuError("SysV C ABI with a floating-point aggregate is not implemented yet "
                                  "(function `" + f->name + "`, target `" + targetTriple +
                                  "`); pass the fields as scalars instead");
                }
                // SysV puts an aggregate whose field is unaligned, or straddles an eightbyte, in class
                // MEMORY (passed by pointer). Packing it through two INTEGER registers would silently
                // move that field, so refuse instead.
                for (size_t i = 0; i < t->fields.size(); ++i) {
                    const TypePtr& ft = t->fields[i].type;
                    const uint64_t off = type_member_offset(t, i);
                    const uint64_t fsz = type_size(ft);
                    const uint64_t fal = type_align(ft);
                    const bool unaligned = fal != 0 && off % fal != 0;
                    const bool straddles = fsz != 0 && (off / 8) != ((off + fsz - 1) / 8);
                    if (unaligned || straddles) {
                        throw MuError("SysV C ABI with an unaligned (packed) aggregate is not implemented "
                                      "yet (function `" + f->name + "`, target `" + targetTriple +
                                      "`); pass the fields as scalars instead");
                    }
                }
            };
            for (const auto& pt : f->pty) check(pt);
            check(f->rty);
        }
        Function* fn = nullptr;
        if (f->c_abi) {
            fn = llvm::dyn_cast<Function>(cAbiCallee(f).getCallee());
        } else {
            std::vector<LType*> ptys;
            for (size_t i = 0; i < f->pty.size(); ++i)
                ptys.push_back(i < f->pty_ref.size() && f->pty_ref[i] ? ptrTy() : lt(f->pty[i]));
            auto* ft = FunctionType::get(lt(f->rty), ptys, false);
            fn = llvm::dyn_cast<Function>(mod->getOrInsertFunction(f->cname, ft).getCallee());
        }
        if (!fn) {
            throw MuError("LLVM backend: `" + f->cname +
                          "` is already declared with a different signature (same C symbol, two Mu types?)");
        }
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

        // parameters. An `sret` function receives a hidden first argument (the address of the
        // caller's result slot), and a `byval` parameter arrives as a pointer to the caller's copy.
        curSretSlot = nullptr;
        curCoerceRet = nullptr;
        size_t argBase = 0;
        if (f->c_abi) {
            if (abi_class(f->rty, /*is_return=*/true, targetTriple) == AbiPass::Sret) {
                curSretSlot = fn->getArg(0);
                curSretSlot->setName("sret");
                argBase = 1;
            } else {
                curCoerceRet =
                    coercedType(abi_class(f->rty, /*is_return=*/true, targetTriple), f->rty);
            }
        }
        size_t i = 0;
        for (auto& a : fn->args()) {
            if (i < argBase) {
                ++i;
                continue;
            }
            const size_t pi = i - argBase;
            LocalVar* pl = pi < f->param_locals.size() ? f->param_locals[pi].get() : nullptr;
            if (pl) {
                const TypePtr pt = pi < f->pty.size() ? f->pty[pi] : pl->rty;
                const bool iref = pi < f->pty_ref.size() && f->pty_ref[pi];
                const AbiPass apass =
                    f->c_abi && !iref ? abi_class(pt, /*is_return=*/false, targetTriple)
                                      : AbiPass::Direct;
                LType* slotTy = (pl->by_ref || iref) ? ptrTy() : lt(pl->rty);
                auto* slot = entryAlloca(slotTy, sane(pl->name), alignOf(pl->rty));
                slots[pl] = slot;
                b.SetInsertPoint(bodyBB);
                Value* v = nullptr;
                if (apass == AbiPass::ByVal) {
                    // the callee receives a pointer to the caller's copy
                    v = b.CreateLoad(lt(pt), &a);
                } else if (apass != AbiPass::Direct) {
                    v = coerceFromReg(&a, pt);  // packed into a register by C
                } else {
                    v = &a;
                }
                b.CreateStore(v, slot);
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
        // `[weak …]` captures: the environment holds an uncounted reference and the weak registry clears
        // the slot when the target is destroyed, so a null slot means the captured object is gone. The
        // delegate then has no receiver to act on: return before running the body. This also makes every
        // weak capture non-null inside the body, so no further checks are needed there.
        BasicBlock* prologueBB = bodyBB;
        if (lambdaEnvArg && !f->weak_capture_indices.empty()) {
            Value* dead = Constant::getNullValue(i1());
            for (int idx : f->weak_capture_indices) {
                Value* slot = fieldAddr(lambdaEnvArg, f->pty[0], idx);
                dead = b.CreateOr(dead, b.CreateICmpEQ(b.CreateLoad(ptrTy(), slot),
                                                       Constant::getNullValue(ptrTy())));
            }
            auto* gone = BasicBlock::Create(ctx, "lambda.dead", curFn);
            auto* live = BasicBlock::Create(ctx, "lambda.live", curFn);
            b.CreateCondBr(dead, gone, live);
            b.SetInsertPoint(gone);
            if (is_void(f->rty)) {
                b.CreateRetVoid();
            } else {
                b.CreateRet(zeroConst(lt(f->rty), f->rty));
            }
            prologueBB = live;
            b.SetInsertPoint(prologueBB);
        }
        // `[this]` capture: the body's `this` is the environment field. A class field holds the handle;
        // a struct field holds the object value (so `thisSlot` points at that field) or, with
        // `[ref this]`, the object address.
        if (f->this_capture_ty && lambdaEnvArg && f->this_capture_index >= 0) {
            curClassTy = f->this_capture_ty;
            Value* field = fieldAddr(lambdaEnvArg, f->pty[0], f->this_capture_index);
            thisSlot = entryAlloca(ptrTy(), "this");
            b.SetInsertPoint(prologueBB);
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
        curSretSlot = nullptr;
        curCoerceRet = nullptr;
        allocaBB = nullptr;
        bodyBB = nullptr;
        slots.clear();
    }

    void Emitter::emitGlobals() {
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto* g = dynamic_cast<GlobalVar*>(d.get());
                if (!g || g->is_extern || g->is_builtin) continue;
                LType* ty = lt(g->rty);
                Constant* init = nullptr;
                if (g->runtime_init) {
                    // 9.1: the data area only holds the zero value; the unit init sequence does the work.
                    init = zeroConst(ty, g->rty);
                } else if (g->const_func) {
                    FuncDecl* f = g->const_func;
                    checkFnValueAbi(f, g);  // the global stores the Mu function value
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
                const bool llvm_const = g->is_const && !g->runtime_init;
                auto* gv = new GlobalVariable(*mod, ty, llvm_const, linkage, init, g->cname);
                if (llvm_const) gv->setConstant(true);
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
            // LLVM < 23 takes the language as `unsigned`, >= 23 as a `DISourceLanguageName`
            // wrapper class; the plain DWARF enum converts implicitly to both, a cast to either
            // type breaks the other.
            llvm::dwarf::DW_LANG_C99, diFile, "muc " MUC_VERSION " (build " MUC_BUILD ")",
            /*isOptimized=*/false, "", 0);
    }

    void Emitter::run() {
        mayThrow = computeMayThrow(prog);
        collectExportSurface();
        // target
        std::string tripleStr = targetTriple.empty() ? llvm::sys::getDefaultTargetTriple() : targetTriple;
        targetTriple = tripleStr;
        std::string err;
        llvm::Triple triple(tripleStr);
        // LLVM 19 moved the module-level target APIs from `const std::string&` to `const Triple&`.
#if LLVM_VERSION_MAJOR >= 19
        const Target* target = TargetRegistry::lookupTarget(triple, err);
#else
        const Target* target = TargetRegistry::lookupTarget(tripleStr, err);
#endif
        if (!target) throw MuError("LLVM backend: cannot find target " + tripleStr + ": " + err);
        TargetOptions opt;
#if LLVM_VERSION_MAJOR >= 19
        tm.reset(target->createTargetMachine(triple, "generic", "", opt,
#else
        tm.reset(target->createTargetMachine(tripleStr, "generic", "", opt,
#endif
                                             std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_),
                                             std::nullopt, CodeGenOptLevel::Default));
        mod = std::make_unique<Module>("mulan", ctx);
#if LLVM_VERSION_MAJOR >= 19
        mod->setTargetTriple(triple);
#else
        mod->setTargetTriple(tripleStr);
#endif
        if (tm) mod->setDataLayout(tm->createDataLayout());
        mod->addModuleFlag(Module::Warning, "mulan.version",
                           llvm::MDString::get(ctx, MUC_VERSION));
        mod->addModuleFlag(Module::Warning, "mulan.build",
                           llvm::MDString::get(ctx, MUC_BUILD));
        initDebug();

        sliceTy = ptrTy();  // dynamic array handle (header pointer)
        descTy = LStruct::create(ctx, "mu_desc");
        // {size, destroy, vtable, itable, base, name}: `name` is the qualified class name (an i8*
        // literal), used by the uncaught-exception report.
        descTy->setBody({i64(), ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()}, false);
        ifaceTy = LStruct::create(ctx, "mu_iface");
        ifaceTy->setBody({ptrTy(), ptrTy()}, false);
        delegateTy = LStruct::create(ctx, "mu_delegate");
        delegateTy->setBody({ptrTy(), ptrTy()}, false);  // {code, env}
        weakNodeTy = LStruct::create(ctx, "mu_weak_node");
        weakNodeTy->setBody({ptrTy(), ptrTy(), ptrTy()}, false);
        // The weak registry is sharded by target address: every shard has its own list and its own
        // spin lock, so unregistering walks O(W / kWeakShards) nodes and threads that touch different
        // targets do not serialise on one cache line (#18). The object layout is untouched.
        auto* weakHeadsTy = llvm::ArrayType::get(ptrTy(), kWeakShards);
        new GlobalVariable(*mod, weakHeadsTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
                           Constant::getNullValue(weakHeadsTy), "mu_weak_heads");
        auto* weakLocksTy = llvm::ArrayType::get(i32(), kWeakShards);
        new GlobalVariable(*mod, weakLocksTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
                           Constant::getNullValue(weakLocksTy), "mu_weak_locks");

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
        collectClassTypes();  // closed-world devirtualization needs the full class set
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

        // Extern declarations are created by emitFunction as well (it returns right after the
        // signature is in place), always through cAbiCallee so `byval`/`sret` match the call sites.
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get()); f && !f->is_builtin) emitFunction(f);
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
                    emitDtorPartial(c);
                    emitDestroyPartialFn(c);
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
                    emitStructDtorPartial(sd);
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
        // Debug aid: the verifier accepts an over-wide store (that is UB, not invalid IR), and such a bug
        // only shows up as "the optimizer deleted my code" (agents/llvm-assume-notes.md).
        if (std::getenv("MUC_VERIFY_MEM")) verifyStackWrites();

        // standard -O2 pipeline: lets Str's short methods/operators and the RC helper functions be inlined.
        // `MUC_NO_OPT=1` drops it to -O0: `--emit-ir` prints the module after this point, so a debugging
        // session needs a way to see what was emitted before the inliner rewrote it.
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
            const char* no_opt = std::getenv("MUC_NO_OPT");
            const bool disable_opt =
                no_opt && *no_opt && std::strcmp(no_opt, "0") != 0 && std::strcmp(no_opt, "false") != 0 &&
                std::strcmp(no_opt, "off") != 0;
            ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(
                disable_opt ? llvm::OptimizationLevel::O0 : llvm::OptimizationLevel::O2);
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
