// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Exception code generation (B2: thread-local pending slot + call-site checks).
//
// The language ABI is unchanged: a function that throws simply returns a dummy
// value with `muc_eh_pending.flag` set.  Every mu_lan call site checks the flag,
// runs the local cleanups, and either branches to the innermost `try` dispatch
// or returns to its caller.  Uncaught exceptions abort at `main`.
#include "codegen_impl.hpp"

namespace muc_detail {

    void Emitter::ensureEHSlot() {
        if (ehSlot) return;
        auto* st = LStruct::get(ctx, {i32(), ptrTy()});
        ehSlot = new GlobalVariable(*mod, st, /*isConstant=*/false,
                                    GlobalValue::InternalLinkage,
                                    ConstantAggregateZero::get(st), "muc_eh_pending");
        ehSlot->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
        ehSlot->setAlignment(llvm::Align(8));
    }

    Value* Emitter::loadPendingFlag() {
        ensureEHSlot();
        auto* st = cast<LStruct>(ehSlot->getValueType());
        return b.CreateLoad(i32(), b.CreateStructGEP(st, ehSlot, 0));
    }

    Value* Emitter::loadPendingObj() {
        ensureEHSlot();
        auto* st = cast<LStruct>(ehSlot->getValueType());
        return b.CreateLoad(ptrTy(), b.CreateStructGEP(st, ehSlot, 1));
    }

    void Emitter::storePending(Value* obj) {
        ensureEHSlot();
        auto* st = cast<LStruct>(ehSlot->getValueType());
        b.CreateStore(obj, b.CreateStructGEP(st, ehSlot, 1));
        b.CreateStore(ConstantInt::get(i32(), 1), b.CreateStructGEP(st, ehSlot, 0));
    }

    void Emitter::storePendingState(Value* obj, Value* flag) {
        ensureEHSlot();
        auto* st = cast<LStruct>(ehSlot->getValueType());
        b.CreateStore(obj, b.CreateStructGEP(st, ehSlot, 1));
        b.CreateStore(flag, b.CreateStructGEP(st, ehSlot, 0));
    }

    void Emitter::clearPending() {
        storePendingState(Constant::getNullValue(ptrTy()), ConstantInt::get(i32(), 0));
    }

    void Emitter::emitReturnDummy() {
        if (!curRetTy || is_void(curRetTy)) {
            b.CreateRetVoid();
        } else {
            b.CreateRet(Constant::getNullValue(lt(curRetTy)));
        }
    }

    // Emit releases for owned statement temporaries created at/after `mark`, without resizing
    // the compile-time vector (the normal and unwind paths each get their own copy of the cleanup).
    void Emitter::emitReleaseTempsFrom(size_t mark) {
        if (stmtTemps.size() <= mark) return;
        for (size_t i = stmtTemps.size(); i > mark; --i) {
            emitReleaseValue(stmtTemps[i - 1].first, stmtTemps[i - 1].second);
        }
    }

    // The current `throw`/failed-check target: innermost try dispatch, or the caller.
    void Emitter::emitUnwindCurrent(const std::function<void()>& onThrow) {
        // A destructor / finally may still catch locally (a try inside it); only abort when
        // the exception would escape that cleanup context.
        if (!abortBases.empty() && ehStack.size() <= abortBases.back()) {
            b.CreateCall(libFn("abort", LType::getVoidTy(ctx), {}), {});
            b.CreateUnreachable();
            return;
        }
        if (ehStack.empty() && (inNothrow || curIsEntry)) {
            b.CreateCall(libFn("abort", LType::getVoidTy(ctx), {}), {});
            b.CreateUnreachable();
            return;
        }

        // Cleanup runs with the pending flag cleared: otherwise a destructor that calls an
        // ordinary mu_lan function would see the exception being unwound and abort.
        const bool hasTry = !ehStack.empty();
        EHContext frame{};
        if (hasTry) frame = ehStack.back();
        Value* saved = loadPendingObj();
        clearPending();
        if (onThrow) onThrow();

        // Run the finally bodies that are being left, before releasing the scopes they may
        // still observe.  `finallyDepth` is the stack size after pushing the target try's own
        // finally, so a throw from the try body does not run it before its catch.
        size_t finallyCount = finallyStack.size();
        if (hasTry) {
            finallyCount = finallyCount > frame.finallyDepth ? finallyCount - frame.finallyDepth : 0;
        }
        emitFinallyBodies(finallyCount);

        emitReleaseTempsFrom(hasTry ? frame.tempDepth : 0);
        if (hasTry) {
            releaseScopesDeeperThan(frame.scopeDepth);
        } else {
            releaseAllScopes();
        }

        // A destructor that throws while unwinding is a nested exception: terminate.
        BasicBlock* ok = BasicBlock::Create(ctx, "unwind.ok", curFn);
        BasicBlock* nested = BasicBlock::Create(ctx, "unwind.nested", curFn);
        b.CreateCondBr(b.CreateICmpNE(loadPendingFlag(), ConstantInt::get(i32(), 0)), nested, ok);
        b.SetInsertPoint(nested);
        b.CreateCall(libFn("abort", LType::getVoidTy(ctx), {}), {});
        b.CreateUnreachable();

        b.SetInsertPoint(ok);
        storePending(saved);
        if (hasTry) {
            b.CreateBr(frame.dispatch);
        } else {
            emitReturnDummy();
        }
    }

    // Called immediately after a may-throw call and its argument/receiver cleanup.
    void Emitter::emitCheckPending() {
        emitCheckPendingWith({});
    }

    void Emitter::emitCheckPendingWith(const std::function<void()>& onThrow) {
        ensureEHSlot();
        BasicBlock* ok = BasicBlock::Create(ctx, "eh.ok", curFn);
        BasicBlock* bad = BasicBlock::Create(ctx, "eh.unwind", curFn);
        b.CreateCondBr(b.CreateICmpNE(loadPendingFlag(), ConstantInt::get(i32(), 0)), bad, ok);
        b.SetInsertPoint(bad);
        emitUnwindCurrent(onThrow);
        b.SetInsertPoint(ok);
    }

    void Emitter::emitThrow(Expr* e) {
        ensureEHSlot();
        Value* obj = eval(e);
        // `throw null;` is rejected by sema, but a null handle can still reach here through a variable.
        BasicBlock* ok = BasicBlock::Create(ctx, "throw.ok", curFn);
        BasicBlock* bad = BasicBlock::Create(ctx, "throw.null", curFn);
        b.CreateCondBr(b.CreateICmpEQ(obj, Constant::getNullValue(ptrTy())), bad, ok);
        b.SetInsertPoint(bad);
        b.CreateCall(libFn("abort", LType::getVoidTy(ctx), {}), {});
        b.CreateUnreachable();
        b.SetInsertPoint(ok);
        // `new ...` / a call result already owns one reference; a borrowed local must be retained.
        if (!ownsReference(e)) emitRetainRef(obj, e->ty);
        storePending(obj);
        emitUnwindCurrent();
        BasicBlock* dead = BasicBlock::Create(ctx, "throw.dead", curFn);
        b.SetInsertPoint(dead);
    }

    // Walk the class descriptor base chain (offset 32) and test whether it reaches `target`.
    Value* Emitter::emitDescMatches(Value* desc, Value* target) {
        auto* res = entryAlloca(i1(), "eh.match");
        b.CreateStore(ConstantInt::get(i1(), 0), res);
        auto* cur = entryAlloca(ptrTy(), "eh.cur");
        b.CreateStore(desc, cur);

        BasicBlock* loop = BasicBlock::Create(ctx, "eh.match.loop", curFn);
        BasicBlock* body = BasicBlock::Create(ctx, "eh.match.body", curFn);
        BasicBlock* found = BasicBlock::Create(ctx, "eh.match.found", curFn);
        BasicBlock* next = BasicBlock::Create(ctx, "eh.match.next", curFn);
        BasicBlock* done = BasicBlock::Create(ctx, "eh.match.done", curFn);
        b.CreateBr(loop);

        b.SetInsertPoint(loop);
        Value* d = b.CreateLoad(ptrTy(), cur);
        b.CreateCondBr(b.CreateICmpEQ(d, Constant::getNullValue(ptrTy())), done, body);

        b.SetInsertPoint(body);
        b.CreateCondBr(b.CreateICmpEQ(d, target), found, next);

        b.SetInsertPoint(found);
        b.CreateStore(ConstantInt::get(i1(), 1), res);
        b.CreateBr(done);

        b.SetInsertPoint(next);
        Value* base = b.CreateLoad(ptrTy(), b.CreateGEP(i8(), d, ConstantInt::get(i64(), 32)));
        b.CreateStore(base, cur);
        b.CreateBr(loop);

        b.SetInsertPoint(done);
        return b.CreateLoad(i1(), res);
    }

    void Emitter::emitAbortIfPending() {
        ensureEHSlot();
        BasicBlock* ok = BasicBlock::Create(ctx, "cleanup.ok", curFn);
        BasicBlock* bad = BasicBlock::Create(ctx, "cleanup.throw", curFn);
        b.CreateCondBr(b.CreateICmpNE(loadPendingFlag(), ConstantInt::get(i32(), 0)), bad, ok);
        b.SetInsertPoint(bad);
        b.CreateCall(libFn("abort", LType::getVoidTy(ctx), {}), {});
        b.CreateUnreachable();
        b.SetInsertPoint(ok);
    }

    // A finally body must not let an exception escape: a local try/catch may handle it, but
    // anything leaving the body aborts (like a destructor).  The body is emitted with the
    // current finally hidden from `finallyStack`, so a throw inside it cannot re-run itself.
    void Emitter::emitFinallyBody(Block* body) {
        if (!body) return;
        // The finally body runs with no exception visible: on the no-match path the original
        // exception is still pending, and a plain call inside finally must not see it.
        Value* savedFlag = loadPendingFlag();
        Value* savedObj = loadPendingObj();
        clearPending();
        abortBases.push_back(ehStack.size());
        block(body);
        abortBases.pop_back();
        if (!hasTerm(b.GetInsertBlock())) emitAbortIfPending();
        storePendingState(savedObj, savedFlag);
    }

    // Emit the top `count` finally bodies, innermost first, then restore the compile-time stack
    // (the normal path and each exit path are emitted separately).
    void Emitter::emitFinallyBodies(size_t count) {
        if (count == 0 || finallyStack.empty()) return;
        if (count > finallyStack.size()) count = finallyStack.size();
        std::vector<FinallyCtx> saved;
        saved.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            saved.push_back(finallyStack.back());
            finallyStack.pop_back();
        }
        for (auto& f : saved) emitFinallyBody(f.body);
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) finallyStack.push_back(*it);
    }

    // `break`/`continue`: run the finally bodies inside the loop being left, innermost first.
    void Emitter::emitFinallyForLoopExit() {
        size_t count = 0;
        for (size_t i = finallyStack.size(); i > 0; --i) {
            if (finallyStack[i - 1].loopDepth >= static_cast<int>(loops.size())) {
                ++count;
            } else {
                break;
            }
        }
        emitFinallyBodies(count);
    }

    void Emitter::emitTry(TryStmt* t) {
        ensureEHSlot();
        BasicBlock* dispatch = BasicBlock::Create(ctx, "try.dispatch", curFn);
        BasicBlock* end = BasicBlock::Create(ctx, "try.end", curFn);
        const size_t depth = refScopes.size();
        const bool hasFinally = t->finally_body != nullptr;

        // The current finally is pushed before the body: a throw in a catch body must run it.
        // `finallyDepth` is recorded after the push, so a throw in the try body does not run
        // the current finally before its own catch has had a chance to match.
        if (hasFinally) {
            finallyStack.push_back({t->finally_body.get(), static_cast<int>(loops.size())});
        }
        ehStack.push_back({dispatch, depth, stmtTemps.size(), finallyStack.size()});
        block(t->body.get());
        ehStack.pop_back();
        if (!hasTerm(b.GetInsertBlock())) {
            if (hasFinally) emitFinallyBodies(1);
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(end);
        }

        // Catch dispatch: the pending object is still owned by the slot.
        b.SetInsertPoint(dispatch);
        Value* obj = loadPendingObj();
        Value* desc = b.CreateLoad(ptrTy(), b.CreateGEP(i8(), obj, ConstantInt::get(i64(), 8)));

        BasicBlock* noMatch = nullptr;
        for (auto& c : t->catches) {
            BasicBlock* catchBB = BasicBlock::Create(ctx, "catch", curFn);
            BasicBlock* nextBB = BasicBlock::Create(ctx, "catch.next", curFn);
            Value* match = emitDescMatches(desc, descOf(c->rty));
            b.CreateCondBr(match, catchBB, nextBB);

            b.SetInsertPoint(catchBB);
            clearPending();
            AllocaInst* slot = entryAlloca(lt(c->rty), "catch.obj", alignOf(c->rty));
            b.CreateStore(obj, slot);
            slots[c->var.get()] = slot;

            // The catch binding is a borrow; a synthetic owner releases the pending reference at scope exit.
            auto owner = std::make_unique<LocalVar>();
            owner->rty = c->rty;
            owner->by_ref = false;
            owner->is_const = true;
            owner->line = c->line;
            owner->col = c->col;
            LocalVar* ownerPtr = owner.get();
            ehLocals.push_back(std::move(owner));
            slots[ownerPtr] = slot;

            pushRefScope();
            refScopes.back().push_back(ownerPtr);
            block(c->body.get());
            // The catch binding stays owned while `finally` runs, matching the exception path
            // (where the pending reference is still held by the slot).
            if (hasFinally) emitFinallyBodies(1);
            popRefScope();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(end);
            b.SetInsertPoint(nextBB);
            noMatch = nextBB;
        }

        b.SetInsertPoint(noMatch ? noMatch : dispatch);
        bool finallyPopped = false;
        if (hasFinally) {
            emitFinallyBodies(1);
            // The no-match path leaves the try: pop the finally before propagating outward,
            // otherwise the outer target would run it a second time.
            finallyStack.pop_back();
            finallyPopped = true;
        }
        emitUnwindCurrent();
        b.SetInsertPoint(end);
        if (hasFinally && !finallyPopped) finallyStack.pop_back();
    }

}  // namespace muc_detail
