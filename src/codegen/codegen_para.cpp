// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// `para` / `para for` emission and the task-runtime declarations they call (Emitter member function
// definitions).
//
// owns:      the section lowering (`emitPara`), `para for` (spawn item, body, section) and `runtimeFn`
//            (declares a `std.task` entry point on demand)
// not:       ordinary statement dispatch (codegen_stmt.cpp), expression evaluation, class metadata
// pass:      emission, after every type and body check
// concept:   codegen-para

#include "codegen_impl.hpp"

namespace muc_detail {

    // ---------------------------------------------------------------- para (3.17.2)

    FunctionCallee Emitter::runtimeFn(const char* unit, const char* name, LType* ret,
                                      std::vector<LType*> params) {
        for (auto& u : prog.units) {
            if (u->name != unit) continue;
            for (auto& d : u->decls) {
                auto* f = dynamic_cast<FuncDecl*>(d.get());
                if (f && f->name == name) {
                    return mod->getOrInsertFunction(f->cname, FunctionType::get(ret, params, false));
                }
            }
        }
        fail(std::string("LLVM backend: runtime function `") + unit + "." + name +
                 "` is missing (std.task is required for `para`)",
             0, 0);
    }

    void Emitter::emitPara(ParaStmt* p) {
        FunctionCallee join = runtimeFn("std.task", "muc_para_join", LType::getVoidTy(ctx), {ptrTy()});
        std::vector<Value*> handles;
        for (size_t si = 0; si < p->sections.size(); ++si) {
            static const std::vector<LocalVar*> kNoCaps;
            const std::vector<LocalVar*>& caps = si < p->captures.size() ? p->captures[si] : kNoCaps;

            // One frame per section: one pointer-sized field per captured local, holding the address
            // of the caller's slot. The caller is suspended at the join below, so those slots live
            // for as long as any section can read them.
            std::vector<LType*> fields(caps.size(), ptrTy());
            auto* ft = LStruct::get(ctx, fields);
            auto* frame = entryAlloca(ft, "para.frame", 8);
            for (size_t j = 0; j < caps.size(); ++j) {
                Value* slot = slotFor(caps[j]);
                b.CreateStore(b.CreateBitCast(slot, ptrTy()),
                              b.CreateStructGEP(cast<LStruct>(ft), frame, static_cast<unsigned>(j)));
            }

            PendingPara pp;
            pp.body = p->sections[si].get();
            pp.captures = caps;
            pp.frameTy = ft;
            const std::string base = "mu_para_" + std::to_string(paraSeq++);
            auto* sig = FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false);
            pp.bodyFn = Function::Create(sig, GlobalValue::InternalLinkage, base + "_body", *mod);
            pp.wrapFn = Function::Create(sig, GlobalValue::InternalLinkage, base + "_sec", *mod);
            pendingParas.push_back(pp);

            FunctionCallee start =
                runtimeFn("std.task", "muc_para_start", ptrTy(), {ptrTy(), ptrTy()});
            Value* h = b.CreateCall(start, {pp.wrapFn, b.CreateBitCast(frame, ptrTy())});
            handles.push_back(h);
            // Starting a section can fail (the runtime caps live execution units); that is an
            // ordinary exception, so join the sections that did start before the region unwinds.
            std::vector<Value*> started(handles.begin(), handles.end() - 1);
            emitCheckPendingWith([&] {
                for (Value* prev : started) b.CreateCall(join, {prev});
            });
        }
        // Structured join: every section finishes before the statement completes.
        for (Value* h : handles) b.CreateCall(join, {h});
    }

    LStruct* Emitter::paraCtxTy(const TypePtr& elemTy, bool byRef) {
        return LStruct::get(ctx, {ptrTy(), byRef ? ptrTy() : lt(elemTy)});
    }

    void Emitter::emitParaFor(ParaForStmt* p) {
        ForInStmt* f = p->loop.get();
        if (!f) return;

        // Captures: one frame of pointers, shared by every item task (they are read-only). The
        // producer is suspended at the join below, so the caller's slots outlive every task.
        std::vector<LType*> fields(p->captures.size(), ptrTy());
        auto* ft = LStruct::get(ctx, fields);
        auto* frame = entryAlloca(ft, "para.frame", 8);
        for (size_t j = 0; j < p->captures.size(); ++j) {
            Value* slot = slotFor(p->captures[j]);
            b.CreateStore(b.CreateBitCast(slot, ptrTy()),
                          b.CreateStructGEP(cast<LStruct>(ft), frame, static_cast<unsigned>(j)));
        }

        const std::string base = "mu_para_for_" + std::to_string(paraSeq++);
        auto* sig = FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false);
        Function* bodyFn = Function::Create(sig, GlobalValue::InternalLinkage, base + "_body", *mod);
        Function* wrapFn = Function::Create(sig, GlobalValue::InternalLinkage, base + "_sec", *mod);

        // The region's task list: every item task is recorded so the source end can join them all.
        FunctionCallee groupNew = runtimeFn("std.task", "muc_para_group_new", ptrTy(), {});
        auto* groupSlot = entryAlloca(ptrTy(), "para.group", 8);
        b.CreateStore(b.CreateCall(groupNew, {}), groupSlot);

        PendingParaFor q;
        q.p = p;
        q.bodyFn = bodyFn;
        q.wrapFn = wrapFn;
        q.frameTy = ft;
        q.ctxTy = paraCtxTy(f->elem_ty, f->by_ref);
        q.elemTy = f->elem_ty;
        q.byRef = f->by_ref;
        pendingParaFors.push_back(q);

        // The ordinary for-in lowering produces the items (one pull per iteration); its body is
        // replaced by "start one task for the current item" (SPEC 3.17.3: one task per item).
        ParaForSpawn sp;
        sp.wrapFn = wrapFn;
        sp.frame = b.CreateBitCast(frame, ptrTy());
        sp.groupSlot = groupSlot;
        sp.elemTy = f->elem_ty;
        sp.byRef = f->by_ref;
        paraSpawn = &sp;
        // Unwinding out of the source (a throwing `next()` / `operator[]` / source expression) must
        // join what already started; the callback runs before any scope release.
        paraUnwindCleanup.push_back([this, groupSlot] {
            FunctionCallee jf = runtimeFn("std.task", "muc_para_group_join_free",
                                          LType::getVoidTy(ctx), {ptrTy()});
            b.CreateCall(jf, {b.CreateLoad(ptrTy(), groupSlot)});
        });
        emitFor(f);
        paraUnwindCleanup.pop_back();
        paraSpawn = nullptr;

        // Structured: the region ends when the source ends, and every task is joined here.
        FunctionCallee joinFree =
            runtimeFn("std.task", "muc_para_group_join_free", LType::getVoidTy(ctx), {ptrTy()});
        b.CreateCall(joinFree, {b.CreateLoad(ptrTy(), groupSlot)});
    }

    // One item: the producer has already pulled the item into the loop variable's slot. Copy it into
    // a heap context `{ frame*, item }`, start the task and record the handle in the task list.
    void Emitter::emitParaForSpawnItem(Value* slot) {
        ParaForSpawn* sp = paraSpawn;
        const TypePtr et = sp->elemTy;
        auto* ct = cast<LStruct>(paraCtxTy(et, sp->byRef));
        const uint64_t bytes = mod->getDataLayout().getTypeAllocSize(ct);
        Value* mem =
            b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {ConstantInt::get(i64(), bytes)});
        // OOM must abort cleanly like every other allocation, not write through null (#222).
        BasicBlock* memOk = BasicBlock::Create(ctx, "spawn.mem", curFn);
        BasicBlock* memBad = BasicBlock::Create(ctx, "spawn.oom", curFn);
        b.CreateCondBr(b.CreateICmpEQ(mem, Constant::getNullValue(ptrTy())), memBad, memOk);
        b.SetInsertPoint(memBad);
        b.CreateCall(abortTrap(), {});
        b.CreateUnreachable();
        b.SetInsertPoint(memOk);
        b.CreateStore(sp->frame, b.CreateStructGEP(ct, mem, 0));
        Value* fld = b.CreateStructGEP(ct, mem, 1);
        if (sp->byRef) {
            // `for ref`: the context holds the element address. The container outlives the region
            // (no capture escapes), so nothing is retained and no borrow is stored.
            b.CreateStore(b.CreateLoad(ptrTy(), slot), fld);
        } else {
            b.CreateStore(b.CreateLoad(lt(et), slot), fld);
            // The task owns its copy; the producer still releases its own iteration value below.
            if (needsCleanup(et)) {
                if (isRefKind(et)) emitRetainRef(b.CreateLoad(lt(et), fld), et);
                else emitRetainValue(fld, et);
            }
        }

        FunctionCallee startFn =
            runtimeFn("std.task", "muc_para_start", ptrTy(), {ptrTy(), ptrTy()});
        Value* h = b.CreateCall(startFn, {sp->wrapFn, mem});

        // A failed spawn is an ordinary exception (SPEC 3.17.7): release this item's resources and
        // then unwind like any other failed call (the region's unwind cleanup joins the tasks that
        // already started, see `paraUnwindCleanup`). The check comes *before* the handle is recorded:
        // a refused start returns no handle, so nothing undefined may enter the task list.
        const bool ownCopy = !sp->byRef && needsCleanup(et);
        emitCheckPendingWith([&] {
            if (ownCopy) {
                // same release shape as a local variable (releaseLocal)
                if (isRefKind(et)) {
                    emitReleaseRef(b.CreateLoad(lt(et), fld), et);
                } else if (et->kind == TypeKind::Struct && et->has_dtor) {
                    FunctionCallee dtor = mod->getOrInsertFunction(
                        dtor_cname(et->unit, et->name),
                        FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                    b.CreateCall(dtor, {fld});
                } else {
                    emitReleaseValue(fld, et);
                }
            }
            b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {mem});
        });

        FunctionCallee addFn =
            runtimeFn("std.task", "muc_para_group_add", LType::getVoidTy(ctx), {ptrTy(), ptrTy()});
        b.CreateCall(addFn, {b.CreateLoad(ptrTy(), sp->groupSlot), h});
    }

    // One item body: `void(ctx)` with ctx = { frame*, item }. The loop variable is bound from the
    // context (by value, or as the element address for `for ref`); the body is otherwise unchanged.
    // Emitted after the enclosing function, like a `para` section body.
    void Emitter::emitParaForBody(PendingParaFor q) {
        ParaForStmt* p = q.p;
        ForInStmt* f = p->loop.get();
        LType* ft = q.frameTy;
        auto* ct = cast<LStruct>(q.ctxTy);
        Function* bodyFn = q.bodyFn;
        Function* wrapFn = q.wrapFn;

        resetDebugScope();
        diSub = nullptr;
        curClassTy = nullptr;
        curThis = nullptr;
        thisSlot = nullptr;
        refScopes.clear();
        loopScopeDepth.clear();
        loops.clear();  // reak/continue cannot cross a section (sema), so no entry may survive
        stmtTemps.clear();
        ehStack.clear();
        finallyStack.clear();
        abortBases.clear();
        ehLocals.clear();
        slots.clear();
        paraUnwindCleanup.clear();
        curFn = bodyFn;
        curRetTy = Type::make_void();
        curIsEntry = false;
        inNothrow = false;
        allocaBB = BasicBlock::Create(ctx, "allocas", bodyFn);
        bodyBB = BasicBlock::Create(ctx, "entry", bodyFn);
        ab.SetInsertPoint(allocaBB);
        b.SetInsertPoint(bodyBB);

        // Unpack the context.
        Value* arg = bodyFn->getArg(0);
        frameArg = b.CreateLoad(ptrTy(), b.CreateStructGEP(ct, arg, 0));
        frameTy = ft;
        captureIndex.clear();
        for (size_t i = 0; i < p->captures.size(); ++i)
            captureIndex[p->captures[i]] = static_cast<int>(i);

        // Bind the loop variable to the item.
        Value* fld = b.CreateStructGEP(ct, arg, 1);
        Value* varSlot = slotFor(f->lvar.get());
        if (q.byRef) b.CreateStore(b.CreateLoad(ptrTy(), fld), varSlot);
        else b.CreateStore(b.CreateLoad(lt(q.elemTy), fld), varSlot);

        // The item copy is an owning local of the body (SPEC §4); a borrowed element is not released.
        const bool own = !q.byRef && needsCleanup(q.elemTy);
        pushRefScope();
        if (own) registerLocalCleanup(f->lvar.get());
        block(f->body.get());
        popRefScope();
        if (!hasTerm(b.GetInsertBlock())) b.CreateRetVoid();
        ab.SetInsertPoint(allocaBB);
        ab.CreateBr(bodyBB);

        captureIndex.clear();
        frameArg = nullptr;
        frameTy = nullptr;
        slots.clear();

        // Wrapper: call the body, free the per-item context, abort if an exception escaped it
        // (same v1 thread-boundary rule as `para`).
        BasicBlock* we = BasicBlock::Create(ctx, "entry", wrapFn);
        b.SetInsertPoint(we);
        b.CreateCall(bodyFn, {wrapFn->getArg(0)});
        b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {wrapFn->getArg(0)});
        Value* flag = loadPendingFlag();
        BasicBlock* aborted = BasicBlock::Create(ctx, "pf.abort", wrapFn);
        BasicBlock* done = BasicBlock::Create(ctx, "pf.done", wrapFn);
        b.CreateCondBr(b.CreateICmpNE(flag, ConstantInt::get(i32(), 0)), aborted, done);
        b.SetInsertPoint(aborted);
        b.CreateCall(abortTrap(), {});
        b.CreateUnreachable();
        b.SetInsertPoint(done);
        b.CreateRetVoid();

        curFn = nullptr;
        allocaBB = nullptr;
        bodyBB = nullptr;
    }

    void Emitter::emitParaSection(PendingPara p) {
        // ---- the section body: void(ptr frame); captured locals resolve through the frame
        Function* fn = p.bodyFn;
        resetDebugScope();
        diSub = nullptr;
        curClassTy = nullptr;
        curThis = nullptr;
        thisSlot = nullptr;
        refScopes.clear();
        loopScopeDepth.clear();
        loops.clear();  // reak/continue cannot cross a section (sema), so no entry may survive
        stmtTemps.clear();
        ehStack.clear();
        finallyStack.clear();
        abortBases.clear();
        ehLocals.clear();
        slots.clear();
        paraUnwindCleanup.clear();
        curFn = fn;
        curRetTy = Type::make_void();
        curIsEntry = false;
        inNothrow = false;
        allocaBB = BasicBlock::Create(ctx, "allocas", fn);
        bodyBB = BasicBlock::Create(ctx, "entry", fn);
        ab.SetInsertPoint(allocaBB);
        b.SetInsertPoint(bodyBB);
        frameArg = fn->getArg(0);
        frameTy = p.frameTy;
        captureIndex.clear();
        for (size_t i = 0; i < p.captures.size(); ++i)
            captureIndex[p.captures[i]] = static_cast<int>(i);

        block(p.body);
        if (!hasTerm(b.GetInsertBlock())) b.CreateRetVoid();
        ab.SetInsertPoint(allocaBB);
        ab.CreateBr(bodyBB);

        captureIndex.clear();
        frameArg = nullptr;
        frameTy = nullptr;
        slots.clear();

        // ---- the thread entry: call the body, then abort if an exception escaped it.
        // v1 has no cross-thread exception transfer (SPEC 3.17 note), so this is the B2
        // "uncaught at a thread boundary" rule.
        Function* wf = p.wrapFn;
        BasicBlock* we = BasicBlock::Create(ctx, "entry", wf);
        b.SetInsertPoint(we);
        b.CreateCall(fn, {wf->getArg(0)});
        Value* flag = loadPendingFlag();
        BasicBlock* aborted = BasicBlock::Create(ctx, "para.abort", wf);
        BasicBlock* done = BasicBlock::Create(ctx, "para.done", wf);
        b.CreateCondBr(b.CreateICmpNE(flag, ConstantInt::get(i32(), 0)), aborted, done);
        b.SetInsertPoint(aborted);
        b.CreateCall(abortTrap(), {});
        b.CreateUnreachable();
        b.SetInsertPoint(done);
        b.CreateRetVoid();

        curFn = nullptr;
        allocaBB = nullptr;
        bodyBB = nullptr;
    }

}  // namespace muc_detail
