// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 本文件包含表达式代码生成（调用、new、向下转换、eval 与 sizeof 类型解析）（Emitter 成员函数定义）。

#include "codegen_impl.hpp"

namespace muc_detail {

using ArgPack = Emitter::ArgPack;

    // 方法调用（recv 约定：class = 句柄值；struct = 对象地址）
    Value* Emitter::emitMethodCallVals(const MethodSig* sig, Value* recv, const TypePtr& recvTy,
                                       const std::vector<Value*>& args, Node* at) {
        std::vector<LType*> ptys{ptrTy()};
        for (auto& t : sig->params) ptys.push_back(lt(t));
        auto* fnTy = FunctionType::get(lt(sig->ret), ptys, false);

        bool ifaceCall = (recvTy && recvTy->kind == TypeKind::Interface) ||
                         (sig->owner && sig->owner->kind == TypeKind::Interface);
        if (ifaceCall) {
            auto it = islotIndex.find(sigKey(*sig));
            if (it == islotIndex.end()) {
                fail("LLVM backend: interface method is missing from the itable", at->line, at->col);
            }
            int slot = it->second;
            Value* obj = nullptr;
            Value* dsc = nullptr;
            if (recvTy && recvTy->kind == TypeKind::Interface) {
                obj = b.CreateExtractValue(recv, {0});
                dsc = b.CreateExtractValue(recv, {1});
            } else {
                // 类变量上调用接口默认方法：现场构造接口值
                obj = recv;
                Value* dpp = b.CreateGEP(i8(), recv, ConstantInt::get(i64(), 8));
                dsc = b.CreateLoad(ptrTy(), dpp);
            }
            Value* itp = b.CreateGEP(i8(), dsc, ConstantInt::get(i64(), 24));
            Value* itab = b.CreateLoad(ptrTy(), itp);
            Value* fnp = b.CreateGEP(ptrTy(), itab, ConstantInt::get(i64(), slot));
            Value* fn = b.CreateLoad(ptrTy(), fnp);
            std::vector<LType*> ip{ptrTy(), ptrTy()};
            for (auto& t : sig->params) ip.push_back(lt(t));
            auto* ifnTy = FunctionType::get(lt(sig->ret), ip, false);
            std::vector<Value*> iargs{obj, dsc};
            iargs.insert(iargs.end(), args.begin(), args.end());
            return b.CreateCall(ifnTy, fn, iargs);
        }

        int slot = slotOf(recvTy, sig);
        if (slot >= 0) {
            Value* dp = b.CreateGEP(i8(), recv, ConstantInt::get(i64(), 8));
            Value* desc = b.CreateLoad(ptrTy(), dp);
            Value* vtp = b.CreateGEP(i8(), desc, ConstantInt::get(i64(), 16));
            Value* vt = b.CreateLoad(ptrTy(), vtp);
            Value* fnp = b.CreateGEP(ptrTy(), vt, ConstantInt::get(i64(), slot));
            Value* fn = b.CreateLoad(ptrTy(), fnp);
            std::vector<Value*> vals{recv};
            vals.insert(vals.end(), args.begin(), args.end());
            return b.CreateCall(fnTy, fn, vals);
        }
        // 未进虚表：静态调用声明处实现
        FunctionCallee callee = methodFn(sig);
        std::vector<Value*> vals{recv};
        vals.insert(vals.end(), args.begin(), args.end());
        return b.CreateCall(callee, vals);
    }

    // 用户 operator[]（自由函数）：容器按 class 句柄 / struct 地址 / struct 值传参
    Value* Emitter::emitOperatorIndexCall(FuncDecl* f, Value* seqAddr, const TypePtr& sty, Value* idxVal,
                                          bool unaligned) {
        std::vector<LType*> ptys;
        for (size_t i = 0; i < f->pty.size(); ++i)
            ptys.push_back(i < f->pty_ref.size() && f->pty_ref[i] ? ptrTy() : lt(f->pty[i]));
        FunctionCallee callee =
            mod->getOrInsertFunction(f->cname, FunctionType::get(lt(f->rty), ptys, false));
        Value* cArg = nullptr;
        const bool recv_ref = !f->pty_ref.empty() && f->pty_ref[0];
        if (recv_ref) {
            cArg = seqAddr;  // 借用接收者：直接把容器地址传进去
        } else if (sty->kind == TypeKind::Class) {
            cArg = b.CreateLoad(lt(sty), seqAddr);
        } else if (!f->pty.empty() && f->pty[0] && f->pty[0]->kind == TypeKind::Ptr) {
            cArg = seqAddr;
        } else {
            cArg = loadAt(lt(sty), seqAddr, unaligned);
        }
        return b.CreateCall(callee, {cArg, idxVal});
    }

    // 调用自由函数/运算符/方法（静态派发）
    Value* Emitter::emitUserCall(FuncDecl* f, const std::vector<Expr*>& args) {
        std::vector<LType*> ptys;
        for (size_t i = 0; i < f->pty.size(); ++i)
            ptys.push_back(i < f->pty_ref.size() && f->pty_ref[i] ? ptrTy() : lt(f->pty[i]));
        FunctionCallee callee =
            mod->getOrInsertFunction(f->cname, FunctionType::get(lt(f->rty), ptys, f->is_varargs));
        ArgPack pack;
        for (size_t i = 0; i < args.size() && i < f->pty.size(); ++i) {
            if (i < f->pty_ref.size() && f->pty_ref[i]) {
                pack.vals.push_back(addrOf(args[i]));  // 借用：callee 不拥有，调用后不清理
                continue;
            }
            Value* v = coerce(args[i], f->pty[i]);
            if (needsCleanup(f->pty[i]) && convOwns(args[i], f->pty[i])) {
                pack.owned.emplace_back(v, f->pty[i]);
            }
            pack.vals.push_back(v);
        }
        if (f->is_varargs) {
            for (size_t i = f->pty.size(); i < args.size(); ++i) {
                pack.vals.push_back(promoteVararg(eval(args[i]), args[i]->ty));
            }
        }
        Value* r = b.CreateCall(callee, pack.vals);
        releaseTemps(pack);
        return r;
    }

    Value* Emitter::evalNew(NewExpr* ne) {
        if (ne->is_array) {
            std::vector<Value*> dims;
            for (auto& d : ne->dims) {
                dims.push_back(convertTo(eval(d.get()), d->ty, Type::make_prim("usize")));
            }
            return emitSliceAlloc(dims, ne->elem_ty);
        }
        TypePtr cls = ne->class_ty;
        uint64_t sz = objectSize(cls);
        Value* p = nullptr;
        if (classNeedsAlignedAlloc(cls)) {
            // _aligned_malloc(size, alignment)：对象对齐超过 malloc 的 16 字节保证（对应 _aligned_free）
            p = b.CreateCall(libFn("_aligned_malloc", ptrTy(), {i64(), i64()}),
                             {ConstantInt::get(i64(), sz),
                              ConstantInt::get(i64(), objectAlign(cls))});
        } else {
            p = b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {ConstantInt::get(i64(), sz)});
        }
        // 分配失败直接 abort（语言无异常）
        BasicBlock* okBB = BasicBlock::Create(ctx, "new.ok", curFn);
        BasicBlock* failBB = BasicBlock::Create(ctx, "new.fail", curFn);
        b.CreateCondBr(b.CreateICmpEQ(p, Constant::getNullValue(ptrTy())), failBB, okBB);
        b.SetInsertPoint(failBB);
        b.CreateCall(libFn("abort", LType::getVoidTy(ctx), {}), {});
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
        b.CreateMemSet(p, ConstantInt::get(i8(), 0), ConstantInt::get(i64(), sz),
                       MaybeAlign(objectAlign(cls)));
        // rc = 1
        Value* rcp = b.CreateGEP(i8(), p, ConstantInt::get(i64(), 0));
        b.CreateStore(ConstantInt::get(i64(), 1), rcp);
        auto storeDesc = [&]() {
            Value* dp = b.CreateGEP(i8(), p, ConstantInt::get(i64(), 8));
            return b.CreateStore(descOf(cls), dp);
        };
        storeDesc();

        if (ne->has_ctor) {
            std::vector<LType*> ptys{ptrTy()};
            for (auto& t : ne->ctor_pty) ptys.push_back(lt(t));
            FunctionCallee callee = mod->getOrInsertFunction(
                ctor_cname(cls->unit, cls->name, ne->ctor_pty),
                FunctionType::get(LType::getVoidTy(ctx), ptys, false));
            std::vector<Expr*> rawArgs;
            for (auto& a : ne->args) rawArgs.push_back(a.get());
            ArgPack pack = prepareArgs(rawArgs, ne->ctor_pty);
            std::vector<Value*> vals{p};
            for (auto* v : pack.vals) vals.push_back(v);
            b.CreateCall(callee, vals);
            releaseTemps(pack);
        } else {
            // 隐式默认构造：调用最近祖先声明的无参构造器（它会继续链式调用自己的基类）
            TypePtr b0 = cls->base;
            while (b0 && b0->kind == TypeKind::Class) {
                if (!b0->ctors.empty()) {
                    bool hasDefault = false;
                    for (auto& cs : b0->ctors) {
                        if (cs.empty()) { hasDefault = true; break; }
                    }
                    if (hasDefault) {
                        FunctionCallee callee = mod->getOrInsertFunction(
                            ctor_cname(b0->unit, b0->name, {}),
                            FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                        b.CreateCall(callee, {p});
                    }
                    break;
                }
                b0 = b0->base;
            }
            storeDesc();  // 祖先构造器可能改写了 desc，恢复为最终类型
        }
        return p;
    }

    Value* Emitter::evalCall(Call* c) {
        if (c->callee_kind == "builtin") {
            if (c->builtin == "alloc") {
                Value* n = convertTo(eval(c->args[0].get()), c->args[0]->ty,
                                     Type::make_prim("usize"));
                return b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {n});
            }
            if (c->builtin == "realloc") {
                Value* p = eval(c->args[0].get());
                Value* n = convertTo(eval(c->args[1].get()), c->args[1]->ty,
                                     Type::make_prim("usize"));
                return b.CreateCall(libFn("realloc", ptrTy(), {ptrTy(), i64()}), {p, n});
            }
            if (c->builtin == "free") {
                Value* p = eval(c->args[0].get());
                b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {p});
                return nullptr;
            }
        }
        if (c->callee_kind == "ctor") {
            LStruct* st = cast<LStruct>(lt(c->ctor));
            if (c->field_order.empty() && c->args.empty() && c->named.empty())
                return ConstantAggregateZero::get(st);
            Value* v = UndefValue::get(st);
            for (size_t i = 0; i < c->field_order.size(); ++i) {
                int fi = c->field_order[i];
                Expr* arg = i < c->args.size() ? c->args[i].get() : c->named[i].second.get();
                Value* av = coerce(arg, c->ctor->fields[fi].type);
                v = b.CreateInsertValue(v, av, {llvmFieldIndex(c->ctor, fi)});
            }
            return v;
        }
        if (c->callee_kind == "struct_ctor") {
            TypePtr st = c->ctor;
            const auto* cpty = c->ctor_pty;
            auto* tmp = entryAlloca(lt(st), "ctor.tmp", alignOf(st));
            if (st->needs_cleanup()) zeroMem(tmp, st);
            std::vector<LType*> ptys{ptrTy()};
            for (auto& t : *cpty) ptys.push_back(lt(t));
            FunctionCallee callee = mod->getOrInsertFunction(
                ctor_cname(st->unit, st->name, *cpty),
                FunctionType::get(LType::getVoidTy(ctx), ptys, false));
            std::vector<Expr*> rawArgs;
            for (auto& a : c->args) rawArgs.push_back(a.get());
            ArgPack pack = prepareArgs(rawArgs, *cpty);
            std::vector<Value*> vals{tmp};
            for (auto* v : pack.vals) vals.push_back(v);
            b.CreateCall(callee, vals);
            releaseTemps(pack);
            return b.CreateLoad(lt(st), tmp);
        }
        if (c->callee_kind == "struct_method") {
            const MethodSig* sig = c->method;
            if (!sig || !sig->owner) fail("LLVM backend: struct method is missing its signature", c->line, c->col);
            Value* recv = nullptr;
            if (auto m = dynamic_cast<Member*>(c->callee.get())) {
                recv = addrOf(m->obj.get());
            } else {
                recv = b.CreateLoad(ptrTy(), thisSlot);
            }
            std::vector<Expr*> rawArgs;
            for (auto& a : c->args) rawArgs.push_back(a.get());
            ArgPack pack = prepareArgs(rawArgs, sig->params);
            FunctionCallee callee = methodFn(sig);
            std::vector<Value*> vals{recv};
            for (auto* v : pack.vals) vals.push_back(v);
            Value* r = b.CreateCall(callee, vals);
            releaseTemps(pack);
            return r;
        }
        if (c->callee_kind == "method") {
            const MethodSig* sig = c->method;
            if (!sig || !sig->owner) fail("LLVM backend: method call is missing its signature", c->line, c->col);
            Value* recv = nullptr;
            TypePtr recvTy = nullptr;
            bool recvOwned = false;
            if (auto m = dynamic_cast<Member*>(c->callee.get())) {
                recv = eval(m->obj.get());
                recvTy = m->obj->ty;
                recvOwned = ownsReference(m->obj.get());
            } else {
                recvTy = curClassTy;
                recv = b.CreateLoad(lt(recvTy), thisSlot);
            }
            std::vector<Expr*> rawArgs;
            for (auto& a : c->args) rawArgs.push_back(a.get());
            ArgPack pack = prepareArgs(rawArgs, sig->params);
            Value* r = emitMethodCallVals(sig, recv, recvTy, pack.vals, c);
            releaseTemps(pack);
            if (recvOwned) emitReleaseRef(recv, recvTy);
            return r;
        }
        if (c->callee_kind == "super") {
            if (c->method) {
                const MethodSig* sig = c->method;
                std::vector<LType*> ptys{ptrTy()};
                for (auto& t : sig->params) ptys.push_back(lt(t));
                FunctionCallee callee = mod->getOrInsertFunction(
                    method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params),
                    FunctionType::get(lt(sig->ret), ptys, false));
                std::vector<Expr*> rawArgs;
                for (auto& a : c->args) rawArgs.push_back(a.get());
                ArgPack pack = prepareArgs(rawArgs, sig->params);
                std::vector<Value*> vals{b.CreateLoad(ptrTy(), thisSlot)};
                for (auto* v : pack.vals) vals.push_back(v);
                Value* r = b.CreateCall(callee, vals);
                releaseTemps(pack);
                return r;
            }
            // super(...)：调用基类构造器，然后恢复派生类的 desc
            TypePtr base = curClassTy ? curClassTy->base : nullptr;
            if (!base || base->kind != TypeKind::Class)
                fail("LLVM backend: super(...) has no base class", c->line, c->col);
            if (!base->ctors.empty()) {
                const std::vector<TypePtr>* pty =
                    c->ctor_pty ? c->ctor_pty : nullptr;
                std::vector<TypePtr> empty;
                std::vector<LType*> ptys{ptrTy()};
                for (auto& t : (pty ? *pty : empty)) ptys.push_back(lt(t));
                FunctionCallee callee = mod->getOrInsertFunction(
                    ctor_cname(base->unit, base->name, pty ? *pty : empty),
                    FunctionType::get(LType::getVoidTy(ctx), ptys, false));
                std::vector<Expr*> rawArgs;
                for (auto& a : c->args) rawArgs.push_back(a.get());
                ArgPack pack = prepareArgs(rawArgs, pty ? *pty : empty);
                std::vector<Value*> vals{b.CreateLoad(ptrTy(), thisSlot)};
                for (auto* v : pack.vals) vals.push_back(v);
                b.CreateCall(callee, vals);
                releaseTemps(pack);
            }
            if (curClassTy) {
                Value* p = b.CreateLoad(ptrTy(), thisSlot);
                Value* dp = b.CreateGEP(i8(), p, ConstantInt::get(i64(), 8));
                b.CreateStore(descOf(curClassTy), dp);
            }
            return nullptr;
        }
        // 普通函数调用
        FuncDecl* f = c->func;
        if (!f) fail("LLVM backend: call is missing its target function", c->line, c->col);
        std::vector<Expr*> rawArgs;
        for (auto& a : c->args) rawArgs.push_back(a.get());
        return emitUserCall(f, rawArgs);
    }

    // 块是否已有终结指令（getTerminator 在空块上会断言）
    bool Emitter::hasTerm(BasicBlock* bb) {
        return bb && !bb->empty() && bb->back().isTerminator();
    }

    // 引用类型向下/向上转换（运行期检查 desc 链；失败得 null）
    Value* Emitter::emitDowncast(Value* src, const TypePtr& from, const TypePtr& to) {
        Value* obj = (from->kind == TypeKind::Interface) ? b.CreateExtractValue(src, {0}) : src;
        AllocaInst* res = entryAlloca(ptrTy(), "cast.res");
        BasicBlock* search = BasicBlock::Create(ctx, "cast.search", curFn);
        BasicBlock* check = BasicBlock::Create(ctx, "cast.check", curFn);
        BasicBlock* found = BasicBlock::Create(ctx, "cast.found", curFn);
        BasicBlock* done = BasicBlock::Create(ctx, "cast.done", curFn);
        b.CreateStore(Constant::getNullValue(ptrTy()), res);
        b.CreateCondBr(b.CreateICmpEQ(obj, Constant::getNullValue(ptrTy())), done, search);

        b.SetInsertPoint(search);
        AllocaInst* cur = entryAlloca(ptrTy(), "cast.cur");
        Value* d0 = b.CreateLoad(ptrTy(), b.CreateGEP(i8(), obj, ConstantInt::get(i64(), 8)));
        b.CreateStore(d0, cur);
        b.CreateBr(check);

        b.SetInsertPoint(check);
        Value* d = b.CreateLoad(ptrTy(), cur);
        Value* isNull = b.CreateICmpEQ(d, Constant::getNullValue(ptrTy()));
        BasicBlock* next = BasicBlock::Create(ctx, "cast.next", curFn);
        b.CreateCondBr(isNull, done, next);

        b.SetInsertPoint(next);
        Value* isTarget = b.CreateICmpEQ(d, cast<Value>(descOf(to)));
        BasicBlock* advance = BasicBlock::Create(ctx, "cast.advance", curFn);
        b.CreateCondBr(isTarget, found, advance);

        b.SetInsertPoint(advance);
        Value* base = b.CreateLoad(ptrTy(), b.CreateGEP(i8(), d, ConstantInt::get(i64(), 32)));
        b.CreateStore(base, cur);
        b.CreateBr(check);

        b.SetInsertPoint(found);
        b.CreateStore(obj, res);
        b.CreateBr(done);

        b.SetInsertPoint(done);
        return b.CreateLoad(ptrTy(), res);
    }

    Value* Emitter::eval(Expr* e) {
        if (auto v = dynamic_cast<IntLit*>(e)) {
            if (isRefKind(e->ty)) return Constant::getNullValue(lt(e->ty));
            if (e->ty && e->ty->kind == TypeKind::Null) return Constant::getNullValue(ptrTy());
            LType* ty = lt(e->ty);
            // 整数字面量按上下文适配到浮点类型（SPEC §3.6）
            if (e->ty && e->ty->kind == TypeKind::Prim && e->ty->is_float())
                return ConstantFP::get(ty, static_cast<double>(v->value));
            return ConstantInt::get(cast<IntegerType>(ty), static_cast<uint64_t>(v->value),
                                    /*isSigned=*/true);
        }
        if (auto v = dynamic_cast<FloatLit*>(e)) return ConstantFP::get(lt(e->ty), v->value);
        if (auto v = dynamic_cast<CharLit*>(e)) return ConstantInt::get(i8(), v->value);
        if (dynamic_cast<BoolLit*>(e))
            return ConstantInt::get(i1(), static_cast<BoolLit*>(e)->value ? 1 : 0);
        if (dynamic_cast<StringLit*>(e))
            return stringConst(static_cast<StringLit*>(e)->value);
        if (dynamic_cast<NullLit*>(e)) {
            if (e->ty && isRefKind(e->ty)) return Constant::getNullValue(lt(e->ty));
            return Constant::getNullValue(ptrTy());
        }
        if (auto n = dynamic_cast<Name*>(e)) {
            if (n->kind == "enum_const") {
                auto* ec = std::get_if<EnumConst*>(&n->sym);
                return ConstantInt::get(i32(), static_cast<uint64_t>((*ec)->value),
                                        true);
            }
            if (n->kind == "const_member") {
                auto* cm = std::get_if<ConstMember*>(&n->sym);
                if (!cm || !*cm) fail("LLVM backend: missing const member", e->line, e->col);
                return constOfValue(e->ty, (*cm)->is_float, (*cm)->ival, (*cm)->fval);
            }
            if (n->kind == "func") fail("LLVM backend: functions cannot be used as values", e->line, e->col);
            return loadAt(lt(e->ty), addrOf(e), unalignedExpr(e));
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            if (m->kind == "enum_const") {
                auto* ec = std::get_if<EnumConst*>(&m->sym);
                if (!ec) fail("LLVM backend: missing enum constant", e->line, e->col);
                return ConstantInt::get(i32(), static_cast<uint64_t>((*ec)->value), true);
            }
            if (m->kind == "const_member") {
                auto* cm = std::get_if<ConstMember*>(&m->sym);
                if (!cm || !*cm) fail("LLVM backend: missing const member", e->line, e->col);
                return constOfValue(e->ty, (*cm)->is_float, (*cm)->ival, (*cm)->fval);
            }
            if (m->kind == "field" && m->obj->ty && m->obj->ty->kind == TypeKind::Class) {
                Value* p = eval(m->obj.get());
                Value* addr = fieldAddr(p, m->obj->ty, m->field_index);
                Value* v = b.CreateLoad(lt(e->ty), addr);
                if (ownsReference(m->obj.get())) emitReleaseRef(p, m->obj->ty);
                return v;
            }
            return loadAt(lt(e->ty), addrOf(e), unalignedExpr(e));
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            if (ix->get_func) {
                Value* r = emitUserCall(ix->get_func, {ix->base.get(), ix->indices[0].get()});
                if (ix->by_ref) return loadAt(lt(e->ty), r, unalignedExpr(e));
                return r;
            }
            return loadAt(lt(e->ty), addrOf(e), unalignedExpr(e));
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op_func) return emitUserCall(u->op_func, {u->operand.get()});
            if (u->op == "&") return addrOf(u->operand.get());
            if (u->op == "*") return loadAt(lt(e->ty), eval(u->operand.get()), unalignedExpr(e));
            Value* v = eval(u->operand.get());
            if (u->op == "-") {
                if (e->ty->kind == TypeKind::Prim && e->ty->is_float())
                    return b.CreateFNeg(v);
                return b.CreateSub(ConstantInt::get(lt(e->ty), 0), v);
            }
            if (u->op == "+") return v;
            if (u->op == "~")
                return b.CreateXor(v, ConstantInt::get(lt(e->ty), static_cast<uint64_t>(-1), true));
        }
        if (auto v = dynamic_cast<NotExpr*>(e)) {
            Value* x = eval(v->operand.get());
            return b.CreateXor(x, ConstantInt::get(i1(), 1));
        }
        if (auto v = dynamic_cast<Binary*>(e)) return evalBinary(v);
        if (auto v = dynamic_cast<Ternary*>(e)) return evalTernary(v);
        if (auto v = dynamic_cast<Cast*>(e)) {
            TypePtr from = v->expr->ty;
            if (from && e->ty && isRefKind(from) && e->ty->kind == TypeKind::Class &&
                !same_type(from, e->ty)) {
                return emitDowncast(eval(v->expr.get()), from, e->ty);
            }
            return convertTo(eval(v->expr.get()), v->expr->ty, e->ty, v->line, v->col);
        }
        if (auto v = dynamic_cast<Call*>(e)) {
            Value* r = evalCall(v);
            if (v->by_ref) return loadAt(lt(e->ty), r, false);
            return r;
        }
        if (auto v = dynamic_cast<NewExpr*>(e)) return evalNew(v);
        if (auto v = dynamic_cast<SizeofExpr*>(e)) {
            // resolve spec via sema 已写入 e->ty=usize；尺寸需要类型本身：由 spec 推断
            TypePtr t = specType(v->spec.get());
            uint64_t n = v->which == "alignof" ? alignOf(t) : sizeOf(t);
            return ConstantInt::get(i64(), n);
        }
        if (auto v = dynamic_cast<Len*>(e)) {
            TypePtr st = v->seq->ty;
            int axis = 0;
            Value* axisV = nullptr;
            if (isStrType(st)) {
                Value* s = evalStrValue(v->seq.get());
                return b.CreateExtractValue(s, {1});
            }
            if (v->dim) {
                if (auto il = dynamic_cast<IntLit*>(v->dim.get())) {
                    axis = static_cast<int>(il->value);
                } else if (st->kind == TypeKind::Slice && st->rank > 1) {
                    axisV = convertTo(eval(v->dim.get()), v->dim->ty,
                                      Type::make_prim("usize"));
                } else {
                    fail("the `len(seq, k)` axis of a static or one-dimensional array must be an integer literal",
                         v->dim->line, v->dim->col);
                }
            }
            if (st->kind == TypeKind::Array) {
                TypePtr cur = st;
                for (int i = 0; i < axis; ++i) {
                    if (!cur || cur->kind != TypeKind::Array)
                        fail("`len` axis is out of bounds", v->line, v->col);
                    cur = cur->elem;
                }
                if (!cur || cur->kind != TypeKind::Array)
                    fail("`len` axis is out of bounds", v->line, v->col);
                return ConstantInt::get(i64(), cur->array_size);
            }
            Value* s = eval(v->seq.get());
            if (st->kind == TypeKind::Slice) {
                trackOwnedSliceTemp(v->seq.get(), s, st);
                if (axisV) return emitSliceDimDynamic(s, axisV, st->rank);
                return emitSliceDim(s, axis);
            }
            return b.CreateExtractValue(s, {1});
        }
        fail("LLVM backend: unknown expression", e->line, e->col);
    }

    // sizeof/alignof 的 TypeSpec 解析（只依赖已解析的类型模型）
    TypePtr Emitter::specType(TypeSpec* spec) {
        if (!spec) return nullptr;
        curSpecUnitFix();
        switch (spec->kind) {
            case TypeSpec::K::Named: {
                if (spec->name == "void") return Type::make_void();
                if (spec->name == "string") return Type::make_string();
                if (is_prim_name(spec->name)) return Type::make_prim(spec->name);
                if (spec->name.find('.') != std::string::npos) {
                    auto p = spec->name.find('.');
                    std::string un = spec->name.substr(0, p), tn = spec->name.substr(p + 1);
                    auto it = prog.infos.find(un);
                    if (it != prog.infos.end()) {
                        auto s = it->second.structs.find(tn);
                        if (s != it->second.structs.end()) return s->second->ty;
                        auto c = it->second.classes.find(tn);
                        if (c != it->second.classes.end()) return c->second->ty;
                        auto i2 = it->second.interfaces.find(tn);
                        if (i2 != it->second.interfaces.end()) return i2->second->ty;
                        auto e = it->second.enums.find(tn);
                        if (e != it->second.enums.end()) return e->second->ty;
                        auto al = it->second.aliases.find(tn);
                        if (al != it->second.aliases.end() && al->second->resolved) return al->second->rty;
                    }
                }
                auto it = prog.infos[curUnit].structs.find(spec->name);
                if (it != prog.infos[curUnit].structs.end()) return it->second->ty;
                auto c = prog.infos[curUnit].classes.find(spec->name);
                if (c != prog.infos[curUnit].classes.end()) return c->second->ty;
                auto i2 = prog.infos[curUnit].interfaces.find(spec->name);
                if (i2 != prog.infos[curUnit].interfaces.end()) return i2->second->ty;
                auto e = prog.infos[curUnit].enums.find(spec->name);
                if (e != prog.infos[curUnit].enums.end()) return e->second->ty;
                auto al = prog.infos[curUnit].aliases.find(spec->name);
                if (al != prog.infos[curUnit].aliases.end() && al->second->resolved) return al->second->rty;
                fail("LLVM backend: sizeof/alignof on unknown type " + spec->name);
            }
            case TypeSpec::K::Ptr:
                return Type::make_ptr(specType(spec->elem.get()), spec->is_const);
            case TypeSpec::K::Slice:
                return Type::make_slice(specType(spec->elem.get()), spec->is_const);
            case TypeSpec::K::Array:
                return Type::make_array(specType(spec->elem.get()), spec->size);
        }
        fail("LLVM backend: unknown type spec");
    }
    void Emitter::curSpecUnitFix() {}

}  // namespace muc_detail
