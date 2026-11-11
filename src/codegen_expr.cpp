// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 本文件包含表达式代码生成（转换、地址、二元/三元、参数准备）（Emitter 成员函数定义）。

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


    // ------------------------------------------------------------ 表达式
    Value* Emitter::convertTo(Value* v, const TypePtr& from, const TypePtr& to, int line,
                              int col) {
        if (!from || !to || same_type(from, to)) return v;
        LType* ft = v->getType();
        LType* tt = lt(to);
        if (ft == tt) return v;

        if (isStrType(from) && to->kind == TypeKind::Ptr)
            return b.CreateExtractValue(v, {0});
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

    // 该表达式的地址是否可能未满足其类型的自然对齐（packed 结构链上的字段/数组元素）。
    // 保守判定：取不到时按自然对齐处理（规范里 `&p.field` 的解引用本身就是 UB）。
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
                // class 字段按模型对齐排布；若该字段仍落在未满足自身对齐的偏移上（理论上不该发生），
                // 保守按未对齐处理（例如含 packed 结构体字段时其成员访问）。
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
            // struct 方法内直接访问字段：this 指向的 packed 结构可能未对齐
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

    // 带校验的 GEP：把 LLVM 的断言错误变成可读的诊断。
    // 注意：第 0 个下标是"指针步进"，从第 1 个下标起才按聚合类型逐层索引。
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
                // `this` 是当前对象的引用：struct 的接收者本身是对象地址，
                // class/interface 的接收者是句柄（值），定位到句柄槽即可。
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
            TypePtr bt = ix->base->ty;
            std::vector<Value*> idxs;
            for (auto& ie : ix->indices) {
                idxs.push_back(convertTo(eval(ie.get()), ie->ty,
                                         Type::make_prim("usize"), ix->line, ix->col));
            }
            if (isStrType(bt)) {
                if (idxs.size() != 1) fail("LLVM backend: `string` accepts exactly one index", ix->line, ix->col);
                Value* sv = evalStrValue(ix->base.get());
                Value* p = b.CreateExtractValue(sv, {0});
                return b.CreateGEP(i8(), p, idxs[0]);
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
            if (bt->kind == TypeKind::String) {
                Value* sv = eval(ix->base.get());
                return gep(i8(), b.CreateExtractValue(sv, {0}), idxs[0]);
            }
            fail("LLVM backend: type is not indexable", ix->line, ix->col);
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op == "*") return eval(u->operand.get());
        }
        // 其它表达式：先求值到临时量
        Value* v = eval(e);
        auto* tmp = entryAlloca(v->getType(), "tmp", e->ty ? alignOf(e->ty) : 0);
        b.CreateStore(v, tmp);
        // owned 的 struct/array 右值被物化后，语句结束时要释放其引用字段
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
        if (e && to && to->kind == TypeKind::Slice && e->ty &&
            e->ty->kind == TypeKind::Array) {
            // 定长数组 -> 动态数组：按嵌套维度分配并复制（不再提供借用视图）
            return emitDynArrayFromArray(addrOf(e), e->ty, to);
        }
        return convertTo(eval(e), e->ty, to);
    }

    Value* Emitter::evalBinary(Binary* e) {
        const std::string& op = e->op;
        TypePtr lt_ = e->left->ty;
        TypePtr rt_ = e->right->ty;

        if (e->op_func) {
            return emitUserCall(e->op_func, {e->left.get(), e->right.get()});
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

        // string == / !=
        if ((op == "==" || op == "!=") && isStrType(lt_) && isStrType(rt_)) {
            Value* l = evalStrValue(e->left.get());
            Value* r = evalStrValue(e->right.get());
            Value* lp = b.CreateExtractValue(l, {0});
            Value* ll = b.CreateExtractValue(l, {1});
            Value* rp = b.CreateExtractValue(r, {0});
            Value* rl = b.CreateExtractValue(r, {1});
            auto memcmpFn = libFn("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
            Value* cmp = b.CreateCall(memcmpFn, {lp, rp, b.CreateSelect(
                                                            b.CreateICmpULT(ll, rl), ll, rl)});
            Value* eq = b.CreateAnd(b.CreateICmpEQ(ll, rl), b.CreateICmpEQ(cmp, ConstantInt::get(i32(), 0)));
            return op == "==" ? eq : b.CreateNot(eq);
        }

        // 操作数只求值一次（后面所有分支复用）
        Value* l = eval(e->left.get());
        Value* r = eval(e->right.get());

        // 指针算术
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
                // 比较后释放临时的 owned 引用（new / 返回引用的调用）
                if (ownsReference(e->left.get())) emitReleaseRef(l, lt_);
                if (ownsReference(e->right.get())) emitReleaseRef(r, rt_);
                return cmp;
            }
        }
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (lt_->kind == TypeKind::Ptr || rt_->kind == TypeKind::Ptr) {
                Value* lv = l;
                Value* rv = r;
                if (lt_->kind == TypeKind::Null) lv = Constant::getNullValue(ptrTy());
                if (rt_->kind == TypeKind::Null) rv = Constant::getNullValue(ptrTy());
                return compareOp(op, lv, rv, false, false);
            }
            if (same_type(lt_, rt_) && lt_->kind == TypeKind::Prim && lt_->is_bool()) {
                return compareOp(op, l, r, false, false);
            }
            // 枚举：只与同一枚举比较，按底层 int 的有符号序比较（SPEC §3.5）
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

    // 条件表达式：只求值被选中的分支，结果物化到 entry 块的结果槽后按 owned 值返回。
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

    // 把分支值写入结果槽：借用源逐字段 retain，owned 源直接转移所有权（与 initInto 一致）
    void Emitter::emitTernaryArm(Value* addr, Expr* src, const TypePtr& ty) {
        if (!addr) {  // void 分支：只求值
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
            // 参数按借用传递（callee 不释放）；只有调用者持有所有权的实参临时值才需在调用后清理
            if (needsCleanup(ptys[i]) && convOwns(args[i], ptys[i])) {
                pack.owned.emplace_back(v, ptys[i]);
            }
            pack.vals.push_back(v);
        }
        return pack;
    }

    // C 默认实参提升：比 int 小的整型 → int，float → double，其余原样（C varargs ABI 要求）
    Value* Emitter::promoteVararg(Value* v, const TypePtr& t) {
        if (!t) return v;
        if (isStrType(t)) {
            // `string` 视图按 C 语义传底层 `const char*`（printf 的 %s）
            return b.CreateExtractValue(v, {0});
        }
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
