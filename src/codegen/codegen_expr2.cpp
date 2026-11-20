// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains expression code generation (calls, new, downcasts, eval and sizeof type resolution) (Emitter member function definitions).
//
// owns:      call and method-call lowering, new, downcasts, the eval dispatch and sizeof type resolution
// not:       statement emission (codegen_stmt.cpp), dispatch tables
// pass:      emission
// concept:   codegen-expressions, codegen-calls

#include "codegen_impl.hpp"

namespace muc_detail {

using ArgPack = Emitter::ArgPack;

    // Method call (recv convention: class = handle value; struct = object address)
    Value* Emitter::emitMethodCallVals(const MethodSig* sig, Value* recv, const TypePtr& recvTy,
                                       const std::vector<Value*>& args, Node* at) {
        std::vector<LType*> ptys{ptrTy()};
        for (auto& t : sig->params) ptys.push_back(lt(t));
        auto* fnTy = FunctionType::get(lt(sig->ret), ptys, false);

        // The receiver is dereferenced (object -> descriptor -> vtable) before the call, so a null
        // handle must be rejected here rather than inside the method (SPEC 8.2).
        if (recv && recv->getType()->isPointerTy()) {
            emitNullCheck(recv, "method call on a null `" + std::string(sig->owner ? sig->owner->name : std::string("class")) +
                                    "` reference");
        } else if (recv && recvTy && recvTy->kind == TypeKind::Interface) {
            emitNullCheck(refObj(recv, recvTy),
                          "method call on a null `" + recvTy->name + "` reference");
        }

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
        if (slot >= 0 && !methodDispatchIsStatic(recvTy, sig)) {
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
        checkFnValueAbi(f, nullptr);  // the argument layout below is fixed by the operator form
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
        // A class member operator receives the handle (borrowed `this`, so no object copy); a struct one
        // receives the object address. Dispatch goes through the same path as a method call, so a
        // `virtual` operator still resolves dynamically.
        if (recv->ty && recv->ty->kind == TypeKind::Class) {
            Value* h = eval(recv);
            const bool owned = ownsReference(recv);
            ArgPack pack = prepareArgs(args, sig->params);
            Value* r = emitMethodCallVals(sig, h, recv->ty, pack.vals, recv);
            releaseTemps(pack);
            if (owned) emitReleaseRef(h, recv->ty);
            if (mayThrow.mayThrowMethod(sig, recv->ty)) emitCheckPending();
            return r;
        }
        return emitStructMethodCall(sig, addrOf(recv), args);
    }

    Value* Emitter::emitMemberIndex(const MethodSig* sig, Value* recvAddr, const std::vector<Value*>& idxVals) {
        if (sig->owner && sig->owner->kind == TypeKind::Class) {
            // `recvAddr` is the slot holding the handle; the operator itself takes the handle by value
            Value* h = b.CreateLoad(ptrTy(), recvAddr);
            Value* r = emitMethodCallVals(sig, h, sig->owner, idxVals, nullptr);
            if (mayThrow.mayThrowMethod(sig, sig->owner)) emitCheckPending();
            return r;
        }
        std::vector<LType*> pty{ptrTy()};
        for (const auto& p : sig->params) pty.push_back(lt(p));
        FunctionCallee callee = mod->getOrInsertFunction(
            method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params, sig->is_const),
            FunctionType::get(lt(sig->ret), pty, false));
        std::vector<Value*> args{recvAddr};
        for (auto* v : idxVals) args.push_back(v);
        Value* r = b.CreateCall(callee, args);
        if (mayThrow.mayThrowMethod(sig, sig->owner)) emitCheckPending();
        return r;
    }

    Value* Emitter::emitMemberIndexSet(const MethodSig* sig, Value* recvAddr, Value* idxVal, Expr* value) {
        FunctionCallee callee = mod->getOrInsertFunction(
            method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params, sig->is_const),
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
        // `extern` and `export` symbols follow the C ABI; everything else keeps the backend's own
        // (faster) convention. Both sides of a C-ABI call come from cAbiSig/cAbiCallee.
        CAbiSig sig;
        FunctionCallee callee;
        std::vector<LType*> ptys;
        if (f->c_abi) {
            sig = cAbiSig(f->pty, f->pty_ref, f->rty);
            callee = cAbiCallee(f);
        } else {
            for (size_t i = 0; i < f->pty.size(); ++i)
                ptys.push_back(i < f->pty_ref.size() && f->pty_ref[i] ? ptrTy() : lt(f->pty[i]));
            callee =
                mod->getOrInsertFunction(f->cname, FunctionType::get(lt(f->rty), ptys, f->is_varargs));
        }
        ArgPack pack;
        Value* sretSlot = nullptr;
        if (sig.sret) {
            sretSlot = entryAlloca(lt(f->rty), "sret", alignOf(f->rty));
            pack.vals.push_back(sretSlot);  // hidden first argument: the result address
        }
        for (size_t i = 0; i < args.size() && i < f->pty.size(); ++i) {
            if (i < f->pty_ref.size() && f->pty_ref[i]) {
                pack.vals.push_back(addrOf(args[i]));  // borrow: the callee does not own it, no cleanup after the call
                continue;
            }
            Value* v = coerceArg(args[i], f->pty[i]);
            // the callee owns a by-value struct parameter (ownParams releases it); see prepareArgs
            const bool handedOver = f->pty[i] && f->pty[i]->kind == TypeKind::Struct;
            if (!handedOver && needsCleanup(f->pty[i]) && convOwns(args[i], f->pty[i])) {
                pack.owned.emplace_back(v, f->pty[i]);
            }
            if (f->c_abi) {
                const size_t sigIdx = (sig.sret ? 1u : 0u) + i;
                if (sig.pass[i] == AbiPass::ByVal) {
                    // C hands the callee a pointer to a copy it may modify: materialize one (the value
                    // itself stays owned by the caller and is released with the other temporaries).
                    auto* tmp = entryAlloca(lt(f->pty[i]), "byval", alignOf(f->pty[i]));
                    b.CreateStore(v, tmp);
                    v = tmp;
                } else if (sig.pass[i] != AbiPass::Direct) {
                    v = coerceToReg(v, f->pty[i], sig.params[sigIdx]);
                }
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
        CallInst* call = b.CreateCall(callee, pack.vals);
        if (f->c_abi) applyCAbiCallAttrs(call, sig, f->pty, f->pty_ref, f->rty);
        releaseTemps(pack);
        // C functions never throw; a mu_lan function is checked only when the
        // conservative may-throw analysis cannot prove it nothrow.
        if (!f->is_extern && mayThrow.mayThrowFunc(f)) emitCheckPending();
        if (sretSlot) return b.CreateLoad(lt(f->rty), sretSlot);
        if (f->c_abi && sig.ret != lt(f->rty)) return coerceFromReg(call, f->rty);
        return call;
    }

    // A function value carries only the Mu signature (no `byval`/`sret`, no packed eightbyte), so a
    // C-ABI function whose aggregates travel differently cannot be taken as one: the call through
    // the value would use the Mu convention on a symbol emitted with the C one.
    void Emitter::checkFnValueAbi(FuncDecl* f, Node* at) {
        if (!f->c_abi) return;
        bool bad = abi_class(f->rty, /*is_return=*/true, targetTriple) != AbiPass::Direct;
        for (size_t i = 0; !bad && i < f->pty.size(); ++i) {
            const bool is_ref = i < f->pty_ref.size() && f->pty_ref[i];
            if (!is_ref && abi_class(f->pty[i], /*is_return=*/false, targetTriple) != AbiPass::Direct) {
                bad = true;
            }
        }
        if (bad) {
            fail("`" + f->name + "` uses the C ABI for a struct parameter or return, which this call "
                 "form cannot express (call it directly)",
                 at ? at->line : 0, at ? at->col : 0);
        }
    }

    Value* Emitter::evalNew(NewExpr* ne) {
        if (ne->is_array) {
            std::vector<Value*> dims;
            for (auto& d : ne->dims) {
                dims.push_back(convertTo(eval(d.get()), d->ty, Type::make_prim("isize")));
            }
            return emitSliceAlloc(dims, ne->elem_ty, ne->uninit);
        }
        TypePtr cls = ne->class_ty;
        uint64_t sz = objectSize(cls);
        Value* p = nullptr;
        if (classNeedsAlignedAlloc(cls)) {
            // The object alignment exceeds malloc's guarantee; emit the target's aligned allocator.
            p = emitAlignedAlloc(ConstantInt::get(i64(), sz),
                                 ConstantInt::get(i64(), objectAlign(cls)));
        } else {
            p = b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {ConstantInt::get(i64(), sz)});
        }
        // abort directly on allocation failure (the language has no exceptions)
        BasicBlock* okBB = BasicBlock::Create(ctx, "new.ok", curFn);
        BasicBlock* failBB = BasicBlock::Create(ctx, "new.fail", curFn);
        b.CreateCondBr(b.CreateICmpEQ(p, Constant::getNullValue(ptrTy())), failBB, okBB);
        b.SetInsertPoint(failBB);
        b.CreateCall(abortTrap(), {});
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
                emitCheckPendingWith([&] { emitPartialDestroyCall(p, cls); });
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
                            emitCheckPendingWith([&] { emitPartialDestroyCall(p, cls); });
                        }
                    }
                    break;
                }
                b0 = b0->base;
            }
            storeDesc();  // the ancestor constructor may have overwritten desc, restore it to the final type
            // This class declares no constructor of its own, so the member step has no constructor body to
            // hang off: run it here, after the inherited default construction and the descriptor restore.
            // A member constructor that throws must release the object: nothing owns it yet at this point.
            emitMemberInits(p, cls, nullptr, [&] { emitPartialDestroyCall(p, cls); });
        }
        return p;
    }

    Value* Emitter::evalCall(Call* c) {
        if (c->callee_kind == "builtin_const") {
            return ConstantInt::get(lt(c->ty), static_cast<uint64_t>(c->builtin_value), true);
        }
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
                                     Type::make_prim("isize"));
            return emitSliceDimDynamic(h, axisV, st->rank);
        }
        if (c->callee_kind == "slice_ptr") {
            // `x.ptr()`: a null handle yields a null pointer, otherwise the element data area.
            auto* m = dynamic_cast<Member*>(c->callee.get());
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
        if (c->callee_kind == "builtin") {
            if (c->builtin == "alloc") {
                Value* n = convertTo(eval(c->args[0].get()), c->args[0]->ty,
                                     Type::make_prim("isize"));
                return b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {n});
            }
            if (c->builtin == "realloc") {
                Value* p = eval(c->args[0].get());
                Value* n = convertTo(eval(c->args[1].get()), c->args[1]->ty,
                                     Type::make_prim("isize"));
                return b.CreateCall(libFn("realloc", ptrTy(), {ptrTy(), i64()}), {p, n});
            }
            if (c->builtin == "free") {
                Value* p = eval(c->args[0].get());
                b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {p});
                return nullptr;
            }
        }
        if (c->callee_kind == "sum_ctor") {
            const TypePtr& st = c->sum_ty;
            const int alt = c->sum_alt;
            const auto& fs = st->sum_fields[static_cast<size_t>(alt)];
            auto* tmp = entryAlloca(lt(st), "sum", alignOf(st));
            zeroMem(tmp, st);
            // tag
            Value* tagp = fieldAddr(tmp, st->sum_layout, 0);
            b.CreateStore(ConstantInt::get(i32(), alt), tagp);
            // payload: the union member for this alternative (all members sit at offset 0)
            if (!fs.empty()) {
                // a union's members all start at the union's own address, so the payload slot is `pay`
                Value* p = fieldAddr(tmp, st->sum_layout, 1);
                if (fs.size() == 1) {
                    Value* av = coerce(c->args[0].get(), fs[0]);
                    b.CreateStore(av, p);
                    // the sum takes ownership of an owned argument; a borrowed one is retained
                    if (fs[0]->needs_cleanup() && !convOwns(c->args[0].get(), fs[0])) {
                        emitRetainValue(p, fs[0]);
                    }
                } else {
                    TypePtr pt = st->sum_payload[static_cast<size_t>(alt)];
                    for (size_t k = 0; k < fs.size(); ++k) {
                        Value* fp = fieldAddr(p, pt, static_cast<int>(k));
                        Value* av = coerce(c->args[k].get(), fs[k]);
                        b.CreateStore(av, fp);
                        if (fs[k]->needs_cleanup() && !convOwns(c->args[k].get(), fs[k])) {
                            emitRetainValue(fp, fs[k]);
                        }
                    }
                }
            }
            return b.CreateLoad(lt(st), tmp);
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
                emitCheckPendingWith([&] { emitReleasePartialValue(tmp, st); });
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
                    method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params, sig->is_const),
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
            // `super(...)` is no longer a statement: the base constructor is a prologue item and is
            // emitted by emitCtor before the members (SPEC 3.12), so sema has already rejected it.
            fail("LLVM backend: a base-class constructor call reached codegen as a statement", c->line, c->col);
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
                // Immediately-invoked capturing lambda: `([x] (…) => …)(args)`. The environment cannot
                // escape this expression, so it lives on the stack — no allocation, and its RC fields
                // are destroyed right after the call (U41 D5).
                if (auto lm = dynamic_cast<Lambda*>(c->callee.get()); lm && lm->env_ty) {
                    LStruct* objTy = objStruct(lm->env_ty);
                    Value* obj = entryAlloca(objTy, "env", objectAlign(lm->env_ty));
                    b.CreateMemSet(obj, ConstantInt::get(i8(), 0),
                                   ConstantInt::get(i64(), objectSize(lm->env_ty)),
                                   MaybeAlign(objectAlign(lm->env_ty)));
                    b.CreateStore(ConstantInt::get(i64(), 1),
                                  b.CreateGEP(i8(), obj, ConstantInt::get(i64(), 0)));
                    b.CreateStore(descOf(lm->env_ty),
                                  b.CreateGEP(i8(), obj, ConstantInt::get(i64(), 8)));
                    emitCaptureInit(lm, obj);
                    FuncDecl* sf = lm->synth;
                    std::vector<LType*> sptys;
                    for (auto& pt : sf->pty) sptys.push_back(lt(pt));
                    Value* code = mod->getOrInsertFunction(
                                         sf->cname, FunctionType::get(lt(sf->rty), sptys, false))
                                      .getCallee();
                    std::vector<LType*> ptys{ptrTy()};
                    for (auto& pt : ft->fparams) ptys.push_back(lt(pt));
                    std::vector<Value*> vals{obj};
                    for (auto* v : pack.vals) vals.push_back(v);
                    Value* r = b.CreateCall(FunctionType::get(lt(ft->fret), ptys, false), code, vals);
                    releaseTemps(pack);
                    // destroy the captured RC fields (the object storage itself is stack memory)
                    b.CreateCall(
                        mod->getOrInsertFunction(
                            dtor_cname(lm->env_ty->unit, lm->env_ty->name),
                            FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false)),
                        {obj});
                    emitCheckPending();
                    return r;
                }
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
        if (auto m = dynamic_cast<MatchExpr*>(e)) return evalMatch(m);
        if (auto sw = dynamic_cast<SwapExpr*>(e)) {
            // A pure byte exchange: no constructor, destructor or reference-count operation runs, which is
            // both what makes it work for a type whose address must not be taken and why a reference-counted
            // handle is transferred correctly (the counts do not change).
            Value* pa = addrOf(sw->a.get());
            Value* pb = addrOf(sw->b.get());
            const uint64_t n = sizeOf(sw->a->ty);
            if (n > 0) {
                Value* scratch = entryAlloca(llvm::ArrayType::get(i8(), n), "swap.tmp");
                copyMem(scratch, pa, n);
                copyMem(pa, pb, n);
                copyMem(pb, scratch, n);
            }
            return Constant::getNullValue(i32());
        }
        if (auto mv = dynamic_cast<MoveExpr*>(e)) {
            TypePtr t = mv->ty;
            Value* tmp = entryAlloca(lt(t), "move.val", alignOf(t));
            emitMoveInto(tmp, t, mv);
            return b.CreateLoad(lt(t), tmp);
        }
        if (auto ise = dynamic_cast<IsExpr*>(e)) return evalIs(ise);
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
            emitCaptureInit(lm, env);
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
            if (n->kind == "sum_unit") {
                // contextual unit alternative: `Option<int> o = None;`
                const TypePtr& st = n->sum_ty;
                auto* tmp = entryAlloca(lt(st), "sum", alignOf(st));
                zeroMem(tmp, st);
                b.CreateStore(ConstantInt::get(i32(), n->sum_alt),
                              fieldAddr(tmp, st->sum_layout, 0));
                return b.CreateLoad(lt(st), tmp);
            }
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
            if (auto gv = std::get_if<GlobalVar*>(&n->sym); gv && *gv && (*gv)->is_builtin) {
                // A builtin constant has no storage: fold every reference to its value (9.3).
                return constOfValue(e->ty, (*gv)->const_is_float, (*gv)->const_ival, (*gv)->const_fval);
            }
            if (n->kind == "func") {
                auto* f = std::get_if<FuncDecl*>(&n->sym);
                if (!f || !*f) fail("LLVM backend: missing function target", e->line, e->col);
                checkFnValueAbi(*f, e);
                std::vector<LType*> ptys;
                for (auto& pt : (*f)->pty) ptys.push_back(lt(pt));
                return mod->getOrInsertFunction((*f)->cname,
                                                FunctionType::get(lt((*f)->rty), ptys, (*f)->is_varargs))
                    .getCallee();
            }
            return loadAt(lt(e->ty), addrOf(e), unalignedExpr(e));
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            if (m->kind == "sum_unit") {
                // `Value.None`: tag = the alternative, payload zeroed
                const TypePtr& st = m->sum_ty;
                auto* tmp = entryAlloca(lt(st), "sum", alignOf(st));
                zeroMem(tmp, st);
                b.CreateStore(ConstantInt::get(i32(), m->sum_alt),
                              fieldAddr(tmp, st->sum_layout, 0));
                return b.CreateLoad(lt(st), tmp);
            }
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
            if (m->kind == "global") {
                auto* gv = std::get_if<GlobalVar*>(&m->sym);
                if (gv && *gv && (*gv)->is_builtin) {
                    return constOfValue(e->ty, (*gv)->const_is_float, (*gv)->const_ival,
                                        (*gv)->const_fval);
                }
            }
            if (m->kind == "func") {
                auto* f = std::get_if<FuncDecl*>(&m->sym);
                if (!f || !*f) fail("LLVM backend: missing function target", e->line, e->col);
                checkFnValueAbi(*f, e);
                std::vector<LType*> ptys;
                for (auto& pt : (*f)->pty) ptys.push_back(lt(pt));
                return mod->getOrInsertFunction((*f)->cname,
                                                FunctionType::get(lt((*f)->rty), ptys, (*f)->is_varargs))
                    .getCallee();
            }
            if (m->kind == "delegate_code" || m->kind == "delegate_env") {
                // The two words of the `{code, env}` aggregate; `code` is word 0, `env` word 1.
                Value* d = eval(m->obj.get());
                return b.CreateExtractValue(d, {m->kind == "delegate_code" ? 0u : 1u});
            }
            if (m->kind == "field" && m->obj->ty && m->obj->ty->kind == TypeKind::Class) {
                Value* p = eval(m->obj.get());
                emitNullCheck(p, "field access on a null `" + m->obj->ty->name + "` reference");
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
                for (size_t k = 0; k < ix->indices.size(); ++k) {
                    auto& ie = ix->indices[k];
                    const TypePtr& want =
                        k < ix->get_method->params.size() ? ix->get_method->params[k] : nullptr;
                    idxVals.push_back(want ? convertTo(eval(ie.get()), ie->ty, want)
                                           : convertTo(eval(ie.get()), ie->ty, Type::make_prim("isize")));
                }
                Value* r = emitMemberIndex(ix->get_method, addrOf(ix->base.get()), idxVals);
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
        if (auto v = dynamic_cast<Binary*>(e)) {
            Value* r = evalBinary(v);
            if (v->by_ref) return loadAt(lt(e->ty), r, false);
            return r;
        }
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
                                      Type::make_prim("isize"));
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
