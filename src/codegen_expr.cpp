// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains expression code generation (conversion, addresses, binary/ternary, argument preparation) (Emitter member function definitions).

#include "codegen_impl.hpp"

namespace muc_detail {

using ArgPack = Emitter::ArgPack;

static bool is_integer(const TypePtr& t) {
    if (!t) return false;
    if (t->kind == TypeKind::Enum) return true;
    return t->kind == TypeKind::Prim && t->is_integer();
}

static bool is_numeric(const TypePtr& t) {
    if (!t) return false;
    if (t->kind == TypeKind::Enum) return true;
    return t->kind == TypeKind::Prim && t->is_numeric();
}


    // ------------------------------------------------------------ expressions
    Value* Emitter::convertTo(Value* v, const TypePtr& from, const TypePtr& to, int line,
                              int col) {
        if (!from || !to || same_type(from, to)) return v;
        LType* ft = v->getType();
        LType* tt = lt(to);
        if (ft == tt) return v;

        if (from->kind == TypeKind::Null) return Constant::getNullValue(tt);
        if (from->kind == TypeKind::Class && to->kind == TypeKind::Interface) {
            Value* dp = b.CreateGEP(i8(), v, ConstantInt::get(i64(), 8));
            Value* d = b.CreateLoad(ptrTy(), dp);
            Value* iv = UndefValue::get(tt);
            iv = b.CreateInsertValue(iv, v, {0});
            iv = b.CreateInsertValue(iv, d, {1});
            return iv;
        }
        if (from->kind == TypeKind::Enum && to->kind == TypeKind::Prim)
            return convertTo(v, Type::make_prim("int"), to, line, col);

        if (is_integer(from) && is_integer(to)) {
            if (sizeOf(from) < sizeOf(to))
                return from && from->is_signed() ? b.CreateSExt(v, tt) : b.CreateZExt(v, tt);
            if (sizeOf(from) > sizeOf(to)) return b.CreateTrunc(v, tt);
            return v;
        }
        if (is_numeric(from) && is_numeric(to)) {
            if (from->kind == TypeKind::Prim && from->is_float() &&
                to->kind == TypeKind::Prim && to->is_float()) {
                return sizeOf(from) < sizeOf(to) ? b.CreateFPExt(v, tt) : b.CreateFPTrunc(v, tt);
            }
            if (from->kind == TypeKind::Prim && from->is_float()) {
                return from->is_signed() ? b.CreateFPToSI(v, tt) : b.CreateFPToUI(v, tt);
            }
            return to->kind == TypeKind::Prim && to->is_float()
                       ? (from->is_signed() ? b.CreateSIToFP(v, tt) : b.CreateUIToFP(v, tt))
                       : v;
        }
        if (from->kind == TypeKind::Ptr && is_integer(to)) return b.CreatePtrToInt(v, tt);
        if (is_integer(from) && to->kind == TypeKind::Ptr) return b.CreateIntToPtr(v, tt);
        if (from->kind == TypeKind::Ptr && to->kind == TypeKind::Ptr) return v;
        fail("LLVM backend: cannot convert " + from->str() + " -> " + to->str(), line, col);
    }

    bool Emitter::isLvalue(Expr* e) {
        if (dynamic_cast<Name*>(e)) return true;
        if (dynamic_cast<Member*>(e)) return true;
        if (dynamic_cast<Index*>(e)) return true;
        if (auto u = dynamic_cast<Unary*>(e)) return u->op == "*";
        return false;
    }

    // whether the address of this expression may fail to satisfy its type's natural alignment (a field/array element on a packed struct chain).
    // Conservative decision: when it cannot be determined, treat it as naturally aligned (in the spec, dereferencing `&p.field` is itself UB).
    bool Emitter::unalignedExpr(Expr* e) {
        if (!e) return false;
        if (auto m = dynamic_cast<Member*>(e)) {
            TypePtr ot = m->obj->ty;
            if (ot && ot->kind == TypeKind::Struct) {
                if (ot->is_packed) return true;
                return unalignedExpr(m->obj.get());
            }
            if (ot && ot->kind == TypeKind::Class && m->field_index >= 0 &&
                m->field_index < static_cast<int>(ot->fields.size())) {
                // class fields are laid out per the model's alignment; if the field still lands on an offset that does not satisfy its own alignment (which should not happen in theory),
                // conservatively treat it as unaligned (for example member access through a packed struct field).
                uint64_t fa = alignOf(ot->fields[m->field_index].type);
                if (fa > 1 && classFieldOffset(ot, m->field_index) % fa != 0) return true;
                return unalignedExpr(m->obj.get());
            }
            return false;
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            TypePtr bt = ix->base->ty;
            if (!bt) return false;
            if (bt->kind == TypeKind::Array || bt->kind == TypeKind::Ptr) {
                TypePtr el = bt->elem;
                if (el && el->kind == TypeKind::Struct && el->is_packed) return true;
                return unalignedExpr(ix->base.get());
            }
            if (bt->kind == TypeKind::Slice) {
                TypePtr el = bt->elem;
                return el && el->kind == TypeKind::Struct && el->is_packed;
            }
            return false;
        }
        if (auto n = dynamic_cast<Name*>(e)) {
            // direct field access inside a struct method: the packed struct pointed to by this may be unaligned
            if (n->kind == "field" && n->implicit_this) {
                return curClassTy && curClassTy->kind == TypeKind::Struct && curClassTy->is_packed;
            }
            if (n->ident == "this") {
                return curClassTy && curClassTy->kind == TypeKind::Struct && curClassTy->is_packed;
            }
            return false;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op == "*" || u->op == "&") return unalignedExpr(u->operand.get());
            return false;
        }
        return false;
    }

    // Checked GEP: turn LLVM's assertion errors into readable diagnostics.
    // Note: index 0 is the "pointer stride"; indexing by aggregate type starts at index 1.
    Value* Emitter::gep(LType* ty, Value* ptr, std::initializer_list<Value*> idxs, int line,
                        int col) {
        LType* cur = ty;
        size_t i = 0;
        for (auto it = idxs.begin(); it != idxs.end(); ++it, ++i) {
            if (i == 0) continue;
            if (auto* st = dyn_cast<LStruct>(cur)) {
                auto* ci = dyn_cast<ConstantInt>(*it);
                if (!ci) fail("LLVM backend: struct index must be a constant", line, col);
                unsigned f = static_cast<unsigned>(ci->getZExtValue());
                if (f >= st->getNumElements())
                    fail("LLVM backend: struct field index out of bounds (" + std::to_string(f) + "/" +
                             std::to_string(st->getNumElements()) + "）",
                         line, col);
                cur = st->getElementType(f);
            } else if (auto* at = dyn_cast<LArray>(cur)) {
                cur = at->getElementType();
            } else {
                fail("LLVM backend: too many GEP indices (base type is not an aggregate)", line, col);
            }
        }
        return b.CreateGEP(ty, ptr, idxs);
    }

    Value* Emitter::gep(LType* ty, Value* ptr, Value* idx, int line, int col) {
        return gep(ty, ptr, {idx}, line, col);
    }

    Value* Emitter::addrOf(Expr* e) {
        if (auto n = dynamic_cast<Name*>(e)) {
            if (n->ident == "this") {
                // `this` is a reference to the current object: for a struct the receiver is itself the object address,
                // for class/interface the receiver is a handle (a value), so locating the handle slot is enough.
                if (curClassTy && curClassTy->kind == TypeKind::Struct)
                    return b.CreateLoad(ptrTy(), thisSlot);
                return thisSlot;
            }
            if (n->kind == "field" && n->implicit_this) {
                if (!curClassTy || !thisSlot) fail("LLVM backend: implicit field access is missing `this`", n->line, n->col);
                return fieldAddr(b.CreateLoad(ptrTy(), thisSlot), curClassTy, n->field_index);
            }
            if (auto lv = std::get_if<LocalVar*>(&n->sym)) {
                AllocaInst* a = slotFor(*lv);
                if ((*lv)->by_ref) return b.CreateLoad(ptrTy(), a);
                return a;
            }
            if (auto gv = std::get_if<GlobalVar*>(&n->sym)) {
                return mod->getOrInsertGlobal((*gv)->cname, lt((*gv)->rty));
            }
            fail("LLVM backend: name `" + n->ident + "` is not addressable", n->line, n->col);
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            if (m->kind == "global") {
                auto* gv = std::get_if<GlobalVar*>(&m->sym);
                if (gv) return mod->getOrInsertGlobal((*gv)->cname, lt((*gv)->rty));
            }
            TypePtr ot = m->obj->ty;
            if (ot->kind == TypeKind::Class) {
                Value* p = eval(m->obj.get());
                return fieldAddr(p, ot, m->field_index);
            }
            Value* base = addrOf(m->obj.get());
            if (ot->kind != TypeKind::Struct)
                fail("LLVM backend: base type of member access is not a struct", m->line, m->col);
            auto* st = cast<LStruct>(lt(ot));
            return fieldSlot(st, ot, base, m->field_index);
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            if (ix->by_ref) {
                // `operator[]` returns a borrow: the call result is the element address
                if (ix->get_method) {
                    Value* r = emitMemberIndex(ix->get_method, addrOf(ix->base.get()),
                                               convertTo(eval(ix->indices[0].get()), ix->indices[0]->ty,
                                                         Type::make_prim("usize")));
                    return r;
                }
                return emitUserCall(ix->get_func, {ix->base.get(), ix->indices[0].get()});
            }
            TypePtr bt = ix->base->ty;
            std::vector<Value*> idxs;
            for (auto& ie : ix->indices) {
                idxs.push_back(convertTo(eval(ie.get()), ie->ty,
                                         Type::make_prim("usize"), ix->line, ix->col));
            }
            if (bt->kind == TypeKind::Array) {
                Value* addr = addrOf(ix->base.get());
                TypePtr curTy = bt;
                for (size_t k = 0; k < idxs.size(); ++k) {
                    auto* st = cast<LStruct>(lt(curTy));
                    addr = gep(st, addr, {ConstantInt::get(i64(), 0),
                                          ConstantInt::get(i32(), 0), idxs[k]});
                    curTy = curTy->elem;
                }
                return addr;
            }
            if (bt->kind == TypeKind::Slice) {
                Value* h = eval(ix->base.get());
                trackOwnedSliceTemp(ix->base.get(), h, bt);
                Value* lin = idxs.empty() ? ConstantInt::get(i64(), 0) : idxs[0];
                for (size_t k = 1; k < idxs.size(); ++k) {
                    Value* dk = sliceDimField(h, static_cast<int>(k));
                    lin = b.CreateAdd(b.CreateMul(lin, dk), idxs[k]);
                }
                Value* data = sliceDataPtr(h, bt->rank);
                return b.CreateGEP(lt(bt->elem), data, lin);
            }
            if (bt->kind == TypeKind::Ptr) {
                if (idxs.size() != 1) fail("LLVM backend: wrong number of pointer indices", ix->line, ix->col);
                return gep(lt(bt->elem), eval(ix->base.get()), idxs[0]);
            }
            fail("LLVM backend: type is not indexable", ix->line, ix->col);
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op == "*") return eval(u->operand.get());
        }
        if (auto c = dynamic_cast<Call*>(e)) {
            if (c->by_ref) {
                // `ref T f()`: the call result is the borrowed address
                return evalCall(c);
            }
        }
        // other expressions: evaluate into a temporary first
        Value* v = eval(e);
        auto* tmp = entryAlloca(v->getType(), "tmp", e->ty ? alignOf(e->ty) : 0);
        b.CreateStore(v, tmp);
        // after an owned struct/array rvalue is materialized, its reference fields are released at the end of the statement
        if (e->ty && needsCleanup(e->ty) && valueOwned(e) &&
            (e->ty->kind == TypeKind::Struct || e->ty->kind == TypeKind::Array ||
             e->ty->kind == TypeKind::Slice)) {
            stmtTemps.emplace_back(tmp, e->ty);
        }
        return tmp;
    }

    Value* Emitter::compareOp(const std::string& op, Value* l, Value* r, bool is_signed, bool is_float) {
        if (is_float) {
            if (op == "==") return b.CreateFCmpOEQ(l, r);
            if (op == "!=") return b.CreateFCmpUNE(l, r);
            if (op == "<") return b.CreateFCmpOLT(l, r);
            if (op == "<=") return b.CreateFCmpOLE(l, r);
            if (op == ">") return b.CreateFCmpOGT(l, r);
            return b.CreateFCmpOGE(l, r);
        }
        if (op == "==") return b.CreateICmpEQ(l, r);
        if (op == "!=") return b.CreateICmpNE(l, r);
        if (op == "<") return is_signed ? b.CreateICmpSLT(l, r) : b.CreateICmpULT(l, r);
        if (op == "<=") return is_signed ? b.CreateICmpSLE(l, r) : b.CreateICmpULE(l, r);
        if (op == ">") return is_signed ? b.CreateICmpSGT(l, r) : b.CreateICmpUGT(l, r);
        return is_signed ? b.CreateICmpSGE(l, r) : b.CreateICmpUGE(l, r);
    }

    Value* Emitter::coerce(Expr* e, const TypePtr& to) {
        if (e && e->conv) {
            if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm]       implicit conv " << e->conv->cname << "\n";
            // Implicit struct construction (3.6): materialize the value through its `implicit` constructor.
            // The constructor takes the source value, so the argument is evaluated as its own type.
            const ImplicitConv& c = *e->conv;
            const TypePtr& st = c.to;
            auto* tmp = entryAlloca(lt(st), "conv.tmp", alignOf(st));
            if (st->needs_cleanup()) zeroMem(tmp, st);
            std::vector<LType*> ptys{ptrTy()};
            for (auto& t : c.pty) ptys.push_back(lt(t));
            FunctionCallee callee =
                mod->getOrInsertFunction(c.cname, FunctionType::get(LType::getVoidTy(ctx), ptys, false));
            const TypePtr& pty0 = c.pty[0];
            Value* av = convertTo(eval(e), e->ty, pty0);
            ArgPack pack;
            if (needsCleanup(pty0) && convOwns(e, pty0)) pack.owned.emplace_back(av, pty0);
            b.CreateCall(callee, {tmp, av});
            releaseTemps(pack);
            if (mayThrow.mayThrowCname(c.cname)) {
                emitCheckPendingWith([&] { emitReleaseValue(tmp, st); });
            }
            return b.CreateLoad(lt(st), tmp);
        }
        if (e && to && to->kind == TypeKind::Slice && e->ty &&
            e->ty->kind == TypeKind::Array) {
            // fixed-size array -> dynamic array: allocate and copy by nested dimension (no borrowed view is provided anymore)
            return emitDynArrayFromArray(addrOf(e), e->ty, to);
        }
        return convertTo(eval(e), e->ty, to);
    }

    Value* Emitter::evalBinary(Binary* e) {
        const std::string& op = e->op;
        TypePtr lt_ = e->left->ty;
        TypePtr rt_ = e->right->ty;

        if (e->op_method) {
            Value* r = emitMemberOp(e->op_method, e->left.get(), {e->right.get()});
            return e->negate_result ? b.CreateNot(r) : r;
        }
        if (e->op_func) {
            Value* r = emitUserCall(e->op_func, {e->left.get(), e->right.get()});
            return e->negate_result ? b.CreateNot(r) : r;
        }

        if (op == "string_eq_helper") return nullptr;

        if (op == "and" || op == "or") {
            Value* l = eval(e->left.get());
            BasicBlock* rhsBB = BasicBlock::Create(ctx, "sc.rhs", curFn);
            BasicBlock* endBB = BasicBlock::Create(ctx, "sc.end", curFn);
            BasicBlock* lhsEnd = b.GetInsertBlock();
            if (op == "and")
                b.CreateCondBr(l, rhsBB, endBB);
            else
                b.CreateCondBr(l, endBB, rhsBB);
            b.SetInsertPoint(rhsBB);
            Value* r = eval(e->right.get());
            BasicBlock* rhsEnd = b.GetInsertBlock();
            if (!hasTerm(rhsEnd)) b.CreateBr(endBB);
            b.SetInsertPoint(endBB);
            PHINode* phi = b.CreatePHI(i1(), 2);
            phi->addIncoming(ConstantInt::get(i1(), op == "and" ? 0 : 1), lhsEnd);
            phi->addIncoming(r, rhsEnd);
            return phi;
        }

        // the operands are evaluated only once (all later branches reuse them)
        Value* l = eval(e->left.get());
        Value* r = eval(e->right.get());

        // pointer arithmetic
        if ((op == "+" || op == "-") && lt_->kind == TypeKind::Ptr && is_integer(rt_)) {
            Value* n = convertTo(r, rt_, Type::make_prim("isize"));
            return gep(lt(lt_->elem), l, n);
        }
        if (op == "+" && is_integer(lt_) && rt_->kind == TypeKind::Ptr) {
            Value* n = convertTo(l, lt_, Type::make_prim("isize"));
            return gep(lt(rt_->elem), r, n);
        }
        if (op == "-" && lt_->kind == TypeKind::Ptr && rt_->kind == TypeKind::Ptr) {
            Value* a = b.CreatePtrToInt(l, i64());
            Value* c = b.CreatePtrToInt(r, i64());
            Value* d = b.CreateSub(a, c);
            uint64_t sz = sizeOf(lt_->elem);
            return b.CreateSDiv(d, ConstantInt::get(i64(), sz ? sz : 1));
        }

        if (op == "==" || op == "!=") {
            bool lref = isRefKind(lt_) || lt_->kind == TypeKind::Null;
            bool rref = isRefKind(rt_) || rt_->kind == TypeKind::Null;
            if (lref && rref && (isRefKind(lt_) || isRefKind(rt_))) {
                Value* lobj = (isRefKind(lt_) && lt_->kind != TypeKind::Null) ? refObj(l, lt_) : l;
                Value* robj = (isRefKind(rt_) && rt_->kind != TypeKind::Null) ? refObj(r, rt_) : r;
                if (lt_->kind == TypeKind::Null) lobj = Constant::getNullValue(ptrTy());
                if (rt_->kind == TypeKind::Null) robj = Constant::getNullValue(ptrTy());
                Value* cmp = compareOp(op, lobj, robj, false, false);
                // release the temporary owned references after the comparison (new / a call returning a reference)
                if (ownsReference(e->left.get())) emitReleaseRef(l, lt_);
                if (ownsReference(e->right.get())) emitReleaseRef(r, rt_);
                return cmp;
            }
        }
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (lt_->kind == TypeKind::Ptr || rt_->kind == TypeKind::Ptr ||
                lt_->kind == TypeKind::Func || rt_->kind == TypeKind::Func) {
                Value* lv = l;
                Value* rv = r;
                if (lt_->kind == TypeKind::Null) lv = Constant::getNullValue(ptrTy());
                if (rt_->kind == TypeKind::Null) rv = Constant::getNullValue(ptrTy());
                return compareOp(op, lv, rv, false, false);
            }
            if (same_type(lt_, rt_) && lt_->kind == TypeKind::Prim && lt_->is_bool()) {
                return compareOp(op, l, r, false, false);
            }
            // enum: only compared with the same enum, using the signed order of the underlying int (SPEC §3.5)
            if (lt_->kind == TypeKind::Enum && rt_->kind == TypeKind::Enum &&
                same_type(lt_, rt_)) {
                l = convertTo(l, lt_, Type::make_prim("int"));
                r = convertTo(r, rt_, Type::make_prim("int"));
                return compareOp(op, l, r, true, false);
            }
            TypePtr common = usual_arith(lt_, rt_);
            if (!common) fail("LLVM backend: comparison operands have different types", e->line, e->col);
            l = convertTo(l, lt_, common);
            r = convertTo(r, rt_, common);
            bool fl = common->kind == TypeKind::Prim && common->is_float();
            bool sg = common->kind == TypeKind::Prim && common->is_signed();
            return compareOp(op, l, r, sg, fl);
        }

        if (op == "<<" || op == ">>") {
            l = convertTo(l, lt_, e->ty);
            r = convertTo(r, rt_, e->ty);
            return op == "<<" ? b.CreateShl(l, r) : b.CreateAShr(l, r);
        }

        TypePtr res = e->ty;
        l = convertTo(l, lt_, res);
        r = convertTo(r, rt_, res);
        bool fl = res->kind == TypeKind::Prim && res->is_float();
        bool sg = res->kind == TypeKind::Prim && res->is_signed();
        if (op == "+") return fl ? b.CreateFAdd(l, r) : b.CreateAdd(l, r);
        if (op == "-") return fl ? b.CreateFSub(l, r) : b.CreateSub(l, r);
        if (op == "*") return fl ? b.CreateFMul(l, r) : b.CreateMul(l, r);
        if (op == "/") return fl ? b.CreateFDiv(l, r) : (sg ? b.CreateSDiv(l, r) : b.CreateUDiv(l, r));
        if (op == "%") return fl ? b.CreateFRem(l, r) : (sg ? b.CreateSRem(l, r) : b.CreateURem(l, r));
        if (op == "&") return b.CreateAnd(l, r);
        if (op == "|") return b.CreateOr(l, r);
        if (op == "^") return b.CreateXor(l, r);
        fail("LLVM backend: unknown binary operator " + op, e->line, e->col);
    }

    // Conditional expression: only the selected branch is evaluated; the result is materialized into the entry block's result slot and returned as an owned value.
    Value* Emitter::evalTernary(Ternary* e) {
        Value* cond = eval(e->cond.get());
        BasicBlock* thenBB = BasicBlock::Create(ctx, "tern.then", curFn);
        BasicBlock* elseBB = BasicBlock::Create(ctx, "tern.else", curFn);
        BasicBlock* endBB = BasicBlock::Create(ctx, "tern.end", curFn);
        b.CreateCondBr(cond, thenBB, elseBB);

        const TypePtr& ty = e->ty;
        AllocaInst* res = is_void(ty) ? nullptr : entryAlloca(lt(ty), "tern.res", alignOf(ty));

        b.SetInsertPoint(thenBB);
        emitTernaryArm(res, e->then_e.get(), ty);
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);

        b.SetInsertPoint(elseBB);
        emitTernaryArm(res, e->else_e.get(), ty);
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);

        b.SetInsertPoint(endBB);
        if (!res) return nullptr;
        return loadAt(lt(ty), res);
    }

    // Write the branch value into the result slot: a borrowed source is retained field by field, an owned source transfers ownership directly (consistent with initInto)
    void Emitter::emitTernaryArm(Value* addr, Expr* src, const TypePtr& ty) {
        if (!addr) {  // void branch: only evaluate
            eval(src);
            return;
        }
        Value* v = coerce(src, ty);
        if (isRefKind(ty)) {
            if (!ownsReference(src)) emitRetainRef(v, ty);
            storeValue(v, addr, ty);
            return;
        }
        if ((ty->kind == TypeKind::Array || ty->kind == TypeKind::Struct) &&
            needsCleanup(ty)) {
            storeValue(v, addr, ty);
            if (!valueOwned(src)) emitRetainValue(addr, ty);
            return;
        }
        if (ty->kind == TypeKind::Slice) {
            storeValue(v, addr, ty);
            if (!convOwns(src, ty)) emitRetainValue(addr, ty);
            return;
        }
        storeValue(v, addr, ty);
    }


    ArgPack Emitter::prepareArgs(const std::vector<Expr*>& args, const std::vector<TypePtr>& ptys) {
        ArgPack pack;
        for (size_t i = 0; i < args.size() && i < ptys.size(); ++i) {
            Value* v = coerce(args[i], ptys[i]);
            // parameters are passed as borrows (the callee does not release them); only argument temporaries whose ownership the caller holds need cleanup after the call
            if (needsCleanup(ptys[i]) && convOwns(args[i], ptys[i])) {
                pack.owned.emplace_back(v, ptys[i]);
            }
            pack.vals.push_back(v);
        }
        return pack;
    }

    // C default argument promotion: integers smaller than int -> int, float -> double, everything else unchanged (required by the C varargs ABI)
    Value* Emitter::promoteVararg(Value* v, const TypePtr& t) {
        if (!t) return v;
        if (t->kind == TypeKind::Prim) {
            const std::string& n = t->name;
            if (n == "float") return b.CreateFPExt(v, LType::getDoubleTy(ctx));
            if (n == "bool" || n == "byte" || n == "char" || n == "ushort") {
                return b.CreateZExt(v, i32());
            }
            if (n == "short") return b.CreateSExt(v, i32());
        }
        return v;
    }

    void Emitter::releaseTemps(ArgPack& pack) {
        for (auto& [v, ty] : pack.owned) {
            if (isRefKind(ty)) {
                emitReleaseRef(v, ty);
            } else if (ty && needsCleanup(ty)) {
                auto* tmp = entryAlloca(lt(ty), "arg.tmp", alignOf(ty));
                b.CreateStore(v, tmp);
                emitReleaseValue(tmp, ty);
            }
        }
        pack.owned.clear();
    }

}  // namespace muc_detail
