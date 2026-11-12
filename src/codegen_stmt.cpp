// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains statement code generation (load/store, initialization, block/statement dispatch, for/switch) (Emitter member function definitions).

#include "codegen_impl.hpp"

namespace muc_detail {

using ArgPack = Emitter::ArgPack;

    // ------------------------------------------------------------ statements
    // Access alignment: fields/elements on a packed struct chain only guarantee 1-byte alignment, so the align of load/store
    // must be explicitly lowered to 1, otherwise LLVM generates incorrect unaligned accesses according to the pointer type's alignment.
    void Emitter::storeAt(Value* v, Value* addr, bool unaligned) {
        if (!v) return;
        auto* si = b.CreateStore(v, addr);
        if (unaligned) si->setAlignment(llvm::Align(1));
    }

    Value* Emitter::loadAt(LType* ty, Value* addr, bool unaligned) {
        auto* li = b.CreateLoad(ty, addr);
        if (unaligned) li->setAlignment(llvm::Align(1));
        return li;
    }

    void Emitter::storeValue(Value* v, Value* addr, const TypePtr& ty, bool unaligned) {
        if (!v) return;
        if (ty->kind == TypeKind::Void) return;
        storeAt(v, addr, unaligned);
    }

    void Emitter::zeroMem(Value* addr, const TypePtr& ty, bool unaligned) {
        uint64_t n = sizeOf(ty);
        if (n == 0) return;
        b.CreateMemSet(addr, ConstantInt::get(i8(), 0), ConstantInt::get(i64(), n),
                       MaybeAlign(unaligned ? 1ULL : static_cast<uint64_t>(alignOf(ty))));
    }

    void Emitter::copyMem(Value* dst, Value* src, uint64_t n) {
        if (n == 0) return;
        // only 1-byte alignment is promised: the source/destination may be a field inside a packed struct
        b.CreateMemCpy(dst, MaybeAlign(1), src, MaybeAlign(1), ConstantInt::get(i64(), n));
    }

    void Emitter::initInto(Value* addr, const TypePtr& ty, Init* init, bool unaligned) {
        if (std::getenv("MUC_LLVM_TRACE"))
            std::cerr << "[llvm]     init " << (ty ? ty->str() : "?")
                      << (dynamic_cast<InitAggregate*>(init) ? " agg" : " expr") << "\n";
        if (!init) return;
        if (is_zero_literal(init) && zeroable(ty)) {
            zeroMem(addr, ty);
            return;
        }
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            Expr* e = ie->expr.get();
            if (isRefKind(ty) && is_zero_literal(init)) {
                storeValue(Constant::getNullValue(lt(ty)), addr, ty);
                return;
            }
            if (auto call = dynamic_cast<Call*>(e);
                call && call->callee_kind == "struct_ctor") {
                TypePtr st = call->ctor;
                const auto* cpty = call->ctor_pty;
                // the constructor may not write every reference field: zero first, to make sure the destructor never releases a garbage pointer
                if (st->needs_cleanup()) zeroMem(addr, st, unaligned);
                std::vector<LType*> ptys{ptrTy()};
                for (auto& t : *cpty) ptys.push_back(lt(t));
                FunctionCallee callee = mod->getOrInsertFunction(
                    ctor_cname(st->unit, st->name, *cpty),
                    FunctionType::get(LType::getVoidTy(ctx), ptys, false));
                std::vector<Expr*> rawArgs;
                for (auto& a : call->args) rawArgs.push_back(a.get());
                ArgPack pack = prepareArgs(rawArgs, *cpty);
                std::vector<Value*> vals{addr};
                for (auto* v : pack.vals) vals.push_back(v);
                b.CreateCall(callee, vals);
                releaseTemps(pack);
                return;
            }
            if (ty->kind == TypeKind::Array && sizeOf(ty->elem) == 1 &&
                dynamic_cast<StringLit*>(e)) {
                if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm]       char-array from string\n";
                Value* s = stringConst(static_cast<StringLit*>(e)->value);
                if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm]       A\n";
                Value* sp = b.CreateExtractValue(s, {0});
                Value* sl = b.CreateExtractValue(s, {1});
                if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm]       B\n";
                zeroMem(addr, ty, unaligned);
                if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm]       C\n";
                b.CreateMemCpy(addr, MaybeAlign(unaligned ? 1 : alignOf(ty)), sp, MaybeAlign(1), sl);
                if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm]       D\n";
                return;
            }
            Value* v = coerce(e, ty);
            if (isRefKind(ty)) {
                if (!ownsReference(e)) emitRetainRef(v, ty);
                storeValue(v, addr, ty, unaligned);
                return;
            }
            if (ty->kind == TypeKind::Array && needsCleanup(ty)) {
                storeValue(v, addr, ty, unaligned);
                if (!valueOwned(e)) emitRetainValue(addr, ty, unaligned);
                return;
            }
            if (ty->kind == TypeKind::Struct && needsCleanup(ty)) {
                // borrowed source: retain field by field; owned source (a call result / a construction result): ownership transfer
                storeValue(v, addr, ty, unaligned);
                if (!valueOwned(e)) emitRetainValue(addr, ty, unaligned);
                return;
            }
            if (ty->kind == TypeKind::Slice) {
                // borrowed source (another dynamic array): retain; new / array conversion / call result: ownership transfer
                storeValue(v, addr, ty, unaligned);
                if (!convOwns(e, ty)) emitRetainValue(addr, ty, unaligned);
                return;
            }
            storeValue(v, addr, ty, unaligned);
            return;
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            // empty braces `{}`: aggregate zero initialization (equivalent to the old `= 0`)
            if (ia->items.empty()) {
                zeroMem(addr, ty, unaligned);
                return;
            }
            // consistent with C: the parts not covered by the aggregate initialization are zeroed
            zeroMem(addr, ty, unaligned);
            if (ty->kind == TypeKind::Struct) {
                std::vector<std::pair<int, Init*>> slot;
                bool named = false;
                for (auto& it : ia->items) {
                    if (!it.first.empty()) named = true;
                }
                if (named) {
                    for (auto& it : ia->items) {
                        int idx = ty->field_index(it.first);
                        if (idx >= 0) slot.emplace_back(idx, it.second.get());
                    }
                } else {
                    for (size_t i = 0; i < ia->items.size(); ++i)
                        slot.emplace_back(static_cast<int>(i), ia->items[i].second.get());
                }
                auto* st = cast<LStruct>(lt(ty));
                for (auto& [idx, sub] : slot) {
                    Value* fp = fieldSlot(st, ty, addr, idx);
                    initInto(fp, ty->fields[idx].type, sub, unaligned || ty->is_packed);
                }
                return;
            }
            if (ty->kind == TypeKind::Array) {
                auto* st = cast<LStruct>(lt(ty));
                Value* inner = gep(st, addr, {ConstantInt::get(i64(), 0),
                                                       ConstantInt::get(i32(), 0)});
                size_t i = 0;
                for (auto& it : ia->items) {
                    if (auto rg = dynamic_cast<InitRange*>(it.second.get())) {
                        for (long long k = 0; k < rg->count && i < ty->array_size; ++k, ++i) {
                            Value* p = gep(lt(ty->elem), inner, ConstantInt::get(i64(), i));
                            Value* v = ConstantInt::get(lt(ty->elem), static_cast<uint64_t>(rg->lo_val + k));
                            b.CreateStore(v, p);
                        }
                        continue;
                    }
                    if (i >= ty->array_size) break;
                    Value* p = gep(lt(ty->elem), inner, ConstantInt::get(i64(), i));
                    initInto(p, ty->elem, it.second.get(), unaligned);
                    ++i;
                }
                return;
            }
            if (ty->kind == TypeKind::Slice) {
                std::vector<Init*> elems;
                for (auto& it : ia->items) elems.push_back(it.second.get());
                Value* h = emitDynArrayAlloc(ConstantInt::get(i64(), elems.size()), ty->elem);
                Value* data = sliceDataPtr(h, ty->rank);
                for (size_t i = 0; i < elems.size(); ++i) {
                    Value* p = b.CreateGEP(lt(ty->elem), data, ConstantInt::get(i64(), i));
                    initInto(p, ty->elem, elems[i], unaligned);
                }
                storeAt(h, addr, unaligned);
                return;
            }
        }
        fail("LLVM backend: unsupported initializer", init->line, init->col);
    }

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
        if (auto b0 = dynamic_cast<Block*>(s)) {
            block(b0);
            return;
        }
        if (auto lv = dynamic_cast<LocalVar*>(s)) {
            AllocaInst* slot = slotFor(lv);
            if (lv->init) {
                initInto(slot, lv->rty, lv->init.get());
            } else if (isRefKind(lv->rty)) {
                b.CreateStore(Constant::getNullValue(lt(lv->rty)), slot);
            } else if (lv->rty && lv->rty->kind == TypeKind::Struct) {
                if (!lv->rty->ctors.empty()) {
                    FunctionCallee ctor = mod->getOrInsertFunction(
                        ctor_cname(lv->rty->unit, lv->rty->name, {}),
                        FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                    if (lv->rty->needs_cleanup()) zeroMem(slot, lv->rty);
                    b.CreateCall(ctor, {slot});
                } else if (needsCleanup(lv->rty)) {
                    zeroMem(slot, lv->rty);
                }
            } else if (lv->rty && lv->rty->kind == TypeKind::Slice) {
                b.CreateStore(Constant::getNullValue(ptrTy()), slot);
            } else if (lv->rty && lv->rty->kind == TypeKind::Array && needsCleanup(lv->rty)) {
                zeroMem(slot, lv->rty);
            }
            if (needsCleanup(lv->rty) && !lv->by_ref) refScopes.back().push_back(lv);
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
            if (as->set_func) {
                auto* ix = dynamic_cast<Index*>(as->target.get());
                emitUserCall(as->set_func,
                             {ix->base.get(), ix->indices[0].get(), as->value.get()});
                return;
            }
            Value* addr = addrOf(as->target.get());
            const bool una = unalignedExpr(as->target.get());
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
                    if (!ownsReference(as->value.get())) emitRetainRef(nv, as->target->ty);
                    emitReleaseRef(old, as->target->ty);
                    storeAt(nv, addr, una);
                    return;
                }
                TypePtr tty = as->target->ty;
                if (!weakTarget && tty && needsCleanup(tty) &&
                    (tty->kind == TypeKind::Array || tty->kind == TypeKind::Struct ||
                     tty->kind == TypeKind::Slice)) {
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
            else if (op == "/") res = fl ? b.CreateFDiv(old, rv) : (sg ? b.CreateSDiv(old, rv) : b.CreateUDiv(old, rv));
            else if (op == "%") res = fl ? b.CreateFRem(old, rv) : (sg ? b.CreateSRem(old, rv) : b.CreateURem(old, rv));
            else if (op == "&") res = b.CreateAnd(old, rv);
            else if (op == "|") res = b.CreateOr(old, rv);
            else if (op == "^") res = b.CreateXor(old, rv);
            else if (op == "<<") res = b.CreateShl(old, rv);
            else if (op == ">>") res = b.CreateAShr(old, rv);
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
            destroyTempsFrom(0);
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().second);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (dynamic_cast<ContinueStmt*>(s)) {
            if (loops.empty()) fail("LLVM backend: `continue` outside of a loop");
            destroyTempsFrom(0);
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().first);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            if (!rs->value) {
                destroyTempsFrom(0);
                releaseAllScopes();
                b.CreateRetVoid();
            } else {
                Value* rv = prepareReturn(rs->value.get());
                destroyTempsFrom(0);
                releaseAllScopes();
                b.CreateRet(rv);
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

        AllocaInst* varSlot = slotFor(s->lvar.get());
        AllocaInst* idxSlot = (s->has_idx && s->idx_lvar) ? slotFor(s->idx_lvar.get()) : nullptr;

        if (s->loop_kind == "range") {
            auto* re = dynamic_cast<RangeExpr*>(s->seq.get());
            TypePtr vt = s->var_ty;
            Value* lo = convertTo(eval(re->lo.get()), re->lo->ty, vt);
            Value* hiV = convertTo(eval(re->hi.get()), re->hi->ty, vt);
            const bool down = s->step_val < 0;
            Value* stepV = ConstantInt::get(lt(vt), static_cast<uint64_t>(s->step_val));
            b.CreateStore(lo, varSlot);
            if (idxSlot) b.CreateStore(ConstantInt::get(cast<IntegerType>(lt(Type::make_prim("usize"))), 0), idxSlot);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            Value* cur = b.CreateLoad(lt(vt), varSlot);
            Value* cmp = nullptr;
            if (!down) {
                cmp = re->inclusive ? b.CreateICmpSLE(cur, hiV) : b.CreateICmpSLT(cur, hiV);
            } else {
                cmp = re->inclusive ? b.CreateICmpSGE(cur, hiV) : b.CreateICmpSGT(cur, hiV);
            }
            b.CreateCondBr(cmp, bodyBB2, endBB);
            b.SetInsertPoint(bodyBB2);
            loops.push_back({incBB, endBB});
            loopScopeDepth.push_back(refScopes.size());
            block(s->body.get());
            loopScopeDepth.pop_back();
            loops.pop_back();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
            b.SetInsertPoint(incBB);
            Value* c2 = b.CreateLoad(lt(vt), varSlot);
            b.CreateStore(b.CreateAdd(c2, stepV), varSlot);
            if (idxSlot) {
                TypePtr us = Type::make_prim("usize");
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

        // index form (SPEC 6.4.1): length() once + operator[](C, usize) each iteration
        if (s->loop_kind == "index") {
            TypePtr us = Type::make_prim("usize");
            Value* recv =
                (sty->kind == TypeKind::Class) ? b.CreateLoad(lt(sty), seqAddr) : seqAddr;
            Value* n = emitMethodCallVals(s->len_sig, recv, sty, {}, s);
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
            Value* ev = emitOperatorIndexCall(s->idx_fn, seqAddr, sty, ci2, seqUnaligned);
            if (s->idx_fn && is_borrow_type(s->idx_fn->rty)) {
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
            if (ownVar) refScopes.back().push_back(s->lvar.get());
            loopScopeDepth.push_back(outerDepth);
            block(s->body.get());
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
        if (ownVar) refScopes.back().push_back(s->lvar.get());
        loopScopeDepth.push_back(outerDepth);
        block(s->body.get());
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

    // switch: value branches use an LLVM switch (jump table), range branches use a comparison chain, and else is the fallback.
    void Emitter::emitSwitch(SwitchStmt* s) {
        Value* sv = eval(s->scrut.get());
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

}  // namespace muc_detail
