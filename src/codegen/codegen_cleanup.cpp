// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Reference cleanup of locals and statement temporaries, and the copy/move helpers (Emitter member
// function definitions).
//
// owns:      the ref-scope stack and local/temporary release, the moved-flag bookkeeping, the
//            copy/move/assign helpers, constructor-progress cleanup, the stack-write verifier and
//            `prepareReturn` (a borrowed return value becomes owned)
// not:       the RC runtime itself and dynamic arrays (codegen_runtime.cpp), expression or
//            statement dispatch
// pass:      emission
// concept:   codegen-cleanup

#include "codegen_impl.hpp"

namespace muc_detail {

    bool Emitter::isRefKind(const TypePtr& t) {
        return t && (t->kind == TypeKind::Class || t->kind == TypeKind::Interface);
    }

    // the object pointer inside a reference value (a class handle or an interface value)
    Value* Emitter::refObj(Value* v, const TypePtr& ty) {
        if (ty && ty->kind == TypeKind::Interface) return b.CreateExtractValue(v, {0});
        // a delegate's RC object is its environment (the second word)
        if (ty && ty->kind == TypeKind::Delegate) return b.CreateExtractValue(v, {1});
        return v;
    }

    void Emitter::emitRetainRef(Value* v, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Interface || ty->kind == TypeKind::Delegate) {
            emitRetain(refObj(v, ty));
        } else {
            emitRetain(v);
        }
    }

    void Emitter::emitReleaseRef(Value* v, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Interface || ty->kind == TypeKind::Delegate) {
            emitRelease(refObj(v, ty));
        } else {
            emitRelease(v);
        }
    }

    // ------------------------------------------------------------ local reference cleanup
    // whether the value's lifetime needs RC cleanup (including recursive struct/array fields)
    bool Emitter::needsCleanup(const TypePtr& t) {
        return t && t->needs_cleanup();
    }

    void Emitter::releaseLocalValue(const LocalVar* lv) {
        auto it = slots.find(lv);
        if (it == slots.end()) return;

        if (isRefKind(lv->rty)) {
            Value* v = b.CreateLoad(lt(lv->rty), it->second);
            emitReleaseRef(v, lv->rty);
        } else if (lv->rty->kind == TypeKind::Struct && lv->rty->has_dtor) {
            FunctionCallee dtor = mod->getOrInsertFunction(
                dtor_cname(lv->rty->unit, lv->rty->name),
                FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
            b.CreateCall(dtor, {it->second});
        } else {
            emitReleaseValue(it->second, lv->rty);
        }
    }

    // Release one local on the way out of the scope(s) that own it. A local that can be `move`d carries a
    // runtime moved flag, so the decision is made on the path actually taken: the branch that moved it
    // skips the destruction, a sibling branch that did not still destroys the value (#21, #23). Only a
    // local that never got a flag (a capture, a synthetic owner) falls back to the compile-time answer.
    void Emitter::releaseLocal(const LocalVar* lv) {
        if (!lv || !needsCleanup(lv->rty) || lv->by_ref) return;
        AllocaInst* flag = movedFlagFor(lv);
        if (!flag) {
            if (!movedLocals.count(lv)) releaseLocalValue(lv);
            return;
        }
        BasicBlock* rel = BasicBlock::Create(ctx, "release.local", curFn);
        BasicBlock* done = BasicBlock::Create(ctx, "release.next", curFn);
        b.CreateCondBr(b.CreateLoad(i1(), flag), done, rel);
        b.SetInsertPoint(rel);
        releaseLocalValue(lv);
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(done);
        b.SetInsertPoint(done);
    }

    // Build `dst` from `src` and leave `src` an empty shell. A type with a move constructor runs it (the
    // constructor owns the transfer rules for whatever it holds); every other movable value is transferred
    // by copying the bytes and zeroing the source, which is exactly right for a handle and harmless for a
    // scalar (agents/value-semantics.md §8.2-§8.3).
    void Emitter::emitMoveInto(Value* dst, const TypePtr& ty, MoveExpr* mv) {
        LocalVar* src = mv->moved;
        Value* from = addrOf(mv->operand.get());
        if (ty->kind == TypeKind::Struct && ty->has_move_ctor) {
            FunctionCallee ctor = mod->getOrInsertFunction(
                move_ctor_cname(ty->unit, ty->name),
                FunctionType::get(LType::getVoidTy(ctx), {ptrTy(), ptrTy()}, false));
            // The destination is zeroed first (SPEC 3.4): the constructor body may assign a reference
            // field, and that assignment releases the previous value before storing the new one.
            if (ty->needs_cleanup()) zeroMem(dst, ty);
            b.CreateCall(ctor, {dst, from});
            emitCheckPending();  // if the move constructor throws, the source still owns its value
            // The constructor defines the source's final state, and the source is a live object: its
            // destructor runs at scope exit like any other. A move constructor that forgets to empty a
            // field therefore double-frees (loudly) instead of leaking silently (SPEC 3.13).
            return;
        }
        storeAt(b.CreateLoad(lt(ty), from), dst, false);
        zeroMem(from, ty);
        noteMovedLocal(src);
    }

    // Build `dst` as a deep copy of the object at `src` through the user's copy constructor.
    void Emitter::emitCopyInto(Value* dst, Value* src, const TypePtr& ty) {
        FunctionCallee ctor = mod->getOrInsertFunction(
            copy_ctor_cname(ty->unit, ty->name),
            FunctionType::get(LType::getVoidTy(ctx), {ptrTy(), ptrTy()}, false));
        // Same as the move constructor: zero the destination so a field assignment inside the body
        // releases null rather than uninitialized memory (SPEC 3.4).
        if (ty->needs_cleanup()) zeroMem(dst, ty);
        b.CreateCall(ctor, {dst, src});
        emitCheckPending();
    }

    // `b = a` for a type with a user destructor: copy-and-swap. The new value is built in a temporary,
    // the temporaries are swapped byte by byte, and the temporary (which now holds the old value) is
    // destroyed. A throwing copy constructor therefore leaves the destination untouched, and a source that
    // aliases the destination is still intact when it is read (agents/value-semantics.md §8.11).
    // Whether an expression is a true prvalue whose storage the assignment may take over by a plain
    // store. Deliberately narrower than `valueOwned`: an operator result may be a borrowed/freshly-
    // retained copy whose lifetime the expression itself manages, so those keep the copy path.
    // A `?:` result does qualify: `evalTernary` materializes one owned value into its result slot
    // (copy constructor for a borrowed arm of a copyable type, field retain otherwise), so taking it
    // over by a plain store leaves nothing behind.
    static bool is_owned_prvalue(Expr* e) {
        if (!e) return false;
        if (dynamic_cast<MoveExpr*>(e)) return true;
        if (auto c = dynamic_cast<Call*>(e)) return !c->by_ref;
        if (dynamic_cast<NewExpr*>(e)) return true;
        if (dynamic_cast<Ternary*>(e)) return true;
        return false;
    }

    void Emitter::emitAssignSwap(Value* dst, Expr* e, const TypePtr& ty) {
        Value* tmp = entryAlloca(lt(ty), "assign.tmp", alignOf(ty));
        if (auto mv = dynamic_cast<MoveExpr*>(e)) {
            emitMoveInto(tmp, ty, mv);
        } else if (is_owned_prvalue(e)) {
            // An owned prvalue (`a = T(args);`, `a = f();`) is built in a slot the expression already
            // owns: moving it into the temporary is a plain store — no copy constructor, so an
            // uncopyable type works, and no extra retain.
            Value* v = coerce(e, ty);
            b.CreateStore(v, tmp);
        } else {
            Value* v = coerce(e, ty);
            Value* src = entryAlloca(lt(ty), "assign.src", alignOf(ty));
            b.CreateStore(v, src);
            emitCopyInto(tmp, src, ty);
        }
        const uint64_t n = sizeOf(ty);
        if (n > 0) {
            Value* scratch = entryAlloca(llvm::ArrayType::get(i8(), n), "assign.swap");
            copyMem(scratch, dst, n);
            copyMem(dst, tmp, n);
            copyMem(tmp, scratch, n);
        }
        // The temporary now owns the destination's previous value.
        emitReleaseValue(tmp, ty);
    }

    // ------------------------------------------------------------ partially constructed objects
    size_t Emitter::findCtorProgress(const TypePtr& ty) const {
        for (size_t i = ctorProgress.size(); i > 0; --i) {
            if (!ctorProgress[i - 1].array && ctorProgress[i - 1].ty == ty) return i - 1;
        }
        return kNoCtorProgress;
    }

    // `first` is the first subobject the sequence owns: a class's inherited fields belong to the level's
    // partial destructor, which releases the (complete) base subobject.
    size_t Emitter::pushCtorProgress(Value* self, const TypePtr& ty, size_t first, bool unaligned) {
        CtorProgress p;
        p.self = self;
        p.ty = ty;
        p.first = first;
        p.next = first;
        p.unaligned = unaligned || ty->is_packed;
        p.array = ty->kind == TypeKind::Array;
        p.released = entryAlloca(i1(), "ctor.rel.done", 1);
        b.CreateStore(ConstantInt::get(i1(), 0), p.released);
        ctorProgress.push_back(p);
        return ctorProgress.size() - 1;
    }

    void Emitter::setCtorProgressNext(size_t entry, size_t next) {
        if (entry >= ctorProgress.size()) return;
        ctorProgress[entry].next = next;
    }

    void Emitter::markCtorProgressBody(size_t entry) {
        if (entry >= ctorProgress.size()) return;
        ctorProgress[entry].inBody = true;
        ctorProgress[entry].next = ctorProgressCount(ctorProgress[entry]);
    }

    Value* Emitter::ctorProgressSlot(const CtorProgress& p, size_t idx) {
        if (p.array) {
            auto* st = cast<LStruct>(lt(p.ty));
            return b.CreateGEP(st, p.self, {ConstantInt::get(i64(), 0),
                                            ConstantInt::get(i32(), 0),
                                            ConstantInt::get(i64(), idx)});
        }
        return fieldAddr(p.self, p.ty, static_cast<int>(idx));
    }

    const TypePtr& Emitter::ctorProgressSub(const CtorProgress& p, size_t idx) {
        return p.array ? p.ty->elem : p.ty->fields[idx].type;
    }

    size_t Emitter::ctorProgressCount(const CtorProgress& p) {
        return p.array ? p.ty->array_size : p.ty->fields.size();
    }

    bool Emitter::ctorProgressWeak(const CtorProgress& p, size_t idx) {
        return !p.array && p.ty->fields[idx].weak;
    }

    void Emitter::emitReleaseFieldZeroed(Value* addr, const TypePtr& ty, bool weak, bool unaligned) {
        if (!ty) return;
        if (weak) {
            b.CreateCall(ensureWeakUnreg(), {addr});
        } else {
            emitReleaseValue(addr, ty, unaligned);
        }
        zeroMem(addr, ty, unaligned);
    }

    // Release everything the interrupted construction sequences have built, innermost first. Subobjects
    // are released in reverse construction order: the ones that finished get their full destructor, the
    // one under construction only its partial destructor (its constructor never returned). Every released
    // subobject is left zeroed, so the enclosing level's partial release - which cannot know how far this
    // constructor got - destroys an empty value.
    void Emitter::emitCtorProgressCleanup() {
        for (size_t k = ctorProgress.size(); k > 0; --k) {
            CtorProgress& p = ctorProgress[k - 1];
            const size_t total = ctorProgressCount(p);
            BasicBlock* skip = BasicBlock::Create(ctx, "ctor.rel.skip", curFn);
            BasicBlock* run = BasicBlock::Create(ctx, "ctor.rel", curFn);
            b.CreateCondBr(b.CreateLoad(i1(), p.released), skip, run);
            b.SetInsertPoint(run);
            // PERF: one guard per object, so an unwind that crosses several checks (the try dispatch
            // re-checks on the no-match path) releases each object exactly once.
            b.CreateStore(ConstantInt::get(i1(), 1), p.released);
            if (!p.inBody && p.next < total) {
                Value* slot = ctorProgressSlot(p, p.next);
                const TypePtr& sub = ctorProgressSub(p, p.next);
                emitReleasePartialValue(slot, sub);
                zeroMem(slot, sub, p.unaligned);
            }
            const size_t done = p.inBody ? total : p.next;
            // PERF: unrolled, like the construction it mirrors; an array's element count is the same
            // compile-time constant, and only elements with a constructor get here at all.
            for (size_t idx = done; idx > p.first; --idx) {
                const TypePtr& sub = ctorProgressSub(p, idx - 1);
                if (!sub) continue;
                emitReleaseFieldZeroed(ctorProgressSlot(p, idx - 1), sub,
                                       ctorProgressWeak(p, idx - 1), p.unaligned);
            }
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(skip);
            b.SetInsertPoint(skip);
        }
    }

    // Run the user constructors an implicit construction has to invoke. Zeroing is the caller's job (it
    // happens whenever the type needs cleanup), so a POD value emits nothing at all and `int[1000] a;`
    // stays as cheap as it was.
    //
    // A sequence of subobject constructions opens a `CtorProgress` entry: an exception leaving it releases
    // the completed subobjects and the one being built (see emitCtorProgressCleanup). A user constructor
    // does that for its own members, so this only covers the inline sequences built here.
    void Emitter::emitCtorGlue(Value* addr, const TypePtr& ty, bool unaligned,
                               const std::function<void()>& onThrow) {
        if (!ty || !ty->needs_ctor()) return;
        if (ty->kind == TypeKind::Struct) {
            if (ty->has_default_ctor()) {
                const std::vector<TypePtr> noArgs;
                FunctionCallee ctor = mod->getOrInsertFunction(
                    ctor_cname(ty->unit, ty->name, noArgs),
                    FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                b.CreateCall(ctor, {addr});
                // PERF: unconditional check - the may-throw summary is per callee and a constructor tree
                // can mix throwing and non-throwing members.
                emitCheckPendingWith(onThrow);
                return;
            }
            const size_t mark = ctorProgress.size();
            const size_t entry = ty->needs_cleanup()
                                     ? pushCtorProgress(addr, ty, 0, unaligned)
                                     : kNoCtorProgress;
            for (size_t i = 0; i < ty->fields.size(); ++i) {
                const Field& f = ty->fields[i];
                if (f.weak || !f.type || !f.type->needs_ctor()) continue;
                setCtorProgressNext(entry, i);
                emitCtorGlue(fieldAddr(addr, ty, static_cast<int>(i)), f.type,
                             unaligned || ty->is_packed, onThrow);
            }
            ctorProgress.resize(mark);
            return;
        }
        if (ty->kind == TypeKind::Array) {
            // PERF: unrolled because the count is a compile-time constant; switch to a loop if arrays of
            // constructor-having elements ever show up with a large size.
            const size_t mark = ctorProgress.size();
            const size_t entry = ty->needs_cleanup()
                                     ? pushCtorProgress(addr, ty, 0, unaligned)
                                     : kNoCtorProgress;
            auto* st = cast<LStruct>(lt(ty));
            for (size_t i = 0; i < ty->array_size; ++i) {
                Value* ep = b.CreateGEP(st, addr, {ConstantInt::get(i64(), 0),
                                                   ConstantInt::get(i32(), 0),
                                                   ConstantInt::get(i64(), i)});
                setCtorProgressNext(entry, i);
                emitCtorGlue(ep, ty->elem, unaligned, onThrow);
            }
            ctorProgress.resize(mark);
        }
    }

    // Record that ownership left this local: `move` zeroes the slot, and the scope must not destroy it.
    // Re-assigning the variable re-arms it (`noteRevivedLocal`).
    void Emitter::noteMovedLocal(const LocalVar* lv) {
        if (!lv) return;
        if (AllocaInst* flag = movedFlagFor(lv)) {
            b.CreateStore(ConstantInt::get(i1(), 1), flag);
            return;
        }
        movedLocals.insert(lv);
        for (auto& scope : refScopes) {
            scope.erase(std::remove(scope.begin(), scope.end(), lv), scope.end());
        }
    }

    void Emitter::noteRevivedLocal(const LocalVar* lv) {
        if (!lv) return;
        if (AllocaInst* flag = movedFlagFor(lv)) {
            b.CreateStore(ConstantInt::get(i1(), 0), flag);
            return;
        }
        if (!movedLocals.count(lv)) return;
        movedLocals.erase(lv);
        if (!needsCleanup(lv->rty) || lv->by_ref || refScopes.empty()) return;
        // Register back into the scope the local was declared in, not into the innermost one: a
        // re-assignment inside a nested block must not shorten the variable's lifetime (#21). A
        // declaration scope that is no longer active would mean the variable outlived its own block, so
        // the outermost still-active scope is used instead and the cleanup is never dropped.
        size_t idx = 0;
        auto it = localDeclScope.find(lv);
        if (it != localDeclScope.end() && it->second < refScopes.size()) idx = it->second;
        refScopes[idx].push_back(lv);
    }

    AllocaInst* Emitter::movedFlagFor(const LocalVar* lv) {
        if (!curFn || !lv) return nullptr;
        auto fn = movedFlags.find(curFn);
        if (fn == movedFlags.end()) return nullptr;
        auto it = fn->second.find(lv);
        return it == fn->second.end() ? nullptr : it->second;
    }

    AllocaInst* Emitter::createMovedFlag(const LocalVar* lv) {
        if (!curFn || !lv) return nullptr;
        AllocaInst*& flag = movedFlags[curFn][lv];
        if (!flag) flag = entryAlloca(i1(), "moved");
        // Cleared at the declaration, so a local declared inside a loop is re-armed every iteration;
        // every release site of that local is dominated by its declaration.
        b.CreateStore(ConstantInt::get(i1(), 0), flag);
        return flag;
    }

    void Emitter::registerLocalCleanup(const LocalVar* lv) {
        if (!lv || !needsCleanup(lv->rty) || lv->by_ref || refScopes.empty()) return;
        createMovedFlag(lv);
        localDeclScope[lv] = refScopes.size() - 1;
        refScopes.back().push_back(lv);
    }

    // Debug aid (MUC_VERIFY_MEM=1): catch writes that run past a stack slot. The LLVM verifier accepts an
    // over-wide store (it is UB, not invalid IR), which is why that class of bug only shows up later as
    // "the optimizer deleted my code". Only an access that provably starts at an alloca is checked, so heap
    // and parameter memory never produces a false positive.
    void Emitter::verifyStackWrites() {
        const llvm::DataLayout& dl = mod->getDataLayout();
        for (llvm::Function& f : *mod) {
            if (f.isDeclaration()) continue;
            std::unordered_map<const llvm::AllocaInst*, uint64_t> size;
            for (llvm::BasicBlock& bb : f) {
                for (llvm::Instruction& i : bb) {
                    auto* ai = llvm::dyn_cast<llvm::AllocaInst>(&i);
                    if (ai && !ai->isArrayAllocation()) {
                        size[ai] = dl.getTypeAllocSize(ai->getAllocatedType());
                    }
                }
            }
            if (size.empty()) continue;

            // Walk a constant GEP / bitcast chain down to the alloca it starts from (null for anything else).
            auto baseOf = [&](const llvm::Value* p, uint64_t& off) -> const llvm::AllocaInst* {
                for (;;) {
                    if (auto* g = llvm::dyn_cast<llvm::GEPOperator>(p)) {
                        llvm::APInt c(dl.getIndexTypeSizeInBits(g->getType()), 0);
                        if (!g->accumulateConstantOffset(dl, c)) return nullptr;
                        if (!c.isNonNegative()) return nullptr;
                        off += c.getZExtValue();
                        p = g->getPointerOperand();
                        continue;
                    }
                    if (auto* bc = llvm::dyn_cast<llvm::BitCastOperator>(p)) {
                        p = bc->getOperand(0);
                        continue;
                    }
                    return llvm::dyn_cast<llvm::AllocaInst>(p);
                }
            };

            for (llvm::BasicBlock& bb : f) {
                for (llvm::Instruction& i : bb) {
                    const llvm::Value* dst = nullptr;
                    uint64_t len = 0;
                    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&i)) {
                        dst = st->getPointerOperand();
                        len = dl.getTypeStoreSize(st->getValueOperand()->getType());
                    } else if (auto* ms = llvm::dyn_cast<llvm::MemSetInst>(&i)) {
                        dst = ms->getDest();
                        auto* c = llvm::dyn_cast<llvm::ConstantInt>(ms->getLength());
                        if (!c) continue;
                        len = c->getZExtValue();
                    } else if (auto* mc = llvm::dyn_cast<llvm::MemCpyInst>(&i)) {
                        dst = mc->getDest();
                        auto* c = llvm::dyn_cast<llvm::ConstantInt>(mc->getLength());
                        if (!c) continue;
                        len = c->getZExtValue();
                    } else {
                        continue;
                    }
                    uint64_t off = 0;
                    const llvm::AllocaInst* ai = baseOf(dst, off);
                    if (!ai) continue;
                    auto it = size.find(ai);
                    if (it == size.end()) continue;
                    if (off + len > it->second) {
                        std::string what;
                        llvm::raw_string_ostream os(what);
                        os << "stack write out of bounds in `" << f.getName().str() << "`: " << (off + len)
                           << " bytes at offset " << off << " into a " << it->second << "-byte slot";
                        throw MuError(os.str());
                    }
                }
            }
        }
    }

    void Emitter::pushRefScope() { refScopes.emplace_back(); }

    void Emitter::popRefScope() {
        if (refScopes.empty()) return;
        auto scope = refScopes.back();
        if (!hasTerm(b.GetInsertBlock())) {
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
        refScopes.pop_back();
    }

    // release the references of inner scopes before leaving the current block/loop
    void Emitter::releaseScopesDeeperThan(size_t depth) {
        if (depth >= refScopes.size()) return;
        for (size_t i = refScopes.size(); i > depth; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // owned dynamic array temporaries materialized inside a statement: released at the end of the statement
    void Emitter::trackOwnedSliceTemp(Expr* e, Value* v, const TypePtr& ty) {
        if (!ty || ty->kind != TypeKind::Slice || !valueOwned(e)) return;
        auto* tmp = entryAlloca(lt(ty), "owned.tmp", alignOf(ty));
        b.CreateStore(v, tmp);
        stmtTemps.emplace_back(tmp, ty);
    }

    // owned Str rvalue: materialized into a statement temporary, with the owner released at the end of the statement
    void Emitter::releaseAllScopes() {
        for (size_t i = refScopes.size(); i > 0; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // Release the owned rvalues materialized during statement execution (those registered after mark), keeping reverse order
    void Emitter::destroyTempsFrom(size_t mark) {
        if (stmtTemps.size() <= mark) return;
        emitReleaseTempsFrom(mark);
        stmtTemps.resize(mark);
    }

    // whether the expression already holds a transferable strong reference (new / a call returning a class reference)
    bool Emitter::ownsReference(Expr* e) {
        if (!e || !isRefKind(e->ty)) return false;
        return valueOwned(e);
    }

    // whether the expression result already holds a transferable strong reference (not limited to class handles; also used for reference arrays)
    bool Emitter::valueOwned(Expr* e) {
        if (!e) return false;
        if (dynamic_cast<NewExpr*>(e)) return true;
        // `move x` hands over the value: the source is zeroed and no longer owns it
        if (dynamic_cast<MoveExpr*>(e)) return true;
        // A call that returns a borrow (`ref T f()`) hands over an address, not an owned value: the
        // callee's object still owns it (the same distinction `Binary` makes below).
        if (auto c = dynamic_cast<Call*>(e)) return !c->by_ref;
        // a capturing lambda materializes a fresh environment (rc = 1), so its value is owned; a
        // non-capturing lambda is a bare function address and carries no ownership either way
        if (dynamic_cast<Lambda*>(e)) return true;
        if (auto b = dynamic_cast<Binary*>(e); b && b->op_func && !b->by_ref) return true;
        if (dynamic_cast<Ternary*>(e)) return true;  // the result slot is already materialized with owned semantics
        if (auto u = dynamic_cast<Unary*>(e); u && u->op_func) return true;
        if (auto c = dynamic_cast<Cast*>(e)) return valueOwned(c->expr.get());
        return false;
    }

    // whether the expression result already holds a transferable ownership under the target type.
    // A fixed-size array -> dynamic array conversion allocates a new array in coerce, so it counts as owned too.
    bool Emitter::convOwns(Expr* e, const TypePtr& dst) {
        if (dst && dst->kind == TypeKind::Slice && e && e->ty &&
            e->ty->kind == TypeKind::Array) {
            return true;
        }
        return valueOwned(e);
    }

    // Turn a borrow into a new strong reference when returning a class reference
    Value* Emitter::prepareReturn(Expr* e) {
        // `ref T f()`: the element address (a borrow) is returned, neither copied nor retained
        if (is_borrow_type(curRetTy)) return addrOf(e);
        Value* v = coerce(e, curRetTy);
        if (isRefKind(curRetTy)) {
            if (!ownsReference(e)) emitRetainRef(v, curRetTy);
            return v;
        }
        if (curRetTy && curRetTy->kind == TypeKind::Array && needsCleanup(curRetTy)) {
            if (valueOwned(e)) return v;
            auto* tmp = entryAlloca(lt(curRetTy), "ret.arr", alignOf(curRetTy));
            b.CreateStore(v, tmp);
            emitRetainValue(tmp, curRetTy);
            return b.CreateLoad(lt(curRetTy), tmp);
        }
        if (curRetTy && curRetTy->kind == TypeKind::Struct && needsCleanup(curRetTy)) {
            // the return value transfers as owned: a borrowed value is retained field by field first, and the local is released later in releaseAllScopes
            if (valueOwned(e)) return v;
            auto* tmp = entryAlloca(lt(curRetTy), "ret.struct", alignOf(curRetTy));
            b.CreateStore(v, tmp);
            emitRetainValue(tmp, curRetTy);
            return b.CreateLoad(lt(curRetTy), tmp);
        }
        if (curRetTy && curRetTy->kind == TypeKind::Slice) {
            if (convOwns(e, curRetTy)) return v;
            auto* tmp = entryAlloca(lt(curRetTy), "ret.arr", alignOf(curRetTy));
            b.CreateStore(v, tmp);
            emitRetainValue(tmp, curRetTy);
            return b.CreateLoad(lt(curRetTy), tmp);
        }
        return v;
    }

}  // namespace muc_detail
