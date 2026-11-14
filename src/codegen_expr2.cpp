// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains expression code generation (calls, new, downcasts, eval and sizeof type resolution) (Emitter member function definitions).

#include "codegen_impl.hpp"

namespace muc_detail {

using ArgPack = Emitter::ArgPack;

    // Method call (recv convention: class = handle value; struct = object address)
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
                // calling an interface default method on a class value: construct the interface value on the spot
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
        // not in the vtable: statically call the implementation at the declaration site
        FunctionCallee callee = methodFn(sig);
        std::vector<Value*> vals{recv};
        vals.insert(vals.end(), args.begin(), args.end());
        return b.CreateCall(callee, vals);
    }

    // User-defined operator[] (free function): the container is passed as a class handle / struct address / struct value
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
            cArg = seqAddr;  // borrowed receiver: pass the container address in directly
        } else if (sty->kind == TypeKind::Class) {
            cArg = b.CreateLoad(lt(sty), seqAddr);
        } else if (!f->pty.empty() && f->pty[0] && f->pty[0]->kind == TypeKind::Ptr) {
            cArg = seqAddr;
        } else {
            cArg = loadAt(lt(sty), seqAddr, unaligned);
        }
        Value* r = b.CreateCall(callee, {cArg, idxVal});
        if (mayThrow.mayThrowFunc(f)) emitCheckPending();
        return r;
    }

    Value* Emitter::emitStructMethodCall(const MethodSig* sig, Value* recvAddr,
                                         const std::vector<Expr*>& args) {
        ArgPack pack = prepareArgs(args, sig->params);
        FunctionCallee callee = methodFn(sig);
        std::vector<Value*> vals{recvAddr};
        for (auto* v : pack.vals) vals.push_back(v);
        Value* r = b.CreateCall(callee, vals);
        releaseTemps(pack);
        if (mayThrow.mayThrowMethod(sig, sig->owner)) emitCheckPending();
        return r;
    }

    Value* Emitter::emitMemberOp(const MethodSig* sig, Expr* recv, const std::vector<Expr*>& args) {
        return emitStructMethodCall(sig, addrOf(recv), args);
    }

    Value* Emitter::emitMemberIndex(const MethodSig* sig, Value* recvAddr, const std::vector<Value*>& idxVals) {
        std::vector<LType*> pty{ptrTy()};
        for (const auto& p : sig->params) pty.push_back(lt(p));
        FunctionCallee callee = mod->getOrInsertFunction(
            method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params),
            FunctionType::get(lt(sig->ret), pty, false));
        std::vector<Value*> args{recvAddr};
        for (auto* v : idxVals) args.push_back(v);
        Value* r = b.CreateCall(callee, args);
        if (mayThrow.mayThrowMethod(sig, sig->owner)) emitCheckPending();
        return r;
    }

    Value* Emitter::emitMemberIndexSet(const MethodSig* sig, Value* recvAddr, Value* idxVal, Expr* value) {
        FunctionCallee callee = mod->getOrInsertFunction(
            method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params),
            FunctionType::get(LType::getVoidTy(ctx), {ptrTy(), lt(sig->params[0]), lt(sig->params[1])}, false));
        ArgPack pack;
        Value* v = coerce(value, sig->params[1]);
        if (needsCleanup(sig->params[1]) && convOwns(value, sig->params[1])) {
            pack.owned.emplace_back(v, sig->params[1]);
        }
        b.CreateCall(callee, {recvAddr, idxVal, v});
        releaseTemps(pack);
        if (mayThrow.mayThrowMethod(sig, sig->owner)) emitCheckPending();
        return nullptr;
    }

    // Call a free function / operator / method (static dispatch)
    Value* Emitter::emitUserCall(FuncDecl* f, const std::vector<Expr*>& args) {
        std::vector<LType*> ptys;
        for (size_t i = 0; i < f->pty.size(); ++i)
            ptys.push_back(i < f->pty_ref.size() && f->pty_ref[i] ? ptrTy() : lt(f->pty[i]));
        FunctionCallee callee =
            mod->getOrInsertFunction(f->cname, FunctionType::get(lt(f->rty), ptys, f->is_varargs));
        ArgPack pack;
        for (size_t i = 0; i < args.size() && i < f->pty.size(); ++i) {
            if (i < f->pty_ref.size() && f->pty_ref[i]) {
                pack.vals.push_back(addrOf(args[i]));  // borrow: the callee does not own it, no cleanup after the call
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
                // C99 6.3.2.1: an array argument decays to a pointer before the default promotions.
                if (args[i]->ty && args[i]->ty->kind == TypeKind::Array) {
                    pack.vals.push_back(addrOf(args[i]));
                    continue;
                }
                pack.vals.push_back(promoteVararg(eval(args[i]), args[i]->ty));
            }
        }
        Value* r = b.CreateCall(callee, pack.vals);
        releaseTemps(pack);
        // C functions never throw; a mu_lan function is checked only when the
        // conservative may-throw analysis cannot prove it nothrow.
        if (!f->is_extern && mayThrow.mayThrowFunc(f)) emitCheckPending();
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
            // _aligned_malloc(size, alignment): the object alignment exceeds malloc's 16-byte guarantee (paired with _aligned_free)
            p = b.CreateCall(libFn("_aligned_malloc", ptrTy(), {i64(), i64()}),
                             {ConstantInt::get(i64(), sz),
                              ConstantInt::get(i64(), objectAlign(cls))});
        } else {
            p = b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {ConstantInt::get(i64(), sz)});
        }
        // abort directly on allocation failure (the language has no exceptions)
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
            if (mayThrow.mayThrowCname(ctor_cname(cls->unit, cls->name, ne->ctor_pty))) {
                emitCheckPendingWith([&] { emitRelease(p); });
            }
        } else {
            // implicit default construction: call the parameterless constructor declared by the nearest ancestor (it keeps chaining to its own base class)
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
                        if (mayThrow.mayThrowCname(ctor_cname(b0->unit, b0->name, {}))) {
                            emitCheckPendingWith([&] { emitRelease(p); });
                        }
                    }
                    break;
                }
                b0 = b0->base;
            }
            storeDesc();  // the ancestor constructor may have overwritten desc, restore it to the final type
        }
        return p;
    }

    Value* Emitter::evalCall(Call* c) {
        if (c->callee_kind == "slice_length") {
            auto* m = dynamic_cast<Member*>(c->callee.get());
            Expr* base = m->obj.get();
            TypePtr st = base->ty;
            Value* h = eval(base);
            trackOwnedSliceTemp(base, h, st);
            if (c->args.empty()) return emitSliceDim(h, 0);
            if (auto il = dynamic_cast<IntLit*>(c->args[0].get())) {
                return emitSliceDim(h, static_cast<int>(il->value));
            }
            Value* axisV = convertTo(eval(c->args[0].get()), c->args[0]->ty,
                                     Type::make_prim("usize"));
            return emitSliceDimDynamic(h, axisV, st->rank);
        }
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
            if (mayThrow.mayThrowCname(ctor_cname(st->unit, st->name, *cpty))) {
                emitCheckPendingWith([&] { emitReleaseValue(tmp, st); });
            }
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
            if (mayThrow.mayThrowMethod(sig, sig->owner)) emitCheckPending();
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
            if (mayThrow.mayThrowMethod(sig, recvTy)) emitCheckPending();
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
                if (mayThrow.mayThrowMethod(sig, sig->owner)) emitCheckPending();
                return r;
            }
            // super(...): call the base class constructor, then restore the derived class's desc
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
                if (mayThrow.mayThrowCname(ctor_cname(base->unit, base->name, pty ? *pty : empty))) {
                    emitCheckPending();
                }
            }
            if (curClassTy) {
                Value* p = b.CreateLoad(ptrTy(), thisSlot);
                Value* dp = b.CreateGEP(i8(), p, ConstantInt::get(i64(), 8));
                b.CreateStore(descOf(curClassTy), dp);
            }
            return nullptr;
        }
        if (c->callee_kind == "fnptr" || c->callee_kind == "delegate") {
            const TypePtr& ft = c->fn_ty;
            if (!ft || !is_callable_value(ft)) {
                fail("LLVM backend: indirect call without a function type", c->line, c->col);
            }
            std::vector<Expr*> rawArgs;
            for (auto& a : c->args) rawArgs.push_back(a.get());
            ArgPack pack = prepareArgs(rawArgs, ft->fparams);
            if (ft->kind == TypeKind::Delegate) {
                // delegate calling convention: `code(env, args...)`
                Value* dv = eval(c->callee.get());
                Value* code = b.CreateExtractValue(dv, {0});
                Value* env = b.CreateExtractValue(dv, {1});
                std::vector<LType*> ptys{ptrTy()};
                for (auto& pt : ft->fparams) ptys.push_back(lt(pt));
                std::vector<Value*> vals{env};
                for (auto* v : pack.vals) vals.push_back(v);
                Value* r = b.CreateCall(FunctionType::get(lt(ft->fret), ptys, false), code, vals);
                releaseTemps(pack);
                emitCheckPending();
                return r;
            }
            Value* fp = eval(c->callee.get());
            std::vector<LType*> ptys;
            for (auto& pt : ft->fparams) ptys.push_back(lt(pt));
            Value* r = b.CreateCall(FunctionType::get(lt(ft->fret), ptys, false), fp, pack.vals);
            releaseTemps(pack);
            // the callee is unknown: conservatively assume it may throw
            emitCheckPending();
            return r;
        }
        // ordinary function call
        FuncDecl* f = c->func;
        if (!f) fail("LLVM backend: call is missing its target function", c->line, c->col);
        std::vector<Expr*> rawArgs;
        for (auto& a : c->args) rawArgs.push_back(a.get());
        return emitUserCall(f, rawArgs);
    }

    // whether the block already has a terminator instruction (getTerminator asserts on an empty block)
    bool Emitter::hasTerm(BasicBlock* bb) {
        return bb && !bb->empty() && bb->back().isTerminator();
    }

    // Downcast/upcast of reference types (checks the desc chain at runtime; yields null on failure)
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
        if (auto lm = dynamic_cast<Lambda*>(e)) {
            FuncDecl* f = lm->synth;
            if (!f) fail("LLVM backend: lambda without a synthesized function", e->line, e->col);
            std::vector<LType*> ptys;
            for (auto& pt : f->pty) ptys.push_back(lt(pt));
            Value* callee = mod->getOrInsertFunction(f->cname, FunctionType::get(lt(f->rty), ptys, false))
                                .getCallee();
            if (!lm->env_ty) return callee;
            // Capturing lambda (U41): allocate the environment object and copy (or take the address of)
            // each captured variable. `[x]` stores a copy and takes its own reference; `[ref x]` stores
            // the address, so writes from the body reach the original variable.
            Value* env = emitClassAlloc(lm->env_ty);
            for (size_t i = 0; i < lm->env_src.size(); ++i) {
                const LocalVar* src = lm->env_src[i];
                Value* dst = fieldAddr(env, lm->env_ty, static_cast<int>(i));
                if (!src) {
                    // `[this]`: the receiver of the enclosing method. `load(thisSlot)` is the class
                    // handle for a class method and the object address for a struct method.
                    Value* objp = b.CreateLoad(ptrTy(), thisSlot);
                    if (lm->env_src_ref[i]) {
                        b.CreateStore(objp, dst);  // `[ref this]` on a struct: writes reach the object
                    } else if (f->this_capture_ty && f->this_capture_ty->kind == TypeKind::Struct) {
                        b.CreateStore(b.CreateLoad(lt(f->this_capture_ty), objp), dst);
                        if (needsCleanup(f->this_capture_ty)) emitRetainValue(dst, f->this_capture_ty);
                    } else {
                        b.CreateStore(objp, dst);  // the handle; the environment keeps the object alive
                        emitRetain(objp);
                    }
                    continue;
                }
                Value* sslot = slotFor(src);
                if (lm->env_src_ref[i]) {
                    Value* addr = src->by_ref ? b.CreateLoad(ptrTy(), sslot) : sslot;
                    b.CreateStore(addr, dst);
                } else {
                    b.CreateStore(loadAt(lt(src->rty), sslot, false), dst);
                    if (needsCleanup(src->rty)) emitRetainValue(dst, src->rty);
                }
            }
            Value* dv = UndefValue::get(lt(e->ty));
            dv = b.CreateInsertValue(dv, callee, {0});
            dv = b.CreateInsertValue(dv, env, {1});
            return dv;
        }
        if (auto v = dynamic_cast<IntLit*>(e)) {
            if (isRefKind(e->ty)) return Constant::getNullValue(lt(e->ty));
            if (e->ty && e->ty->kind == TypeKind::Null) return Constant::getNullValue(ptrTy());
            LType* ty = lt(e->ty);
            // integer literals adapt to the float type according to context (SPEC §3.6)
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
            if (n->kind == "func") {
                auto* f = std::get_if<FuncDecl*>(&n->sym);
                if (!f || !*f) fail("LLVM backend: missing function target", e->line, e->col);
                std::vector<LType*> ptys;
                for (auto& pt : (*f)->pty) ptys.push_back(lt(pt));
                return mod->getOrInsertFunction((*f)->cname,
                                                FunctionType::get(lt((*f)->rty), ptys, (*f)->is_varargs))
                    .getCallee();
            }
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
            if (m->kind == "func") {
                auto* f = std::get_if<FuncDecl*>(&m->sym);
                if (!f || !*f) fail("LLVM backend: missing function target", e->line, e->col);
                std::vector<LType*> ptys;
                for (auto& pt : (*f)->pty) ptys.push_back(lt(pt));
                return mod->getOrInsertFunction((*f)->cname,
                                                FunctionType::get(lt((*f)->rty), ptys, (*f)->is_varargs))
                    .getCallee();
            }
            if (m->kind == "slice_ptr") {
                // `buffer.ptr`: null handle -> null pointer, otherwise the element data area.
                Value* h = eval(m->obj.get());
                BasicBlock* from = b.GetInsertBlock();
                BasicBlock* nn = BasicBlock::Create(ctx, "ptr.nn", curFn);
                BasicBlock* done = BasicBlock::Create(ctx, "ptr.done", curFn);
                b.CreateCondBr(b.CreateICmpEQ(h, Constant::getNullValue(ptrTy())), done, nn);
                b.SetInsertPoint(nn);
                Value* data = sliceDataPtr(h, m->obj->ty->rank);
                BasicBlock* nnEnd = b.GetInsertBlock();
                b.CreateBr(done);
                b.SetInsertPoint(done);
                auto* phi = b.CreatePHI(ptrTy(), 2);
                phi->addIncoming(Constant::getNullValue(ptrTy()), from);
                phi->addIncoming(data, nnEnd);
                return phi;
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
            if (ix->get_method) {
                std::vector<Value*> idxVals;
                for (auto& ie : ix->indices)
                    idxVals.push_back(convertTo(eval(ie.get()), ie->ty, Type::make_prim("usize")));
                Value* r = emitMemberIndex(ix->get_method, addrOf(ix->base.get()), idxVals);
                if (ix->by_ref) return loadAt(lt(e->ty), r, unalignedExpr(e));
                return r;
            }
            if (ix->get_func) {
                std::vector<Expr*> callArgs{ix->base.get()};
                for (auto& ie : ix->indices) callArgs.push_back(ie.get());
                Value* r = emitUserCall(ix->get_func, callArgs);
                if (ix->by_ref) return loadAt(lt(e->ty), r, unalignedExpr(e));
                return r;
            }
            return loadAt(lt(e->ty), addrOf(e), unalignedExpr(e));
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op_method) return emitMemberOp(u->op_method, u->operand.get(), {});
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
            // sema folded the value (type form, expression form and `offsetof` alike); emit the constant
            return ConstantInt::get(i64(), v->cval);
        }
        if (auto v = dynamic_cast<Len*>(e)) {
            TypePtr st = v->seq->ty;
            int axis = 0;
            Value* axisV = nullptr;
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

}  // namespace muc_detail
