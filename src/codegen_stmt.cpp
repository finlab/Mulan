// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 本文件包含语句代码生成（存取、初始化、块/语句分派、for/switch）（Emitter 成员函数定义）。

#include "codegen_impl.hpp"

namespace muc_detail {

using ArgPack = Emitter::ArgPack;

    // ------------------------------------------------------------ 语句
    // 访问对齐：packed 结构链上的字段/元素只保证 1 字节对齐，必须把 load/store
    // 的 align 显式降到 1，否则 LLVM 会按脚点类型对齐生成错误的非对齐访问。
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
        // 只承诺 1 字节对齐：源/目的可能是 packed 结构里的字段
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
                // 构造器可能不写全部引用字段：先清零，保证析构时不会释放垃圾指针
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
                // 借用源：逐字段 retain；owned 源（调用返回/构造结果）：所有权转移
                storeValue(v, addr, ty, unaligned);
                if (!valueOwned(e)) emitRetainValue(addr, ty, unaligned);
                return;
            }
            if (ty->kind == TypeKind::Slice) {
                // 借用源（另一个动态数组）：retain；new/数组转换/调用返回：所有权转移
                storeValue(v, addr, ty, unaligned);
                if (!convOwns(e, ty)) emitRetainValue(addr, ty, unaligned);
                return;
            }
            storeValue(v, addr, ty, unaligned);
            return;
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            // 空花括号 `{}`：聚合零初始化（等价于旧的 `= 0`）
            if (ia->items.empty()) {
                zeroMem(addr, ty, unaligned);
                return;
            }
            // 与 C 一致：聚合初始化未覆盖的部分清零
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
                for (size_t i = 0; i < ia->items.size() && i < ty->array_size; ++i) {
                    Value* p = gep(lt(ty->elem), inner, ConstantInt::get(i64(), i));
                    initInto(p, ty->elem, ia->items[i].second.get(), unaligned);
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
            destroyTempsFrom(cmark);  // 条件里的 struct 右值每轮释放
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
            Value* hi = convertTo(eval(re->hi.get()), re->hi->ty, vt);
            b.CreateStore(lo, varSlot);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            Value* cur = b.CreateLoad(lt(vt), varSlot);
            b.CreateCondBr(b.CreateICmpSLT(cur, hi), bodyBB2, endBB);
            b.SetInsertPoint(bodyBB2);
            if (idxSlot) {
                TypePtr us = Type::make_prim("usize");
                Value* curv = b.CreateLoad(lt(vt), varSlot);
                Value* diff = b.CreateSub(curv, lo);
                b.CreateStore(convertTo(diff, vt, us), idxSlot);
            }
            loops.push_back({incBB, endBB});
            loopScopeDepth.push_back(refScopes.size());
            block(s->body.get());
            loopScopeDepth.pop_back();
            loops.pop_back();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
            b.SetInsertPoint(incBB);
            Value* c2 = b.CreateLoad(lt(vt), varSlot);
            b.CreateStore(b.CreateAdd(c2, ConstantInt::get(lt(vt), 1)), varSlot);
            b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            return;
        }

        TypePtr sty = s->seq_ty;
        Value* seqAddr = nullptr;
        if (auto n = dynamic_cast<Name*>(s->seq.get())) {
            (void)n;
        }
        // 简单左值直接取地址；否则先物化（只求值一次）
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
            // owned 的动态数组/数组临时值：循环结束后释放
            if (needsCleanup(sty) && valueOwned(s->seq.get()))
                stmtTemps.emplace_back(seqAddr, sty);
        }

        // 索引形态（SPEC 6.4.1）：length() 一次 + operator[](C, usize) 每轮
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
            b.CreateStore(emitOperatorIndexCall(s->idx_fn, seqAddr, sty, ci2, seqUnaligned),
                          varSlot);
            // 调用结果是 owned 值：本轮结束（正常/break/continue）时释放
            const bool ownVar = needsCleanup(s->elem_ty);
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

        // 长度与元素地址
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
            elemPtr = data;  // string：byte 视图
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
        // 值绑定是拥有型局部变量（SPEC §4）：每轮 retain，离开本轮（正常/break/continue）时 release
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

    // switch：值分支走 LLVM switch（跳表），区间分支走比较链，else 兜底。
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

        // 默认路径：按源码顺序测试区间分支，最后落到 else 或 switch.end
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
