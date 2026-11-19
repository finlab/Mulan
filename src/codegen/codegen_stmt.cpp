// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains statement code generation (block/statement dispatch, for/switch/para/try) (Emitter member function definitions).
//
// owns:      statement emission (block/if/while/for/switch/para/try) and the statement-level
//            temporary and ref scopes
// not:       the load/store and initialization primitives (codegen_mem.cpp), expression evaluation,
//            call lowering, class metadata
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
                // (agents/value-semantics.md §6.3). POD values stay uninitialized, as in C++.
                const bool zeroed = lv->rty->needs_cleanup();
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
                           (ty->kind == TypeKind::Struct || ty->kind == TypeKind::Slice)) {
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
                as->target->ty->needs_ctor_glue()) {
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
        if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            emitFor(fs);
            return;
        }
        if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            emitSwitch(sw);
            return;
        }
        if (dynamic_cast<BreakStmt*>(s)) {
            if (loops.empty()) fail("LLVM backend: `break` outside of a loop");
            emitFinallyForLoopExit();
            destroyTempsFrom(0);
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().second);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (dynamic_cast<ContinueStmt*>(s)) {
            if (loops.empty()) fail("LLVM backend: `continue` outside of a loop");
            emitFinallyForLoopExit();
            destroyTempsFrom(0);
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().first);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            if (!rs->value) {
                emitFinallyBodies(finallyStack.size());
                destroyTempsFrom(0);
                releaseAllScopes();
                b.CreateRetVoid();
            } else {
                Value* rv = prepareReturn(rs->value.get());
                emitFinallyBodies(finallyStack.size());
                destroyTempsFrom(0);
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
            if (!down) {
                cmp = re->inclusive ? b.CreateICmpSLE(cur, hiV) : b.CreateICmpSLT(cur, hiV);
            } else {
                cmp = re->inclusive ? b.CreateICmpSGE(cur, hiV) : b.CreateICmpSGT(cur, hiV);
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
            // an interface cursor is an owned value: release the copy when the statement ends
            if (s->it_ty->kind == TypeKind::Interface) stmtTemps.emplace_back(itSlot, s->it_ty);
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
            if (ownCur) emitRetainValue(varSlot, s->elem_ty);
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
        Value* cast = emitDowncast(srcv, e->obj->ty, e->test_ty);
        Value* cond = b.CreateICmpNE(cast, Constant::getNullValue(ptrTy()));
        if (!e->bind_locals.empty()) b.CreateStore(cast, slotFor(e->bind_locals[0]));
        return cond;
    }

    // switch: value branches use an LLVM switch (jump table), range branches use a comparison chain, and else is the fallback.
    void Emitter::emitSwitch(SwitchStmt* s) {
        Value* sv = eval(s->scrut.get());
        // Sum scrutinee (SPEC 10.1.2): switch on the tag and bind the payload fields in each arm.
        if (s->is_sum) {
            const TypePtr& st = s->sty;
            auto* slot = entryAlloca(lt(st), "switch.sum", alignOf(st));
            b.CreateStore(sv, slot);
            if (st->needs_cleanup() && valueOwned(s->scrut.get())) stmtTemps.emplace_back(slot, st);
            BasicBlock* endBB = BasicBlock::Create(ctx, "switch.end", curFn);
            Value* pay = fieldAddr(slot, st->sum_layout, 1);
            Value* tag = loadAt(i32(), fieldAddr(slot, st->sum_layout, 0), false);
            int elseIdx = -1;
            std::vector<BasicBlock*> bodyBB;
            for (size_t i = 0; i < s->arms.size(); ++i) {
                bodyBB.push_back(BasicBlock::Create(ctx, "switch.arm", curFn));
                if (s->arms[i]->is_else) elseIdx = static_cast<int>(i);
            }
            BasicBlock* fallback = elseIdx >= 0 ? bodyBB[static_cast<size_t>(elseIdx)] : endBB;
            BasicBlock* defBB = BasicBlock::Create(ctx, "switch.default", curFn);
            b.CreateBr(defBB);
            b.SetInsertPoint(defBB);
            auto* sw = b.CreateSwitch(tag, fallback, static_cast<unsigned>(s->arms.size()));
            for (size_t i = 0; i < s->arms.size(); ++i) {
                if (s->arms[i]->alt_index < 0) continue;
                sw->addCase(cast<ConstantInt>(ConstantInt::get(i32(), s->arms[i]->alt_index)), bodyBB[i]);
            }
            for (size_t i = 0; i < s->arms.size(); ++i) {
                b.SetInsertPoint(bodyBB[i]);
                const int alt = s->arms[i]->alt_index;
                const auto& binds = s->arms[i]->bind_locals;
                if (alt >= 0 && !binds.empty()) {
                    const auto& fs = st->sum_fields[static_cast<size_t>(alt)];
                    for (size_t k = 0; k < binds.size(); ++k) {
                        Value* src = pay;
                        if (fs.size() > 1) {
                            src = fieldAddr(pay, st->sum_payload[static_cast<size_t>(alt)],
                                            static_cast<int>(k));
                        }
                        Value* v = loadAt(lt(fs[k]), src, false);
                        auto* binding = entryAlloca(lt(fs[k]), sane(binds[k]->name), alignOf(fs[k]));
                        b.CreateStore(v, binding);
                        slots[binds[k]] = binding;
                        if (fs[k]->needs_cleanup()) {
                            emitRetainValue(binding, fs[k]);
                            registerLocalCleanup(binds[k]);
                        }
                    }
                }
                block(s->arms[i]->body.get());
                if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
            }
            b.SetInsertPoint(endBB);
            return;
        }
        LType* sty = sv->getType();
        BasicBlock* endBB = BasicBlock::Create(ctx, "switch.end", curFn);

        std::vector<BasicBlock*> bodyBB;
        bodyBB.reserve(s->arms.size());
        for (size_t i = 0; i < s->arms.size(); ++i) {
            bodyBB.push_back(BasicBlock::Create(ctx, "switch.arm", curFn));
        }

        int elseIdx = -1;
        std::vector<std::pair<ConstantInt*, BasicBlock*>> cases;
        std::vector<std::pair<SwitchPat*, BasicBlock*>> ranges;
        for (size_t i = 0; i < s->arms.size(); ++i) {
            if (s->arms[i]->is_else) elseIdx = static_cast<int>(i);
            for (auto& p : s->arms[i]->pats) {
                if (p.is_range) {
                    ranges.push_back({&p, bodyBB[i]});
                } else {
                    cases.push_back({ConstantInt::get(llvm::cast<IntegerType>(sty),
                                                      static_cast<uint64_t>(p.vlo), true),
                                     bodyBB[i]});
                }
            }
        }

        BasicBlock* fallback = elseIdx >= 0 ? bodyBB[elseIdx] : endBB;
        BasicBlock* defBB = BasicBlock::Create(ctx, "switch.default", curFn);
        if (cases.empty()) {
            b.CreateBr(defBB);
        } else {
            auto* sw = b.CreateSwitch(sv, defBB, static_cast<unsigned>(cases.size()));
            for (auto& entry : cases) sw->addCase(entry.first, entry.second);
        }

        // default path: test the range branches in source order, finally falling to else or switch.end
        b.SetInsertPoint(defBB);
        const bool unsig = s->sty && !s->sty->is_signed();
        for (size_t k = 0; k < ranges.size(); ++k) {
            BasicBlock* next = (k + 1 < ranges.size())
                                   ? BasicBlock::Create(ctx, "switch.next", curFn)
                                   : fallback;
            Value* lo = ConstantInt::get(llvm::cast<IntegerType>(sty),
                                         static_cast<uint64_t>(ranges[k].first->vlo), true);
            Value* hi = ConstantInt::get(llvm::cast<IntegerType>(sty),
                                         static_cast<uint64_t>(ranges[k].first->vhi), true);
            Value* ge = unsig ? b.CreateICmpUGE(sv, lo) : b.CreateICmpSGE(sv, lo);
            Value* lt = unsig ? b.CreateICmpULT(sv, hi) : b.CreateICmpSLT(sv, hi);
            b.CreateCondBr(b.CreateAnd(ge, lt), ranges[k].second, next);
            if (k + 1 < ranges.size()) b.SetInsertPoint(next);
        }
        if (ranges.empty()) b.CreateBr(fallback);

        for (size_t i = 0; i < s->arms.size(); ++i) {
            b.SetInsertPoint(bodyBB[i]);
            block(s->arms[i]->body.get());
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
        }
        b.SetInsertPoint(endBB);
    }

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
