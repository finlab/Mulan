// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains statement code generation (block/statement dispatch, for/match/try) (Emitter member function definitions).
//
// owns:      statement emission (block/if/while/for/match/try) and the statement-level temporary
//            and ref scopes
// not:       `para` / `para for` (codegen_para.cpp), the load/store and initialization primitives
//            (codegen_mem.cpp), expression evaluation, call lowering, class metadata
// pass:      emission, after every type and body check
// concept:   codegen-statements

#include "codegen_impl.hpp"

namespace muc_detail {

using ArgPack = Emitter::ArgPack;

    // ------------------------------------------------------------ statements

    void Emitter::block(Block* blk) {
        if (!blk) return;
        pushRefScope();
        for (auto& x : blk->stmts) stmt(x.get());
        popRefScope();
    }

    void Emitter::stmt(Stmt* s) {
        size_t mark = stmtTemps.size();
        stmtImpl(s);
        destroyTempsFrom(mark);
    }

    void Emitter::stmtImpl(Stmt* s) {
        if (diSub && s->line > 0) {
            b.SetCurrentDebugLocation(DILocation::get(ctx, static_cast<unsigned>(s->line),
                                                      static_cast<unsigned>(s->col), diSub));
        }
        if (std::getenv("MUC_LLVM_TRACE")) {
            std::cerr << "[llvm]   stmt " << typeid(*s).name();
            if (auto lv = dynamic_cast<LocalVar*>(s)) std::cerr << " " << lv->name;
            std::cerr << "\n";
        }
        if (auto th = dynamic_cast<ThrowStmt*>(s)) {
            emitThrow(th->value.get());
            return;
        }
        if (auto pp = dynamic_cast<ParaStmt*>(s)) {
            emitPara(pp);
            return;
        }
        if (auto pf = dynamic_cast<ParaForStmt*>(s)) {
            emitParaFor(pf);
            return;
        }
        if (auto m = dynamic_cast<MatchStmt*>(s)) {
            emitMatchStmt(m);
            return;
        }
        if (auto tr = dynamic_cast<TryStmt*>(s)) {
            emitTry(tr);
            return;
        }
        if (auto b0 = dynamic_cast<Block*>(s)) {
            block(b0);
            return;
        }
        if (auto lv = dynamic_cast<LocalVar*>(s)) {
            Value* slot = slotFor(lv);
            if (lv->init) {
                initInto(slot, lv->rty, lv->init.get());
                // The local is not in refScopes yet. If initialization threw (a struct
                // constructor, or a nested constructor inside an aggregate), release the
                // zero-initialized/partially constructed local before propagating.
                if (needsCleanup(lv->rty) && !lv->by_ref) {
                    emitCheckPendingWith([&] { emitReleasePartialValue(slot, lv->rty); });
                }
            } else if (isRefKind(lv->rty)) {
                b.CreateStore(Constant::getNullValue(lt(lv->rty)), slot);
            } else if (lv->rty && !is_void(lv->rty)) {
                // Zero first whatever may hold an RC handle or a user destructor: releasing a zero value
                // is defined to be a no-op, which is what makes every failure path safe
                // (agents/value-semantics.md §6.3). POD values stay uninitialized, as in C++ — except a
                // variant: its tag is never a value to guess, and the zero value is the first
                // alternative (`None` for `Option<T>`).
                const bool zeroed =
                    lv->rty->needs_cleanup() || (lv->rty->kind == TypeKind::Sum);
                if (zeroed) zeroMem(slot, lv->rty);
                if (lv->rty->needs_ctor()) {
                    emitCtorGlue(slot, lv->rty);
                    emitCheckPendingWith([&] {
                        if (zeroed) emitReleasePartialValue(slot, lv->rty);
                    });
                }
            }
            if (needsCleanup(lv->rty) && !lv->by_ref) registerLocalCleanup(lv);
            return;
        }
        if (auto es = dynamic_cast<ExprStmt*>(s)) {
            Value* v = eval(es->expr.get());
            TypePtr ty = es->expr->ty;
            if (valueOwned(es->expr.get())) {
                if (isRefKind(ty)) {
                    emitReleaseRef(v, ty);
                } else if (ty && ty->kind == TypeKind::Array && needsCleanup(ty)) {
                    auto* tmp = entryAlloca(lt(ty), "expr.tmp", alignOf(ty));
                    b.CreateStore(v, tmp);
                    emitReleaseValue(tmp, ty);
                } else if (ty && needsCleanup(ty) &&
                           (ty->kind == TypeKind::Struct || ty->kind == TypeKind::Slice ||
                            ty->kind == TypeKind::Sum || ty->kind == TypeKind::Delegate)) {
                    auto* tmp = entryAlloca(lt(ty), "expr.tmp", alignOf(ty));
                    b.CreateStore(v, tmp);
                    emitReleaseValue(tmp, ty);
                }
            }
            return;
        }
        if (auto as = dynamic_cast<AssignStmt*>(s)) {
            if (as->set_method) {
                auto* ix = dynamic_cast<Index*>(as->target.get());
                emitMemberIndexSet(as->set_method, addrOf(ix->base.get()),
                                   convertTo(eval(ix->indices[0].get()), ix->indices[0]->ty,
                                             Type::make_prim("isize")),
                                   as->value.get());
                return;
            }
            if (as->set_func) {
                auto* ix = dynamic_cast<Index*>(as->target.get());
                emitUserCall(as->set_func,
                             {ix->base.get(), ix->indices[0].get(), as->value.get()});
                return;
            }
            Value* addr = addrOf(as->target.get());
            // Assigning to a moved variable re-arms it for destruction when the scope ends.
            if (as->op == "=") {
                if (auto n = dynamic_cast<Name*>(as->target.get()); n && n->kind == "local") {
                    if (auto* lvp = std::get_if<LocalVar*>(&n->sym)) noteRevivedLocal(*lvp);
                }
            }
            const bool una = unalignedExpr(as->target.get());
            // A type that needs copy/move glue assigns through copy-and-swap, before the value is coerced
            // (coercing would already consume a `move` RHS) — agents/value-semantics.md §8.11. `has_dtor`
            // alone is not enough: a host whose *field* declares the destructor must use its own copy
            // constructor as well, otherwise the generic path byte-copies the field (two owners).
            if (as->op == "=" && as->target->ty && as->target->ty->kind == TypeKind::Struct &&
                (as->target->ty->needs_ctor_glue() || as->target->ty->has_copy_ctor ||
                 as->target->ty->has_move_ctor)) {
                // A user copy/move constructor defines what assignment means even when no nested
                // destructor forced the glue (`needs_ctor_glue` only tracks destructors). A plain
                // store would bypass the constructor (and any exception it throws) (#241).
                emitAssignSwap(addr, as->value.get(), as->target->ty);
                return;
            }
            if (as->op == "=") {
                Value* nv = coerce(as->value.get(), as->target->ty);
                bool weakTarget = false;
                if (auto m = dynamic_cast<Member*>(as->target.get())) {
                    TypePtr ot = m->obj->ty;
                    if (ot && ot->kind == TypeKind::Class && m->field_index >= 0 &&
                        m->field_index < static_cast<int>(ot->fields.size())) {
                        weakTarget = ot->fields[m->field_index].weak;
                    }
                } else if (auto n = dynamic_cast<Name*>(as->target.get())) {
                    if (n->kind == "field" && n->implicit_this && curClassTy &&
                        n->field_index >= 0 &&
                        n->field_index < static_cast<int>(curClassTy->fields.size())) {
                        weakTarget = curClassTy->fields[n->field_index].weak;
                    }
                }
                if (weakTarget) {
                    emitWeakStore(addr, nv);
                    return;
                }
                if (isRefKind(as->target->ty)) {
                    Value* old = loadAt(lt(as->target->ty), addr, una);
                    // `b = a` when both already hold the same pointer is a no-op for the count: the
                    // retain and the release cancel out. Skipping the pair keeps a loop that re-assigns
                    // the same value from paying two atomics per iteration (the fast path COW strings
                    // and shared_ptr implementations take).
                    BasicBlock* sameBB = BasicBlock::Create(ctx, "assign.same", curFn);
                    BasicBlock* diffBB = BasicBlock::Create(ctx, "assign.diff", curFn);
                    BasicBlock* doneBB = BasicBlock::Create(ctx, "assign.done", curFn);
                    // An interface value is a `{obj, desc}` aggregate: the object pointer decides
                    // identity (`pnv`/`pold`), and the copy path treats the pair as one handle.
                    Value* cmpOld = old;
                    Value* cmpNew = nv;
                    if (as->target->ty->kind == TypeKind::Interface) {
                        cmpOld = b.CreateExtractValue(old, {0});
                        cmpNew = b.CreateExtractValue(nv, {0});
                    }
                    b.CreateCondBr(b.CreateICmpEQ(cmpOld, cmpNew), sameBB, diffBB);
                    b.SetInsertPoint(diffBB);
                    if (!ownsReference(as->value.get())) emitRetainRef(nv, as->target->ty);
                    emitReleaseRef(old, as->target->ty);
                    storeAt(nv, addr, una);
                    b.CreateBr(doneBB);
                    b.SetInsertPoint(sameBB);
                    b.CreateBr(doneBB);
                    b.SetInsertPoint(doneBB);
                    return;
                }
                TypePtr tty = as->target->ty;
                if (!weakTarget && tty && needsCleanup(tty) &&
                    (tty->kind == TypeKind::Array || tty->kind == TypeKind::Struct ||
                     tty->kind == TypeKind::Slice || tty->kind == TypeKind::Delegate ||
                     tty->kind == TypeKind::Sum)) {
                    // `Sum` belongs here too: a sum value owns its active payload, so the assignment
                    // must release the old payload and retain the new one (SPEC decision #55). The
                    // retain happens on the copied temporary first, so `h = h` cannot free the value
                    // it is about to store.
                    auto* tmp = entryAlloca(lt(tty), "val.tmp", alignOf(tty));
                    b.CreateStore(nv, tmp);
                    if (!convOwns(as->value.get(), tty)) emitRetainValue(tmp, tty);
                    emitReleaseValue(addr, tty, una);
                    storeAt(b.CreateLoad(lt(tty), tmp), addr, una);
                    return;
                }
                storeAt(nv, addr, una);
                return;
            }
            Value* v = eval(as->value.get());
            std::string op = as->op.substr(0, as->op.size() - 1);
            Value* old = loadAt(lt(as->target->ty), addr, una);
            TypePtr rt = as->target->ty;
            if (rt->kind == TypeKind::Ptr) {
                Value* n = convertTo(v, as->value->ty, Type::make_prim("isize"));
                Value* np = gep(lt(rt->elem), old, op == "+" ? n : b.CreateNeg(n));
                storeAt(np, addr, una);
                return;
            }
            Value* rv = convertTo(v, as->value->ty, rt);
            bool fl = rt->kind == TypeKind::Prim && rt->is_float();
            bool sg = rt->kind == TypeKind::Prim && rt->is_signed();
            Value* res = nullptr;
            if (op == "+") res = fl ? b.CreateFAdd(old, rv) : b.CreateAdd(old, rv);
            else if (op == "-") res = fl ? b.CreateFSub(old, rv) : b.CreateSub(old, rv);
            else if (op == "*") res = fl ? b.CreateFMul(old, rv) : b.CreateMul(old, rv);
            else if (op == "/") {
                if (!fl) emitDivCheck(old, rv, sg);
                res = fl ? b.CreateFDiv(old, rv) : (sg ? b.CreateSDiv(old, rv) : b.CreateUDiv(old, rv));
            }
            else if (op == "%") {
                if (!fl) emitDivCheck(old, rv, sg);
                res = fl ? b.CreateFRem(old, rv) : (sg ? b.CreateSRem(old, rv) : b.CreateURem(old, rv));
            }
            else if (op == "&") res = b.CreateAnd(old, rv);
            else if (op == "|") res = b.CreateOr(old, rv);
            else if (op == "^") res = b.CreateXor(old, rv);
            else if (op == "<<" || op == ">>") {
                if (boundsCheck && rv->getType()->isIntegerTy()) {
                    emitShiftCountCheck(rv, static_cast<unsigned>(sizeOf(rt) * 8));
                }
                res = op == "<<" ? b.CreateShl(old, rv)
                                 : (sg ? b.CreateAShr(old, rv) : b.CreateLShr(old, rv));
            }
            storeAt(res, addr, una);
            return;
        }
        if (auto is = dynamic_cast<IfStmt*>(s)) {
            Value* cond = eval(is->cond.get());
            BasicBlock* thenBB = BasicBlock::Create(ctx, "if.then", curFn);
            BasicBlock* elseBB = is->els ? BasicBlock::Create(ctx, "if.else", curFn)
                                         : BasicBlock::Create(ctx, "if.end", curFn);
            BasicBlock* endBB = is->els ? BasicBlock::Create(ctx, "if.end", curFn) : elseBB;
            b.CreateCondBr(cond, thenBB, elseBB);
            b.SetInsertPoint(thenBB);
            block(is->then.get());
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
            if (is->els) {
                b.SetInsertPoint(elseBB);
                stmt(is->els.get());
                if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
            }
            b.SetInsertPoint(endBB);
            return;
        }
        if (auto ws = dynamic_cast<WhileStmt*>(s)) {
            BasicBlock* condBB = BasicBlock::Create(ctx, "while.cond", curFn);
            BasicBlock* bodyBB2 = BasicBlock::Create(ctx, "while.body", curFn);
            BasicBlock* endBB = BasicBlock::Create(ctx, "while.end", curFn);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            size_t cmark = stmtTemps.size();
            Value* cond = eval(ws->cond.get());
            destroyTempsFrom(cmark);  // struct rvalues in the condition are released each iteration
            b.CreateCondBr(cond, bodyBB2, endBB);
            b.SetInsertPoint(bodyBB2);
            loops.push_back({condBB, endBB});
            loopScopeDepth.push_back(refScopes.size());
            block(ws->body.get());
            loopScopeDepth.pop_back();
            loops.pop_back();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            return;
        }
        if (auto cs = dynamic_cast<ForStmt*>(s)) {
            emitForC(cs);
            return;
        }
        if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            emitFor(fs);
            return;
        }
        if (dynamic_cast<BreakStmt*>(s)) {
            if (loops.empty()) fail("LLVM backend: `break` outside of a loop");
            emitFinallyForLoopExit();
            // Release the iteration's temps but keep the compile-time table for the normal path
            // (#223/#225).
            releaseTempsOnExit(0);
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().second);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (dynamic_cast<ContinueStmt*>(s)) {
            if (loops.empty()) fail("LLVM backend: `continue` outside of a loop");
            emitFinallyForLoopExit();
            // Release the iteration's temps but keep the compile-time table for the normal path
            // (#223/#225).
            releaseTempsOnExit(0);
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().first);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            if (!rs->value) {
                emitFinallyBodies(finallyStack.size());
                releaseTempsOnExit(0);
                releaseAllScopes();
                b.CreateRetVoid();
            } else {
                Value* rv = prepareReturn(rs->value.get());
                emitFinallyBodies(finallyStack.size());
                releaseTempsOnExit(0);
                releaseAllScopes();
                // `sret`: the result travels through the hidden pointer and the function returns void.
                // Otherwise a `Coerce*` result is packed into the register the C ABI asks for.
                if (curSretSlot) {
                    b.CreateStore(rv, curSretSlot);
                    b.CreateRetVoid();
                } else if (curCoerceRet) {
                    b.CreateRet(coerceToReg(rv, curRetTy, curCoerceRet));
                } else {
                    b.CreateRet(rv);
                }
            }
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        fail("LLVM backend: unknown statement");
    }


    void Emitter::emitForC(ForStmt* s) {
        // Lowered to `init; while (cond) { body; step; }` with `continue` branching to the step block
        // (C semantics), not back to the condition.
        pushRefScope();
        for (auto& st : s->init) stmt(st.get());
        BasicBlock* condBB = BasicBlock::Create(ctx, "for.cond", curFn);
        BasicBlock* bodyBB = BasicBlock::Create(ctx, "for.body", curFn);
        BasicBlock* stepBB = BasicBlock::Create(ctx, "for.step", curFn);
        BasicBlock* endBB = BasicBlock::Create(ctx, "for.end", curFn);
        b.CreateBr(condBB);
        b.SetInsertPoint(condBB);
        if (s->cond) {
            size_t cmark = stmtTemps.size();
            Value* cond = eval(s->cond.get());
            destroyTempsFrom(cmark);  // struct rvalues in the condition are released each iteration
            b.CreateCondBr(cond, bodyBB, endBB);
        } else {
            b.CreateBr(bodyBB);  // `for (;;)`: the only exits are `break` and `return`
        }
        b.SetInsertPoint(bodyBB);
        loops.push_back({stepBB, endBB});
        loopScopeDepth.push_back(refScopes.size());
        block(s->body.get());
        loopScopeDepth.pop_back();
        loops.pop_back();
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(stepBB);
        b.SetInsertPoint(stepBB);
        if (s->step) stmt(s->step.get());
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(condBB);
        b.SetInsertPoint(endBB);
        popRefScope();
    }

    void Emitter::emitFor(ForInStmt* s) {
        BasicBlock* condBB = BasicBlock::Create(ctx, "for.cond", curFn);
        BasicBlock* bodyBB2 = BasicBlock::Create(ctx, "for.body", curFn);
        BasicBlock* incBB = BasicBlock::Create(ctx, "for.inc", curFn);
        BasicBlock* endBB = BasicBlock::Create(ctx, "for.end", curFn);

        Value* varSlot = slotFor(s->lvar.get());
        Value* idxSlot = (s->has_idx && s->idx_lvar) ? slotFor(s->idx_lvar.get()) : nullptr;

        if (s->loop_kind == "range") {
            auto* re = dynamic_cast<RangeExpr*>(s->seq.get());
            TypePtr vt = s->var_ty;
            Value* lo = convertTo(eval(re->lo.get()), re->lo->ty, vt);
            Value* hiV = convertTo(eval(re->hi.get()), re->hi->ty, vt);
            const bool down = s->step_val < 0;
            Value* stepV = ConstantInt::get(lt(vt), static_cast<uint64_t>(s->step_val));
            // The iteration counter lives in a hidden slot: the loop variable is a per-iteration copy,
            // so writing it in the body cannot skip or repeat elements (`for i in 0..5 { i = 4; }`
            // still visits all five values), matching the value-binding rule of sequence for-in.
            Value* cntSlot = entryAlloca(lt(vt), "for.range.counter", alignOf(vt));
            b.CreateStore(lo, cntSlot);
            if (idxSlot) b.CreateStore(ConstantInt::get(cast<IntegerType>(lt(Type::make_prim("isize"))), 0), idxSlot);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            Value* cur = b.CreateLoad(lt(vt), cntSlot);
            Value* cmp = nullptr;
            // An unsigned range compares unsigned; the signed forms made `0u..3000000000u` look empty
            // (#205).
            const bool uns = vt && vt->kind == TypeKind::Prim && vt->is_integer() && !vt->is_signed();
            if (!down) {
                if (uns) cmp = re->inclusive ? b.CreateICmpULE(cur, hiV) : b.CreateICmpULT(cur, hiV);
                else cmp = re->inclusive ? b.CreateICmpSLE(cur, hiV) : b.CreateICmpSLT(cur, hiV);
            } else {
                if (uns) cmp = re->inclusive ? b.CreateICmpUGE(cur, hiV) : b.CreateICmpUGT(cur, hiV);
                else cmp = re->inclusive ? b.CreateICmpSGE(cur, hiV) : b.CreateICmpSGT(cur, hiV);
            }
            b.CreateCondBr(cmp, bodyBB2, endBB);
            b.SetInsertPoint(bodyBB2);
            b.CreateStore(b.CreateLoad(lt(vt), cntSlot), varSlot);
            loops.push_back({incBB, endBB});
            loopScopeDepth.push_back(refScopes.size());
            if (paraSpawn) emitParaForSpawnItem(varSlot); else block(s->body.get());
            loopScopeDepth.pop_back();
            loops.pop_back();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
            b.SetInsertPoint(incBB);
            Value* c2 = b.CreateLoad(lt(vt), cntSlot);
            b.CreateStore(b.CreateAdd(c2, stepV), cntSlot);
            if (idxSlot) {
                TypePtr us = Type::make_prim("isize");
                Value* iv = b.CreateLoad(lt(us), idxSlot);
                b.CreateStore(b.CreateAdd(iv, ConstantInt::get(cast<IntegerType>(lt(us)), 1)), idxSlot);
            }
            b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            return;
        }

        TypePtr sty = s->seq_ty;
        Value* seqAddr = nullptr;
        if (auto n = dynamic_cast<Name*>(s->seq.get())) {
            (void)n;
        }
        // a simple lvalue has its address taken directly; otherwise materialize it first (evaluated only once)
        bool simple = false;
        {
            Expr* e = s->seq.get();
            while (auto m = dynamic_cast<Member*>(e)) e = m->obj.get();
            simple = dynamic_cast<Name*>(e) != nullptr;
        }
        bool seqUnaligned = false;
        if (simple) {
            seqAddr = addrOf(s->seq.get());
            seqUnaligned = unalignedExpr(s->seq.get());
        } else {
            Value* v = eval(s->seq.get());
            seqAddr = entryAlloca(lt(sty), "seq.tmp", alignOf(sty));
            b.CreateStore(v, seqAddr);
            // owned dynamic array / array temporaries: released after the loop ends
            if (needsCleanup(sty) && valueOwned(s->seq.get()))
                stmtTemps.emplace_back(seqAddr, sty);
        }

        // cursor form (SPEC 6.4.2): auto it = seq.iterator(); while it.next() { v = it.current(); ... }
        if (s->loop_kind == "cursor") {
            Value* recv = (sty->kind == TypeKind::Class) ? b.CreateLoad(lt(sty), seqAddr) : seqAddr;
            Value* itv = emitMethodCallVals(s->iter_sig, recv, sty, {}, s);
            if (mayThrow.mayThrowMethod(s->iter_sig, sty)) emitCheckPending();
            auto* itSlot = entryAlloca(lt(s->it_ty), "it", alignOf(s->it_ty));
            b.CreateStore(itv, itSlot);
            // The iterator is an owned value: a class/interface handle, or a struct cursor with RC
            // fields, is released when the statement ends (#221).
            if (isRefKind(s->it_ty) ||
                (s->it_ty->kind == TypeKind::Struct && needsCleanup(s->it_ty))) {
                stmtTemps.emplace_back(itSlot, s->it_ty);
            }
            // struct cursors are passed by address (static dispatch); a class/interface cursor value is
            // passed as the receiver value itself.
            const bool itByValue =
                s->it_ty->kind == TypeKind::Interface || s->it_ty->kind == TypeKind::Class;
            auto cursorSelf = [&]() -> Value* {
                return itByValue ? static_cast<Value*>(b.CreateLoad(lt(s->it_ty), itSlot)) : static_cast<Value*>(itSlot);
            };

            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            Value* more = emitMethodCallVals(s->next_sig, cursorSelf(), s->it_ty, {}, s);
            if (mayThrow.mayThrowMethod(s->next_sig, s->it_ty)) emitCheckPending();
            b.CreateCondBr(more, bodyBB2, endBB);
            b.SetInsertPoint(bodyBB2);
            Value* ev = emitMethodCallVals(s->cur_sig, cursorSelf(), s->it_ty, {}, s);
            if (mayThrow.mayThrowMethod(s->cur_sig, s->it_ty)) emitCheckPending();
            b.CreateStore(ev, varSlot);
            const bool ownCur = needsCleanup(s->elem_ty);
            // `current()` returns by value (sema rejects a borrow return): the returned value already
            // owns its reference, so the loop variable takes it over; only the cleanup below is needed
            // (#214). Retaining again leaked one reference per iteration.

            const size_t curDepth = refScopes.size();
            loops.push_back({incBB, endBB});
            pushRefScope();
            if (ownCur) registerLocalCleanup(s->lvar.get());
            loopScopeDepth.push_back(curDepth);
            if (paraSpawn) emitParaForSpawnItem(varSlot); else block(s->body.get());
            loopScopeDepth.pop_back();
            loops.pop_back();
            popRefScope();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
            b.SetInsertPoint(incBB);
            b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            return;
        }

        // index form (SPEC 6.4.1): length() once + operator[](C, isize) each iteration
        if (s->loop_kind == "index") {
            TypePtr us = Type::make_prim("isize");
            Value* recv =
                (sty->kind == TypeKind::Class) ? b.CreateLoad(lt(sty), seqAddr) : seqAddr;
            Value* n = emitMethodCallVals(s->len_sig, recv, sty, {}, s);
            if (mayThrow.mayThrowMethod(s->len_sig, sty)) emitCheckPending();
            n = convertTo(n, s->len_sig->ret, us);

            auto* idx = entryAlloca(i64(), "for.idx");
            b.CreateStore(ConstantInt::get(i64(), 0), idx);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            Value* ci = b.CreateLoad(i64(), idx);
            b.CreateCondBr(b.CreateICmpULT(ci, n), bodyBB2, endBB);
            b.SetInsertPoint(bodyBB2);
            Value* ci2 = b.CreateLoad(i64(), idx);
            if (idxSlot) b.CreateStore(ci2, idxSlot);
            Value* ev = emitMemberIndex(s->idx_method, seqAddr, {ci2});
            if (is_borrow_type(s->idx_method->ret)) {
                // borrowed element: a ref binding stores the pointer; a value binding reads it out on the spot
                if (s->by_ref) b.CreateStore(ev, varSlot);
                else b.CreateStore(loadAt(lt(s->elem_ty), ev, false), varSlot);
            } else {
                b.CreateStore(ev, varSlot);
            }
            // the call result is an owned value: released when the iteration ends (normally / break / continue)
            const bool ownVar = !s->by_ref && needsCleanup(s->elem_ty);
            const size_t outerDepth = refScopes.size();
            loops.push_back({incBB, endBB});
            pushRefScope();
            if (ownVar) registerLocalCleanup(s->lvar.get());
            loopScopeDepth.push_back(outerDepth);
            if (paraSpawn) emitParaForSpawnItem(varSlot); else block(s->body.get());
            loopScopeDepth.pop_back();
            loops.pop_back();
            popRefScope();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
            b.SetInsertPoint(incBB);
            Value* ci3 = b.CreateLoad(i64(), idx);
            b.CreateStore(b.CreateAdd(ci3, ConstantInt::get(i64(), 1)), idx);
            b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            return;
        }

        // length and element address
        Value* len = nullptr;
        Value* elemPtr = nullptr;
        if (sty->kind == TypeKind::Array) {
            len = ConstantInt::get(i64(), sty->array_size);
            elemPtr = gep(cast<LStruct>(lt(sty)), seqAddr,
                                  {ConstantInt::get(i64(), 0), ConstantInt::get(i32(), 0),
                                   ConstantInt::get(i64(), 0)});
        } else if (sty->kind == TypeKind::Slice) {
            Value* h = b.CreateLoad(lt(sty), seqAddr);
            len = emitSliceTotal(h, sty->rank);
            elemPtr = sliceDataPtr(h, sty->rank);
        } else {
            Value* sv = b.CreateLoad(lt(sty), seqAddr);
            Value* data = b.CreateExtractValue(sv, {0});
            len = b.CreateExtractValue(sv, {1});
            elemPtr = data;  // string: byte view
        }

        auto* idx = entryAlloca(i64(), "for.idx");
        b.CreateStore(ConstantInt::get(i64(), 0), idx);
        b.CreateBr(condBB);
        b.SetInsertPoint(condBB);
        Value* ci = b.CreateLoad(i64(), idx);
        b.CreateCondBr(b.CreateICmpULT(ci, len), bodyBB2, endBB);
        b.SetInsertPoint(bodyBB2);
        Value* ci2 = b.CreateLoad(i64(), idx);
        if (idxSlot) b.CreateStore(ci2, idxSlot);
        TypePtr et = s->elem_ty;
        Value* ep = gep(lt(et), elemPtr, ci2);
        if (s->by_ref) {
            b.CreateStore(ep, varSlot);
        } else {
            b.CreateStore(loadAt(lt(et), ep, seqUnaligned), varSlot);
        }
        // the value binding is an owning local variable (SPEC §4): retained each iteration, released when leaving the iteration (normally / break / continue)
        const bool ownVar = !s->by_ref && needsCleanup(et);
        if (ownVar) {
            if (isRefKind(et)) {
                emitRetainRef(b.CreateLoad(lt(et), varSlot), et);
            } else {
                emitRetainValue(varSlot, et);
            }
        }
        const size_t outerDepth = refScopes.size();
        loops.push_back({incBB, endBB});
        pushRefScope();
        if (ownVar) registerLocalCleanup(s->lvar.get());
        loopScopeDepth.push_back(outerDepth);
        if (paraSpawn) emitParaForSpawnItem(varSlot); else block(s->body.get());
        loopScopeDepth.pop_back();
        loops.pop_back();
        popRefScope();
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
        b.SetInsertPoint(incBB);
        Value* ci3 = b.CreateLoad(i64(), idx);
        b.CreateStore(b.CreateAdd(ci3, ConstantInt::get(i64(), 1)), idx);
        b.CreateBr(condBB);
        b.SetInsertPoint(endBB);
    }

    // `x is Alt(a, b)` / `x is Cat d`: a runtime test. For a sum value the tag is compared and the
    // payload is bound (stored into the binding locals declared by the `if` checker); for a
    // class/interface value the descriptor chain is walked, reusing the `as` machinery.
    Value* Emitter::evalIs(IsExpr* e) {
        if (e->is_sum) {
            const TypePtr& st = e->sum_ty;
            Value* sv = eval(e->obj.get());
            auto* slot = entryAlloca(lt(st), "is.sum", alignOf(st));
            b.CreateStore(sv, slot);
            if (st->needs_cleanup() && valueOwned(e->obj.get())) stmtTemps.emplace_back(slot, st);
            Value* tag = loadAt(i32(), fieldAddr(slot, st->sum_layout, 0), false);
            Value* cond = b.CreateICmpEQ(tag, ConstantInt::get(i32(), e->alt));
            if (!e->bind_locals.empty()) {
                const auto& fs = st->sum_fields[static_cast<size_t>(e->alt)];
                Value* pay = fieldAddr(slot, st->sum_layout, 1);
                for (size_t k = 0; k < e->bind_locals.size() && k < fs.size(); ++k) {
                    Value* src = pay;
                    if (fs.size() > 1) {
                        src = fieldAddr(pay, st->sum_payload[static_cast<size_t>(e->alt)],
                                        static_cast<int>(k));
                    }
                    b.CreateStore(loadAt(lt(fs[k]), src, false), slotFor(e->bind_locals[k]));
                }
            }
            return cond;
        }
        // `is` on a class/interface value is a safe runtime test (SPEC 10.1.2): a null receiver
        // matches nothing, and an interface value is a `{obj*, desc*}` aggregate, so the object
        // pointer is taken first. Both are exactly the `as` lowering, so reuse it: `x is C` is
        // `x as C != null`, and the optional binding is that non-null pointer.
        Value* srcv = eval(e->obj.get());
        // An owned scrutinee (`fetch() is Dog`) must be released at scope end; the downcast result
        // is an alias, not a new reference (#215).
        if (isRefKind(e->obj->ty) && valueOwned(e->obj.get())) {
            auto* slot = entryAlloca(lt(e->obj->ty), "is.obj", alignOf(e->obj->ty));
            b.CreateStore(srcv, slot);
            stmtTemps.emplace_back(slot, e->obj->ty);
        }
        Value* cast = emitDowncast(srcv, e->obj->ty, e->test_ty);
        Value* cond = b.CreateICmpNE(cast, Constant::getNullValue(ptrTy()));
        if (!e->bind_locals.empty()) b.CreateStore(cast, slotFor(e->bind_locals[0]));
        return cond;
    }

    // Constant-pattern match (integer / enum scrutinee; SPEC 10.1.2). Without guards the value goes
    // through an LLVM switch (jump table) and the ranges through a comparison chain, exactly as the
    // former `switch` did. A guard makes source order observable, so the whole dispatch becomes an
    // ordered chain: test the arm's patterns, evaluate its guard once on the hit path, then branch.
    void Emitter::emitConstMatch(Expr* scrut, const std::vector<std::unique_ptr<MatchArm>>& arms,
                                 const TypePtr& st, AllocaInst* result) {
        Value* sv = eval(scrut);
        LType* sty = sv->getType();
        BasicBlock* endBB = BasicBlock::Create(ctx, "match.end", curFn);
        std::vector<BasicBlock*> bodyBB;
        bodyBB.reserve(arms.size());
        for (size_t i = 0; i < arms.size(); ++i) {
            bodyBB.push_back(BasicBlock::Create(ctx, "match.arm", curFn));
        }
        int elseIdx = -1;
        bool anyGuard = false;
        for (size_t i = 0; i < arms.size(); ++i) {
            if (arms[i]->is_else) elseIdx = static_cast<int>(i);
            if (arms[i]->guard) anyGuard = true;
        }
        BasicBlock* fallback = elseIdx >= 0 ? bodyBB[static_cast<size_t>(elseIdx)] : endBB;
        const bool unsig = st && !st->is_signed();

        auto constOf = [&](long long v) {
            return ConstantInt::get(llvm::cast<IntegerType>(sty), static_cast<uint64_t>(v), true);
        };
        auto patCond = [&](const MatchPat& p) -> Value* {
            if (!p.is_range) return b.CreateICmpEQ(sv, constOf(p.vlo));
            Value* ge = unsig ? b.CreateICmpUGE(sv, constOf(p.vlo)) : b.CreateICmpSGE(sv, constOf(p.vlo));
            Value* lt = unsig ? b.CreateICmpULT(sv, constOf(p.vhi)) : b.CreateICmpSLT(sv, constOf(p.vhi));
            return b.CreateAnd(ge, lt);
        };

        if (!anyGuard) {
            std::vector<std::pair<ConstantInt*, BasicBlock*>> cases;
            std::vector<std::pair<const MatchPat*, BasicBlock*>> ranges;
            std::unordered_set<long long> seen_values;
            for (size_t i = 0; i < arms.size(); ++i) {
                for (auto& p : arms[i]->pats) {
                    if (p.is_range) {
                        ranges.push_back({&p, bodyBB[i]});
                    } else {
                        // Distinct enum members may share a value (aliases); LLVM rejects duplicate
                        // switch cases, and the runtime value cannot distinguish them. The first arm
                        // wins; the later one is unreachable (#158).
                        if (!seen_values.insert(p.vlo).second) continue;
                        cases.push_back({constOf(p.vlo), bodyBB[i]});
                    }
                }
            }
            BasicBlock* defBB = BasicBlock::Create(ctx, "match.default", curFn);
            if (cases.empty()) {
                b.CreateBr(defBB);
            } else {
                auto* sw = b.CreateSwitch(sv, defBB, static_cast<unsigned>(cases.size()));
                for (auto& entry : cases) sw->addCase(entry.first, entry.second);
            }
            b.SetInsertPoint(defBB);
            for (size_t k = 0; k < ranges.size(); ++k) {
                BasicBlock* next = (k + 1 < ranges.size())
                                       ? BasicBlock::Create(ctx, "match.next", curFn)
                                       : fallback;
                Value* lo = constOf(ranges[k].first->vlo);
                Value* hi = constOf(ranges[k].first->vhi);
                Value* ge = unsig ? b.CreateICmpUGE(sv, lo) : b.CreateICmpSGE(sv, lo);
                Value* lt = unsig ? b.CreateICmpULT(sv, hi) : b.CreateICmpSLT(sv, hi);
                b.CreateCondBr(b.CreateAnd(ge, lt), ranges[k].second, next);
                if (k + 1 < ranges.size()) b.SetInsertPoint(next);
            }
            if (ranges.empty()) b.CreateBr(fallback);
        } else {
            BasicBlock* cur = b.GetInsertBlock();
            for (size_t i = 0; i < arms.size(); ++i) {
                MatchArm* a = arms[i].get();
                b.SetInsertPoint(cur);
                if (a->is_else) {
                    b.CreateBr(bodyBB[i]);
                    cur = nullptr;
                    continue;
                }
                BasicBlock* hit = BasicBlock::Create(ctx, "match.hit", curFn);
                for (const auto& p : a->pats) {
                    b.SetInsertPoint(cur);
                    BasicBlock* next = BasicBlock::Create(ctx, "match.test", curFn);
                    b.CreateCondBr(patCond(p), hit, next);
                    cur = next;
                }
                b.SetInsertPoint(hit);
                if (a->guard) {
                    Value* g = eval(a->guard.get());
                    BasicBlock* ok = BasicBlock::Create(ctx, "match.guard", curFn);
                    b.CreateCondBr(g, ok, cur);  // a failed guard falls through to the next arm
                    b.SetInsertPoint(ok);
                }
                b.CreateBr(bodyBB[i]);
            }
            if (cur) {
                b.SetInsertPoint(cur);
                b.CreateBr(fallback);
            }
        }

        for (size_t i = 0; i < arms.size(); ++i) {
            b.SetInsertPoint(bodyBB[i]);
            MatchArm* a = arms[i].get();
            if (a->body) {
                block(a->body.get());
            } else if (a->value && result) {
                b.CreateStore(eval(a->value.get()), result);
            } else if (a->value) {
                (void)eval(a->value.get());
            }
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
        }
        b.SetInsertPoint(endBB);
    }

    void Emitter::emitMatchStmt(MatchStmt* s) {
        emitMatchCommon(s->scrut.get(), s->arms, s->sty, s->match_ref, nullptr);
    }

    Value* Emitter::evalMatch(MatchExpr* e) {
        const bool voidResult = is_void(e->ty);
        AllocaInst* result = nullptr;
        if (!voidResult) result = entryAlloca(lt(e->ty), "match.res", alignOf(e->ty));
        emitMatchCommon(e->scrut.get(), e->arms, e->sty, e->match_ref, result, e->ty);
        if (voidResult) return Constant::getNullValue(i32());
        return loadAt(lt(e->ty), result, unalignedExpr(e));
    }

    // SPEC 10.1.2: dispatch on the tag, bind the payload, evaluate the guard, then the arm body.
    // A `ref` match works on the scrutinee's own storage (the sema layer requires an addressable
    // place); every other match copies the scrutinee into an owned temporary first.
    void Emitter::emitMatchCommon(Expr* scrut, const std::vector<std::unique_ptr<MatchArm>>& arms,
                                  const TypePtr& st, bool match_ref, AllocaInst* result,
                                  const TypePtr& result_ty) {
        if (!st || st->kind != TypeKind::Sum) {
            emitConstMatch(scrut, arms, st, result);
            return;
        }
        Value* addr = nullptr;
        if (match_ref) {
            addr = addrOf(scrut);
        } else {
            Value* sv = eval(scrut);
            auto* slot = entryAlloca(lt(st), "match.sum", alignOf(st));
            b.CreateStore(sv, slot);
            if (st->needs_cleanup() && valueOwned(scrut)) stmtTemps.emplace_back(slot, st);
            addr = slot;
        }
        BasicBlock* endBB = BasicBlock::Create(ctx, "match.end", curFn);
        std::vector<BasicBlock*> bodyBB;
        bodyBB.reserve(arms.size());
        int elseIdx = -1;
        for (size_t i = 0; i < arms.size(); ++i) {
            bodyBB.push_back(BasicBlock::Create(ctx, "match.body", curFn));
            if (arms[i]->is_else) elseIdx = static_cast<int>(i);
        }
        BasicBlock* fallback = elseIdx >= 0 ? bodyBB[static_cast<size_t>(elseIdx)] : endBB;
        Value* pay = fieldAddr(addr, st->sum_layout, 1);
        Value* tag = loadAt(i32(), fieldAddr(addr, st->sum_layout, 0), false);

        // One entry block per (arm, alternative): it loads that alternative's payload into the arm's
        // shared locals, checks the guard, and jumps to the arm body. `A(x) | B(x)` therefore binds
        // `x` from whichever alternative matched.
        std::vector<std::vector<std::pair<int, BasicBlock*>>> entries(arms.size());
        for (size_t i = 0; i < arms.size(); ++i) {
            for (auto& a : arms[i]->alts) {
                entries[i].push_back({a.index, BasicBlock::Create(ctx, "match.alt", curFn)});
            }
        }
        auto entryFor = [&](size_t arm, int alt) -> BasicBlock* {
            for (auto& e : entries[arm]) {
                if (e.first == alt) return e.second;
            }
            return nullptr;
        };
        // The next arm that also covers this alternative: a failed guard falls through to it.
        auto nextEntryFor = [&](size_t i, int alt) -> BasicBlock* {
            for (size_t j = i + 1; j < arms.size(); ++j) {
                if (arms[j]->is_else) continue;
                if (BasicBlock* bb = entryFor(j, alt)) return bb;
            }
            return fallback;
        };

        BasicBlock* defBB = BasicBlock::Create(ctx, "match.default", curFn);
        b.CreateBr(defBB);
        b.SetInsertPoint(defBB);
        auto* sw = b.CreateSwitch(tag, fallback, static_cast<unsigned>(st->sum_names.size()));
        std::vector<char> first(st->sum_names.size(), 0);
        for (size_t i = 0; i < arms.size(); ++i) {
            for (auto& e : entries[i]) {
                if (first[static_cast<size_t>(e.first)]) continue;
                first[static_cast<size_t>(e.first)] = 1;
                sw->addCase(cast<ConstantInt>(ConstantInt::get(i32(), e.first)), e.second);
            }
        }

        for (size_t i = 0; i < arms.size(); ++i) {
            MatchArm* a = arms[i].get();
            pushRefScope();  // value bindings are retained for the arm and released when it ends
            if (!a->alts.empty()) {
                // Slots and cleanup registration are per local: every alternative writes the same
                // locals (sema requires identical names, modes and types across alternatives). The
                // registration emits the "moved = false" store, so the insert point must already be a
                // real block — use the first entry.
                const auto& fs0 = st->sum_fields[static_cast<size_t>(a->alts[0].index)];
                std::vector<AllocaInst*> slotOf;
                b.SetInsertPoint(entries[i][0].second);
                for (size_t k = 0; k < a->bind_locals.size() && k < fs0.size(); ++k) {
                    LocalVar* lv = a->bind_locals[k];
                    AllocaInst* slot = lv->by_ref ? entryAlloca(ptrTy(), sane(lv->name))
                                                  : entryAlloca(lt(fs0[k]), sane(lv->name), alignOf(fs0[k]));
                    slots[lv] = slot;
                    slotOf.push_back(slot);
                    if (!lv->by_ref && fs0[k]->needs_cleanup()) registerLocalCleanup(lv);
                }
                for (auto& [alt, bb] : entries[i]) {
                    b.SetInsertPoint(bb);
                    const auto& fs = st->sum_fields[static_cast<size_t>(alt)];
                    for (size_t k = 0; k < slotOf.size() && k < fs.size(); ++k) {
                        LocalVar* lv = a->bind_locals[k];
                        Value* src = pay;
                        if (fs.size() > 1) src = fieldAddr(pay, st->sum_payload[static_cast<size_t>(alt)],
                                                           static_cast<int>(k));
                        if (lv->by_ref) {
                            b.CreateStore(src, slotOf[k]);
                        } else {
                            // Re-arm the moved flag on this path (an alternative other than the first
                            // did not run the registration store).
                            if (fs[k]->needs_cleanup()) createMovedFlag(lv);
                            if (!fs[k]->copyable()) {
                                // A move-only payload is moved out of the union: the binding owns it,
                                // and the source payload is left empty so the scrutinee temporary's
                                // tag-aware destruction only sees a zeroed value. A user move
                                // constructor defines the source state itself (C++ model).
                                if (fs[k]->kind == TypeKind::Struct && fs[k]->has_move_ctor) {
                                    FunctionCallee ctor = mod->getOrInsertFunction(
                                        move_ctor_cname(fs[k]->unit, fs[k]->name),
                                        FunctionType::get(LType::getVoidTy(ctx), {ptrTy(), ptrTy()},
                                                          false));
                                    if (fs[k]->needs_cleanup()) zeroMem(slotOf[k], fs[k]);
                                    b.CreateCall(ctor, {slotOf[k], src});
                                    emitCheckPending();
                                } else {
                                    b.CreateStore(loadAt(lt(fs[k]), src, false), slotOf[k]);
                                    zeroMem(src, fs[k]);
                                }
                            } else {
                                b.CreateStore(loadAt(lt(fs[k]), src, false), slotOf[k]);
                                if (fs[k]->needs_cleanup()) emitRetainValue(slotOf[k], fs[k]);
                            }
                        }
                    }
                    if (a->guard) {
                        Value* g = eval(a->guard.get());
                        BasicBlock* failBB = BasicBlock::Create(ctx, "match.next", curFn);
                        b.CreateCondBr(g, bodyBB[i], failBB);
                        // A failed guard releases this arm's bindings before trying the next arm.
                        b.SetInsertPoint(failBB);
                        for (auto it = refScopes.back().rbegin(); it != refScopes.back().rend(); ++it) {
                            releaseLocal(*it);
                        }
                        b.CreateBr(nextEntryFor(i, alt));
                    } else {
                        b.CreateBr(bodyBB[i]);
                    }
                }
            }
            b.SetInsertPoint(bodyBB[i]);
            if (a->body) {
                block(a->body.get());
            } else if (result) {
                // The result slot owns one value, exactly like a `?:` arm: a borrowed payload binding
                // is retained before popRefScope releases it; an owned source transfers ownership.
                emitTernaryArm(result, a->value.get(), result_ty);
            } else if (a->value) {
                (void)eval(a->value.get());
            }
            popRefScope();  // releases the bindings on the arm's normal exit
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
        }
        b.SetInsertPoint(endBB);
    }

}  // namespace muc_detail
