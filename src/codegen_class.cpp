// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains the interface table, the virtual table and class/struct/interface code generation (Emitter member function definitions).

#include "codegen_impl.hpp"

namespace muc_detail {

    // ------------------------------------------------------------ interface table

    std::string Emitter::sigKey(const MethodSig& m) {
        std::string k = m.name + "(";
        for (auto& p : m.params) k += type_code(p) + ",";
        k += ")";
        return k;
    }

    void Emitter::buildIfaceSlots() {
        for (auto& u : prog.units) {
            for (auto& d : u->decls) {
                auto itf = dynamic_cast<InterfaceDecl*>(d.get());
                if (!itf || !itf->ty) continue;
                for (auto& m : itf->ty->methods) {
                    // Operators are static-only contract members: no itable slot (a `ref` receiver
                    // could not travel through a vtable, and dispatch is not the point).
                    if (m.is_operator) continue;
                    std::string k = sigKey(m);
                    if (islotIndex.count(k)) continue;
                    islotIndex[k] = static_cast<int>(islotSigs.size());
                    islotSigs.push_back(&m);
                }
            }
        }
    }

    const MethodSig* Emitter::ifaceDefault(const TypePtr& t, const MethodSig& slot, int depth) {
        if (!t || depth > 256) return nullptr;
        for (auto& m : t->methods) {
            if (same_sig(m, slot) && !m.is_abstract) return &m;
        }
        for (auto& i : t->interfaces) {
            if (auto r = ifaceDefault(i, slot, depth + 1)) return r;
        }
        return nullptr;
    }

    // default implementation on the class/base class interface chain
    const MethodSig* Emitter::findInterfaceImpl(const TypePtr& cls, const MethodSig& slot) {
        if (const MethodSig* m = findImpl(cls, slot)) return m;
        for (TypePtr cur = cls; cur && cur->kind == TypeKind::Class; cur = cur->base) {
            for (auto& i : cur->interfaces) {
                if (const MethodSig* d = ifaceDefault(i, slot)) return d;
            }
        }
        return nullptr;
    }


    // Interface method calling convention: ret(ptr obj, ptr desc, params...)
    Function* Emitter::ifaceThunk(const TypePtr& cls, const MethodSig& slot, const MethodSig& impl,
                                  int idx) {
        std::string key = sigKey(slot) + "#" + impl.owner->str() + "#" + std::to_string(idx);
        auto it = thunkCache.find(key);
        if (it != thunkCache.end()) return it->second;
        std::string nm = "mu$" + cls->unit + "$ithunk$" + cls->name + "$" +
                         std::to_string(idx);
        std::vector<LType*> ptys{ptrTy(), ptrTy()};
        for (auto& t : slot.params) ptys.push_back(lt(t));
        Function* fn = cast<Function>(mod->getOrInsertFunction(
                                          nm, FunctionType::get(lt(slot.ret), ptys, false))
                                          .getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        IRBuilder<> hb(ctx);
        auto* entry = BasicBlock::Create(ctx, "entry", fn);
        hb.SetInsertPoint(entry);
        std::vector<Value*> args;
        if (impl.owner->kind == TypeKind::Interface) {
            Value* iv = UndefValue::get(ifaceTy);
            iv = hb.CreateInsertValue(iv, fn->getArg(0), {0});
            iv = hb.CreateInsertValue(iv, fn->getArg(1), {1});
            args.push_back(iv);
        } else {
            args.push_back(fn->getArg(0));
        }
        for (size_t i = 0; i < slot.params.size(); ++i)
            args.push_back(fn->getArg(static_cast<unsigned>(i + 2)));
        std::vector<LType*> ip{args.empty() ? ptrTy() : args[0]->getType()};
        for (size_t i = 0; i < slot.params.size(); ++i) ip.push_back(lt(slot.params[i]));
        if (impl.owner->kind == TypeKind::Interface) ip[0] = ifaceTy;
        FunctionCallee target = mod->getOrInsertFunction(
            method_cname(impl.owner->unit, impl.owner->name, impl.name, impl.params, impl.is_const),
            FunctionType::get(lt(impl.ret), ip, false));
        Value* r = hb.CreateCall(target, args);
        if (fn->getReturnType()->isVoidTy()) hb.CreateRetVoid();
        else hb.CreateRet(r);
        thunkCache[key] = fn;
        return fn;
    }

    GlobalVariable* Emitter::emitItable(const TypePtr& cls) {
        if (islotSigs.empty()) return nullptr;
        std::vector<Constant*> ents;
        for (size_t i = 0; i < islotSigs.size(); ++i) {
            const MethodSig* impl = findInterfaceImpl(cls, *islotSigs[i]);
            if (impl) {
                Function* th = ifaceThunk(cls, *islotSigs[i], *impl, static_cast<int>(i));
                ents.push_back(cast<Constant>(th));
            } else {
                ents.push_back(Constant::getNullValue(ptrTy()));
            }
        }
        auto* arrTy = LArray::get(ptrTy(), ents.size());
        return new GlobalVariable(*mod, arrTy, true, GlobalValue::InternalLinkage,
                                  ConstantArray::get(arrTy, ents),
                                  "mu$" + cls->unit + "$itab$" + cls->name);
    }

    // ------------------------------------------------------------ virtual table
    bool Emitter::same_sig(const MethodSig& a, const MethodSig& b) {
        if (a.name != b.name || a.params.size() != b.params.size()) return false;
        for (size_t i = 0; i < a.params.size(); ++i) {
            if (!same_type(a.params[i], b.params[i])) return false;
        }
        return true;
    }


    const std::vector<const MethodSig*>& Emitter::vtableOf(const TypePtr& cls) {
        auto it = vtabMemo2.find(cls.get());
        if (it != vtabMemo2.end()) return it->second;
        std::vector<const MethodSig*> out;
        // Only `virtual` (and abstract) methods take a slot: a plain class method is static-dispatch,
        // so it is reached by a direct call even through a base-class handle (build 160).
        auto add = [&out](const MethodSig& m) {
            if (!m.is_virtual && !m.is_abstract) return;
            for (auto* s : out) {
                if (same_sig(*s, m)) return;
            }
            out.push_back(&m);
        };
        if (cls->kind == TypeKind::Class && cls->base && cls->base->kind == TypeKind::Class) {
            out = vtableOf(cls->base);
            for (auto& m : cls->methods) add(m);
        } else {
            for (auto& m : cls->methods) add(m);
        }
        return vtabMemo2[cls.get()] = std::move(out);
    }

    const MethodSig* Emitter::findImpl(const TypePtr& cls, const MethodSig& sig) {
        TypePtr cur = cls;
        while (cur && cur->kind == TypeKind::Class) {
            for (auto& m : cur->methods) {
                if (same_sig(m, sig)) return m.is_abstract ? nullptr : &m;
            }
            cur = cur->base;
        }
        return nullptr;
    }

    int Emitter::slotOf(const TypePtr& t, const MethodSig* sig) {
        if (!t || t->kind != TypeKind::Class || !sig) return -1;
        const auto& v = vtableOf(t);
        for (size_t i = 0; i < v.size(); ++i) {
            if (same_sig(*v[i], *sig)) return static_cast<int>(i);
        }
        return -1;
    }

    // Every Class type in the program, generic instantiations included (sema appends those to the
    // entry unit's decls).
    void Emitter::collectClassTypes() {
        allClasses.clear();
        for (auto& u : prog.units) {
            for (auto& d : u->decls) {
                auto* c = dynamic_cast<ClassDecl*>(d.get());
                if (c && c->ty) allClasses.push_back(c->ty);
            }
        }
    }

    // Closed-world dispatch decision: true when every possible dynamic type of `recvTy` runs the
    // implementation `sig` names, so the call site can be a direct call instead of
    // `load desc -> load vtable -> load slot -> call %fn`. Sound because no class object crosses the
    // C ABI (no `export` of class types) and every vtable is internal to this module.
    bool Emitter::methodDispatchIsStatic(const TypePtr& recvTy, const MethodSig* sig) {
        if (!recvTy || recvTy->kind != TypeKind::Class || !sig) return false;
        if (sig->is_abstract) return false;  // no implementation to name without the vtable
        if (!findImpl(recvTy, *sig)) return false;
        for (const auto& cls : allClasses) {
            // A `const` view is a copy of the class type with the cv flag set, so identity by pointer
            // would miss every subclass: compare signatures (cv is not part of a class identity).
            if (!cls || same_type_sig(cls, recvTy)) continue;
            bool derived = false;
            for (TypePtr b = cls->base; b; b = b->base) {
                if (same_type_sig(b, recvTy)) {
                    derived = true;
                    break;
                }
            }
            if (!derived) continue;
            for (auto& m : cls->methods) {
                if (same_sig(m, *sig)) return false;  // an override exists somewhere: keep the vtable
            }
        }
        return true;
    }

    FunctionCallee Emitter::methodFn(const MethodSig* sig) {
        std::vector<LType*> ptys{ptrTy()};
        for (auto& t : sig->params) ptys.push_back(lt(t));
        return mod->getOrInsertFunction(
            method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params, sig->is_const),
            FunctionType::get(lt(sig->ret), ptys, false));
    }

    GlobalVariable* Emitter::emitVtable(const TypePtr& cls) {
        const auto& slots = vtableOf(cls);
        if (slots.empty()) return nullptr;
        std::vector<Constant*> ents;
        for (auto* s : slots) {
            const MethodSig* impl = findImpl(cls, *s);
            if (impl) {
                ents.push_back(cast<Constant>(methodFn(impl).getCallee()));
            } else {
                ents.push_back(Constant::getNullValue(ptrTy()));
            }
        }
        auto* arrTy = LArray::get(ptrTy(), ents.size());
        auto* carr = ConstantArray::get(arrTy, ents);
        return new GlobalVariable(*mod, arrTy, /*isConstant=*/true, GlobalValue::InternalLinkage,
                                  carr, class_vtable_name(cls->unit, cls->name));
    }

    // ------------------------------------------------------------ class code generation
    void Emitter::beginBody(Function* fn, const TypePtr& ret) {
        curFn = fn;
        curRetTy = ret;
        curIsEntry = false;
        inNothrow = false;
        ehStack.clear();
        finallyStack.clear();
        abortBases.clear();
        ehLocals.clear();
        refScopes.clear();
        loopScopeDepth.clear();
        stmtTemps.clear();
        ctorProgress.clear();
        allocaBB = BasicBlock::Create(ctx, "allocas", fn);
        bodyBB = BasicBlock::Create(ctx, "entry", fn);
        ab.SetInsertPoint(allocaBB);
        b.SetInsertPoint(bodyBB);
        thisSlot = nullptr;
        curThis = nullptr;
        slots.clear();
    }

    void Emitter::finishBody(Function* fn, const std::string& what) {
        if (!hasTerm(b.GetInsertBlock())) {
            if (fn->getReturnType()->isVoidTy()) {
                b.CreateRetVoid();
            } else {
                b.CreateUnreachable();
            }
        }
        ab.SetInsertPoint(allocaBB);
        ab.CreateBr(bodyBB);
        std::string errs;
        raw_string_ostream os(errs);
        if (verifyFunction(*fn, &os)) {
            throw MuError("LLVM backend: `" + what + "` produced invalid IR: " + os.str());
        }
        curFn = nullptr;
        allocaBB = nullptr;
        bodyBB = nullptr;
        slots.clear();
        refScopes.clear();
        loopScopeDepth.clear();
        stmtTemps.clear();
        ctorProgress.clear();
        ehStack.clear();
        finallyStack.clear();
        abortBases.clear();
        ehLocals.clear();
        curClassTy = nullptr;
        curThis = nullptr;
        thisSlot = nullptr;
    }

    void Emitter::bindParams(Function* fn, size_t first,
                             const std::vector<std::unique_ptr<LocalVar>>& pls) {
        size_t i = first;
        for (auto& pl : pls) {
            if (i >= fn->arg_size()) break;
            // A `ref` / `move` parameter's slot holds the *address*, not the value: it must be pointer
            // sized even when the referent is smaller (storing an 8-byte pointer into a 4-byte alloca is
            // an out-of-bounds write, i.e. UB that lets the optimizer delete the surrounding code).
            const bool indirect = pl->by_ref;
            auto* slot = entryAlloca(indirect ? ptrTy() : lt(pl->rty), sane(pl->name),
                                     indirect ? 8u : alignOf(pl->rty));
            slots[pl.get()] = slot;
            IRBuilder<>::InsertPointGuard g(b);
            b.SetInsertPoint(bodyBB);
            b.CreateStore(fn->getArg(static_cast<unsigned>(i)), slot);
            i++;
        }
    }

    // Parameters are owning local variables (SPEC §4): one reference is retained on entry and released with the scope when the function exits.
    // The caller still passes parameters by value and releases its own temporaries when needed, so the net reference count does not change.
    // (Borrow/escape analysis can later be done without changing the semantics to optimize away the retain/release pairs.)
    void Emitter::ownParams(const std::vector<std::unique_ptr<LocalVar>>& pls) {
        pushRefScope();
        for (auto& pl : pls) {
            if (!pl || !needsCleanup(pl->rty)) continue;
            auto it = slots.find(pl.get());
            if (it == slots.end()) continue;
            if (isRefKind(pl->rty)) {
                emitRetainRef(b.CreateLoad(lt(pl->rty), it->second), pl->rty);
            } else {
                emitRetainValue(it->second, pl->rty);
            }
            registerLocalCleanup(pl.get());
        }
    }

    void Emitter::emitMethod(ClassDecl* c, MethodDecl* m) {
        resetDebugScope();
        if (!m->body) return;
        std::vector<LType*> ptys{ptrTy()};
        for (auto& t : m->pty) ptys.push_back(lt(t));
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(m->cname, FunctionType::get(lt(m->rty), ptys, false))
                .getCallee());
        fn->setLinkage((c->is_public && m->is_public) ? GlobalValue::ExternalLinkage
                                                      : GlobalValue::InternalLinkage);
        curClassTy = c->ty;
        beginBody(fn, m->rty);
        curThis = fn->getArg(0);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(curThis, thisSlot);
        bindParams(fn, 1, m->param_locals);
        ownParams(m->param_locals);
        block(m->body.get());
        popRefScope();
        finishBody(fn, c->name + "." + m->name);
    }

    // Store the member initializer list (`C(...) : x(v) { }`) before the body runs. The destination
    // fields are zeroed at allocation (class) or default-constructed by the member step (struct), so a
    // simple store is correct for trivial field types; a managed field type is retained below.
    void Emitter::emitCtorInitList(Value* self, const TypePtr& ty, CtorDecl* ctor) {
        if (!self || !ty || !ctor || ctor->init_list.empty()) return;
        for (auto& item : ctor->init_list) {
            const int idx = ty->field_index(item.first);
            if (idx < 0) continue;  // sema already reported
            const Field& f = ty->fields[static_cast<size_t>(idx)];
            Value* dst = fieldAddr(self, ty, idx);
            Value* v = eval(item.second.get());
            if (f.type) v = coerce(item.second.get(), f.type);
            b.CreateStore(v, dst);
        }
    }

    void Emitter::emitCtor(ClassDecl* c, CtorDecl* ctor) {
        resetDebugScope();
        std::vector<LType*> ptys{ptrTy()};
        for (auto& t : ctor->pty) ptys.push_back(lt(t));
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(ctor->cname,
                                     FunctionType::get(LType::getVoidTy(ctx), ptys, false))
                .getCallee());
        fn->setLinkage((c->is_public && ctor->is_public) ? GlobalValue::ExternalLinkage
                                                        : GlobalValue::InternalLinkage);
        curClassTy = c->ty;
        beginBody(fn);
        curThis = fn->getArg(0);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(curThis, thisSlot);
        // set this class's desc on entry; it is restored after the super(...) call
        Value* dp = b.CreateGEP(i8(), curThis, ConstantInt::get(i64(), 8));
        b.CreateStore(descOf(c->ty), dp);
        bindParams(fn, 1, ctor->param_locals);

        // super(...) omitted and the base class has a parameterless constructor: call it implicitly following C++ semantics
        if (!ctor->has_explicit_super && c->ty->base && c->ty->base->kind == TypeKind::Class) {
            bool baseDefault = false;
            for (auto& cs : c->ty->base->ctors) {
                if (cs.empty()) { baseDefault = true; break; }
            }
            if (baseDefault) {
                FunctionCallee baseCtor = mod->getOrInsertFunction(
                    ctor_cname(c->ty->base->unit, c->ty->base->name, {}),
                    FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                b.CreateCall(baseCtor, {curThis});
                if (mayThrow.mayThrowCname(ctor_cname(c->ty->base->unit, c->ty->base->name, {}))) {
                    // Nothing is cleaned up here: the desc is still the base's, so the allocating
                    // caller's partial destroy dispatches to the base's partial destructor
                    // (emitDestroyPartialFn). Our own desc is restored only on success.
                    emitCheckPending();
                }
                Value* dp2 = b.CreateGEP(i8(), curThis, ConstantInt::get(i64(), 8));
                b.CreateStore(descOf(c->ty), dp2);
            }
        }
        ownParams(ctor->param_locals);
        // Members come between base and body. With an explicit `super(...)` the base constructor call is a
        // statement in the body, and that statement runs the member step itself (codegen_expr2).
        //
        // The whole constructor is one construction sequence: a throw anywhere in the member step or the
        // body releases the fields this class finished and the one being built (emitMemberInits advances
        // the same entry, so the explicit-`super` path is covered too).
        size_t baseCount = 0;
        if (c->ty->base && c->ty->base->kind == TypeKind::Class) baseCount = c->ty->base->fields.size();
        const size_t progressMark = ctorProgress.size();
        pushCtorProgress(b.CreateLoad(ptrTy(), thisSlot), c->ty, baseCount, false);
        if (!ctor->has_explicit_super) emitMemberInits(b.CreateLoad(ptrTy(), thisSlot), c->ty);
        emitCtorInitList(b.CreateLoad(ptrTy(), thisSlot), c->ty, ctor);
        block(ctor->body.get());
        ctorProgress.resize(progressMark);
        popRefScope();
        finishBody(fn, c->name + " constructor");
    }

    // C++ order: base constructor, then the members in declaration order, then the body. The object was
    // zero-filled at allocation, and a failing member constructor leaves the rest zeroed, so the
    // allocating caller's partial destroy releases everything that did get constructed.
    //
    // The member step advances the constructor's construction sequence (emitCtor opened it): members that
    // finished are destroyed by it, and the member that threw only gets its partial destructor, because
    // its constructor never returned (#24). An explicit `super(...)` runs this step from inside the body,
    // hence the lookup instead of a fresh sequence.
    void Emitter::emitMemberInits(Value* self, const TypePtr& cls) {
        if (!self || !cls || cls->kind != TypeKind::Class) return;
        size_t total = cls->fields.size();
        size_t baseCount = 0;
        if (cls->base && cls->base->kind == TypeKind::Class) baseCount = cls->base->fields.size();
        size_t own = findCtorProgress(cls);
        const size_t mark = ctorProgress.size();
        if (own == kNoCtorProgress) {
            own = pushCtorProgress(self, cls, baseCount, false);
            setCtorProgressNext(own, baseCount);
        }
        for (size_t i = baseCount; i < total; ++i) {
            const Field& f = cls->fields[i];
            if (f.weak || !f.type || !f.type->needs_ctor()) continue;
            setCtorProgressNext(own, i);
            emitCtorGlue(fieldAddr(self, cls, static_cast<int>(i)), f.type);
        }
        // Every own member is complete once the step returns, so the body starts with a fully built level.
        markCtorProgressBody(own);
        ctorProgress.resize(mark);
    }

    void Emitter::emitReleaseValue(Value* addr, const TypePtr& ty, bool unaligned) {
        if (!ty) return;
        if (isRefKind(ty)) {
            emitReleaseRef(loadAt(lt(ty), addr, unaligned), ty);
            return;
        }
        if (ty->kind == TypeKind::Slice) {
            emitSliceRelease(loadAt(lt(ty), addr, unaligned), ty);
            return;
        }
        if (ty->kind == TypeKind::Delegate) {
            // `{code, env}`: the environment is the RC object
            emitReleaseRef(loadAt(lt(ty), addr, unaligned), ty);
            return;
        }
        if (ty->kind == TypeKind::Sum) {
            // release the active alternative's payload: dispatch on the tag
            const TypePtr& st = ty;
            Value* tag = loadAt(i32(), fieldAddr(addr, st->sum_layout, 0), unaligned);
            Value* pay = fieldAddr(addr, st->sum_layout, 1);
            BasicBlock* done = BasicBlock::Create(ctx, "sum.rel.end", curFn);
            auto* sw = b.CreateSwitch(tag, done, static_cast<unsigned>(st->sum_fields.size()));
            for (size_t i = 0; i < st->sum_fields.size(); ++i) {
                const auto& fs = st->sum_fields[i];
                if (fs.empty()) continue;
                bool any = false;
                for (auto& f : fs) any = any || f->needs_cleanup();
                if (!any) continue;
                    BasicBlock* bb = BasicBlock::Create(ctx, "sum.rel", curFn);
                sw->addCase(cast<ConstantInt>(ConstantInt::get(i32(), static_cast<int>(i))), bb);
                b.SetInsertPoint(bb);
                if (fs.size() == 1) {
                    emitReleaseValue(pay, fs[0], unaligned);
                } else {
                    TypePtr pt = st->sum_payload[i];
                    for (size_t k = 0; k < fs.size(); ++k) {
                        emitReleaseValue(fieldAddr(pay, pt, static_cast<int>(k)), fs[k], unaligned);
                    }
                }
                b.CreateBr(done);
            }
            b.SetInsertPoint(done);
            return;
        }
        if (ty->kind == TypeKind::Struct) {
            if (ty->has_dtor) {
                FunctionCallee dtor = mod->getOrInsertFunction(
                    dtor_cname(ty->unit, ty->name),
                    FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                b.CreateCall(dtor, {addr});
                return;
            }
            bool inner = unaligned || ty->is_packed;
            for (size_t k = ty->fields.size(); k > 0; --k) {
                size_t idx = k - 1;
                if (ty->fields[idx].weak) continue;
                emitReleaseValue(fieldAddr(addr, ty, static_cast<int>(idx)),
                                 ty->fields[idx].type, inner);
            }
            return;
        }
        if (ty->kind == TypeKind::Array && needsCleanup(ty->elem)) {
            auto* st = cast<LStruct>(lt(ty));
            for (size_t i = 0; i < ty->array_size; ++i) {
                Value* ep = b.CreateGEP(st, addr, {ConstantInt::get(i64(), 0),
                                                   ConstantInt::get(i32(), 0),
                                                   ConstantInt::get(i64(), i)});
                emitReleaseValue(ep, ty->elem, unaligned);
            }
        }
    }

    // A constructor that exits by exception leaves the object unfinished: its own user destructor
    // body must not run, only the already constructed subobjects are released. The remaining fields
    // are released anyway because destroying a zero value is defined to be a no-op
    // (value-semantics §6.3), which keeps this path free of a per-field "constructed" flag.
    void Emitter::emitReleasePartialValue(Value* addr, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Struct && ty->needs_cleanup()) {
            FunctionCallee partial = mod->getOrInsertFunction(
                dtor_partial_cname(ty->unit, ty->name),
                FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
            b.CreateCall(partial, {addr});
            return;
        }
        if (ty->kind == TypeKind::Array && needsCleanup(ty->elem)) {
            auto* st = cast<LStruct>(lt(ty));
            // Reverse construction order: the elements that were built are destroyed last-first, the way
            // a destructor for a complete array destroys them.
            for (size_t i = ty->array_size; i > 0; --i) {
                Value* ep = b.CreateGEP(st, addr, {ConstantInt::get(i64(), 0),
                                                   ConstantInt::get(i32(), 0),
                                                   ConstantInt::get(i64(), i - 1)});
                emitReleasePartialValue(ep, ty->elem);
            }
            return;
        }
        emitReleaseValue(addr, ty);
    }

    void Emitter::emitRetainValue(Value* addr, const TypePtr& ty, bool unaligned) {
        if (!ty) return;
        if (isRefKind(ty)) {
            emitRetainRef(loadAt(lt(ty), addr, unaligned), ty);
            return;
        }
        if (ty->kind == TypeKind::Slice) {
            emitSliceRetain(loadAt(lt(ty), addr, unaligned));
            return;
        }
        if (ty->kind == TypeKind::Delegate) {
            emitRetainRef(loadAt(lt(ty), addr, unaligned), ty);
            return;
        }
        if (ty->kind == TypeKind::Sum) {
            // retain the active alternative's payload: dispatch on the tag
            const TypePtr& st = ty;
            Value* tag = loadAt(i32(), fieldAddr(addr, st->sum_layout, 0), unaligned);
            Value* pay = fieldAddr(addr, st->sum_layout, 1);
            BasicBlock* done = BasicBlock::Create(ctx, "sum.ret.end", curFn);
            auto* sw = b.CreateSwitch(tag, done, static_cast<unsigned>(st->sum_fields.size()));
            for (size_t i = 0; i < st->sum_fields.size(); ++i) {
                const auto& fs = st->sum_fields[i];
                if (fs.empty()) continue;
                bool any = false;
                for (auto& f : fs) any = any || f->needs_cleanup();
                if (!any) continue;
                BasicBlock* bb = BasicBlock::Create(ctx, "sum.ret", curFn);
                sw->addCase(cast<ConstantInt>(ConstantInt::get(i32(), static_cast<int>(i))), bb);
                b.SetInsertPoint(bb);
                if (fs.size() == 1) {
                    emitRetainValue(pay, fs[0], unaligned);
                } else {
                    TypePtr pt = st->sum_payload[i];
                    for (size_t k = 0; k < fs.size(); ++k) {
                        emitRetainValue(fieldAddr(pay, pt, static_cast<int>(k)), fs[k], unaligned);
                    }
                }
                b.CreateBr(done);
            }
            b.SetInsertPoint(done);
            return;
        }
        if (ty->kind == TypeKind::Struct) {
            bool inner = unaligned || ty->is_packed;
            for (size_t i = 0; i < ty->fields.size(); ++i) {
                if (ty->fields[i].weak) continue;
                emitRetainValue(fieldAddr(addr, ty, static_cast<int>(i)),
                                ty->fields[i].type, inner);
            }
            return;
        }
        if (ty->kind == TypeKind::Array && needsCleanup(ty->elem)) {
            auto* st = cast<LStruct>(lt(ty));
            for (size_t i = 0; i < ty->array_size; ++i) {
                Value* ep = b.CreateGEP(st, addr, {ConstantInt::get(i64(), 0),
                                                   ConstantInt::get(i32(), 0),
                                                   ConstantInt::get(i64(), i)});
                emitRetainValue(ep, ty->elem, unaligned);
            }
        }
    }

    void Emitter::emitDtor(ClassDecl* c) {
        resetDebugScope();
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(dtor_cname(c->ty->unit, c->name),
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(c->is_public ? GlobalValue::ExternalLinkage
                                    : GlobalValue::InternalLinkage);
        curClassTy = c->ty;
        beginBody(fn);
        inNothrow = true;  // destructors must not throw; a pending exception aborts
        abortBases.push_back(ehStack.size());
        curThis = fn->getArg(0);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(curThis, thisSlot);
        if (c->dtor && c->dtor->body) block(c->dtor->body.get());
        abortBases.pop_back();

        // release the reference fields declared by this class (in reverse order); weak fields only unregister
        size_t total = c->ty->fields.size();
        // Base fields come first in the flattened layout. `c->fields` is empty for a synthesized class
        // (a lambda environment, U41), so derive the split from the base class instead of the AST list.
        size_t baseCount = 0;
        if (c->ty->base && c->ty->base->kind == TypeKind::Class) {
            baseCount = c->ty->base->fields.size();
        }
        for (size_t k = total; k > baseCount; --k) {
            size_t idx = k - 1;
            Value* fa = fieldAddr(curThis, c->ty, static_cast<int>(idx));
            if (c->ty->fields[idx].weak) {
                b.CreateCall(ensureWeakUnreg(), {fa});
                continue;
            }
            emitReleaseValue(fa, c->ty->fields[idx].type);
        }
        if (c->ty->base && c->ty->base->kind == TypeKind::Class) {
            FunctionCallee baseDtor = mod->getOrInsertFunction(
                dtor_cname(c->ty->base->unit, c->ty->base->name),
                FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
            b.CreateCall(baseDtor, {curThis});
        }
        finishBody(fn, c->name + " destructor");
    }

    void Emitter::emitDestroy(ClassDecl* c) {
        resetDebugScope();
        std::string nm = destroy_cname(c->ty->unit, c->name);
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(nm, FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        beginBody(fn);
        Value* p = fn->getArg(0);
        // first set all weak references pointing at this object to null
        b.CreateCall(ensureWeakNotify(), {p});
        FunctionCallee dtor = mod->getOrInsertFunction(
            dtor_cname(c->ty->unit, c->name),
            FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
        b.CreateCall(dtor, {p});
        if (classNeedsAlignedAlloc(c->ty)) {
            emitAlignedFree(p);
        } else {
            b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {p});
        }
        finishBody(fn, c->name + " destroy function");
    }

    // Releases exactly what a failed constructor call may have constructed: this class's own
    // fields plus, when the base subobject is complete, the base's full destructor.
    //
    // The constructor already released and zeroed the fields it finished (the construction sequence in
    // emitCtor / emitMemberInits authorizes exactly that), so the own-field release here must treat the
    // remainder as possibly incomplete: a value field gets its partial destructor, never its user
    // destructor body. Releasing an already released (zeroed) field is a no-op.
    void Emitter::emitDtorPartial(ClassDecl* c) {
        resetDebugScope();
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(dtor_partial_cname(c->ty->unit, c->name),
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        curClassTy = c->ty;
        beginBody(fn);
        Value* self = fn->getArg(0);
        size_t total = c->ty->fields.size();
        size_t baseCount = 0;
        if (c->ty->base && c->ty->base->kind == TypeKind::Class) {
            baseCount = c->ty->base->fields.size();
        }
        for (size_t k = total; k > baseCount; --k) {
            size_t idx = k - 1;
            Value* fa = fieldAddr(self, c->ty, static_cast<int>(idx));
            if (c->ty->fields[idx].weak) {
                b.CreateCall(ensureWeakUnreg(), {fa});
                continue;
            }
            emitReleasePartialValue(fa, c->ty->fields[idx].type);
        }
        if (c->ty->base && c->ty->base->kind == TypeKind::Class) {
            // Reaching this level means the base constructor returned normally (see
            // emitDestroyPartialFn), so the base subobject is complete and gets its full destructor.
            FunctionCallee baseDtor = mod->getOrInsertFunction(
                dtor_cname(c->ty->base->unit, c->ty->base->name),
                FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
            b.CreateCall(baseDtor, {self});
        }
        finishBody(fn, c->name + " partial destructor");
    }

    void Emitter::emitDestroyPartialFn(ClassDecl* c) {
        resetDebugScope();
        std::string nm = destroy_partial_cname(c->ty->unit, c->name);
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(nm, FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        beginBody(fn);
        Value* p = fn->getArg(0);
        b.CreateCall(ensureWeakNotify(), {p});
        // The descriptor is written on constructor entry and restored only after `super(...)`
        // returns, so it names the deepest constructor frame that was running when the throw
        // happened: that class's own fields are the ones needing a release, and every ancestor of
        // it was fully constructed. The failing frame cannot do this itself - the exception may
        // escape several constructor frames before the allocating caller runs this cleanup.
        Value* desc = b.CreateLoad(ptrTy(), b.CreateGEP(i8(), p, ConstantInt::get(i64(), 8)));
        std::vector<TypePtr> chain;
        for (TypePtr t = c->ty; t && t->kind == TypeKind::Class; t = t->base) chain.push_back(t);
        BasicBlock* done = BasicBlock::Create(ctx, "partial.done", curFn);
        for (size_t i = 0; i < chain.size(); ++i) {
            const TypePtr& lvl = chain[i];
            FunctionCallee partial = mod->getOrInsertFunction(
                dtor_partial_cname(lvl->unit, lvl->name),
                FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
            if (i + 1 == chain.size()) {
                b.CreateCall(partial, {p});
                break;
            }
            BasicBlock* hit = BasicBlock::Create(ctx, "partial.here", curFn);
            BasicBlock* next = BasicBlock::Create(ctx, "partial.up", curFn);
            b.CreateCondBr(b.CreateICmpEQ(desc, descOf(lvl)), hit, next);
            b.SetInsertPoint(hit);
            b.CreateCall(partial, {p});
            b.CreateBr(done);
            b.SetInsertPoint(next);
        }
        b.CreateBr(done);
        b.SetInsertPoint(done);
        if (classNeedsAlignedAlloc(c->ty)) {
            emitAlignedFree(p);
        } else {
            b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {p});
        }
        finishBody(fn, c->name + " partial destroy function");
    }

    void Emitter::emitPartialDestroyCall(Value* p, const TypePtr& cls) {
        if (!p || !cls) return;
        FunctionCallee callee = mod->getOrInsertFunction(
            destroy_partial_cname(cls->unit, cls->name),
            FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
        b.CreateCall(callee, {p});
    }

    void Emitter::emitClassDesc(ClassDecl* c) {
        std::string destroyName = destroy_cname(c->ty->unit, c->name);
        FunctionCallee dfn =
            mod->getOrInsertFunction(destroyName,
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
        GlobalVariable* vt = emitVtable(c->ty);
        GlobalVariable* it = emitItable(c->ty);
        Constant* baseDesc = Constant::getNullValue(ptrTy());
        if (c->ty->base && c->ty->base->kind == TypeKind::Class) {
            baseDesc = cast<Constant>(descOf(c->ty->base));
        }
        std::vector<Constant*> vals{
            ConstantInt::get(i64(), objectSize(c->ty)),
            cast<Constant>(dfn.getCallee()),
            vt ? cast<Constant>(vt) : Constant::getNullValue(ptrTy()),
            it ? cast<Constant>(it) : Constant::getNullValue(ptrTy()),
            baseDesc,
        };
        auto* gv = mod->getNamedGlobal(class_desc_name(c->ty->unit, c->name));
        if (!gv) {
            gv = new GlobalVariable(*mod, descTy, /*isConstant=*/true,
                                    GlobalValue::InternalLinkage, ConstantStruct::get(descTy, vals),
                                    class_desc_name(c->ty->unit, c->name));
            return;
        }
        gv->setInitializer(ConstantStruct::get(descTy, vals));
    }

    void Emitter::emitStructMethod(StructDecl* sd, MethodDecl* m) {
        resetDebugScope();
        if (!m->body) return;
        std::vector<LType*> ptys{ptrTy()};
        for (auto& t : m->pty) ptys.push_back(lt(t));
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(m->cname, FunctionType::get(lt(m->rty), ptys, false))
                .getCallee());
        fn->setLinkage((sd->is_public && m->is_public) ? GlobalValue::ExternalLinkage
                                                       : GlobalValue::InternalLinkage);
        curClassTy = sd->ty;
        beginBody(fn, m->rty);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(fn->getArg(0), thisSlot);
        bindParams(fn, 1, m->param_locals);
        ownParams(m->param_locals);
        block(m->body.get());
        popRefScope();
        finishBody(fn, sd->name + "." + m->name);
    }

    void Emitter::emitStructCtor(StructDecl* sd, CtorDecl* ctor) {
        resetDebugScope();
        std::vector<LType*> ptys{ptrTy()};
        // A move constructor takes the source by pointer (the body steals from it) and the copy constructor
        // borrows it: both are plain pointers in the signature (agents/value-semantics.md §8.1).
        for (size_t i = 0; i < ctor->params.size() && i < ctor->pty.size(); ++i) {
            ptys.push_back((ctor->params[i]->by_ref || ctor->is_move) ? ptrTy() : lt(ctor->pty[i]));
        }
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(ctor->cname,
                                     FunctionType::get(LType::getVoidTy(ctx), ptys, false))
                .getCallee());
        fn->setLinkage((sd->is_public && ctor->is_public) ? GlobalValue::ExternalLinkage
                                                         : GlobalValue::InternalLinkage);
        curClassTy = sd->ty;
        beginBody(fn);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(fn->getArg(0), thisSlot);
        bindParams(fn, 1, ctor->param_locals);
        ownParams(ctor->param_locals);
        // A member with its own default constructor is constructed before the body (C++ order); the body
        // then assigns whatever it wants on top.
        //
        // The member step and the body are one construction sequence: a throw releases the members that
        // finished and, for the one that threw, only its partial destructor (#24). A base subobject does
        // not exist for a struct, so the sequence owns every field.
        const size_t progressMark = ctorProgress.size();
        const size_t progress = pushCtorProgress(b.CreateLoad(ptrTy(), thisSlot), sd->ty, 0,
                                                sd->ty->is_packed);
        {
            Value* self = b.CreateLoad(ptrTy(), thisSlot);
            for (size_t i = 0; i < sd->ty->fields.size(); ++i) {
                const Field& f = sd->ty->fields[i];
                if (f.weak || !f.type || !f.type->needs_ctor()) continue;
                setCtorProgressNext(progress, i);
                emitCtorGlue(fieldAddr(self, sd->ty, static_cast<int>(i)), f.type);
            }
        }
        markCtorProgressBody(progress);
        emitCtorInitList(b.CreateLoad(ptrTy(), thisSlot), sd->ty, ctor);
        block(ctor->body.get());
        ctorProgress.resize(progressMark);
        popRefScope();
        finishBody(fn, sd->name + " constructor");
    }

    void Emitter::emitStructDtor(StructDecl* sd) {
        resetDebugScope();
        if (!sd->dtor) return;
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(sd->dtor->cname,
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(sd->is_public ? GlobalValue::ExternalLinkage
                                    : GlobalValue::InternalLinkage);
        curClassTy = sd->ty;
        beginBody(fn);
        abortBases.push_back(ehStack.size());
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(fn->getArg(0), thisSlot);
        block(sd->dtor->body.get());
        abortBases.pop_back();
        // after the user destructor body, release the references field by field (in reverse order)
        for (size_t k = sd->ty->fields.size(); k > 0; --k) {
            size_t idx = k - 1;
            if (sd->ty->fields[idx].weak) continue;
            emitReleaseValue(fieldAddr(b.CreateLoad(ptrTy(), thisSlot), sd->ty,
                                       static_cast<int>(idx)),
                             sd->ty->fields[idx].type, sd->ty->is_packed);
        }
        finishBody(fn, sd->name + " destructor");
    }

    // The level's own fields are released as *possibly incomplete* values: the constructor that threw has
    // already destroyed (and zeroed) the subobjects it finished, so what is left here is either the field
    // under construction or untouched storage. A value type therefore gets its partial destructor - its own
    // user destructor body must not run - while handles and references are released normally.
    void Emitter::emitStructDtorPartial(StructDecl* sd) {
        resetDebugScope();
        if (!sd->ty || !sd->ty->needs_cleanup()) return;
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(dtor_partial_cname(sd->ty->unit, sd->name),
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        curClassTy = sd->ty;
        beginBody(fn);
        Value* self = fn->getArg(0);
        for (size_t k = sd->ty->fields.size(); k > 0; --k) {
            size_t idx = k - 1;
            if (sd->ty->fields[idx].weak) continue;
            emitReleasePartialValue(fieldAddr(self, sd->ty, static_cast<int>(idx)),
                                    sd->ty->fields[idx].type);
        }
        finishBody(fn, sd->name + " partial destructor");
    }

    void Emitter::emitInterfaceMethod(InterfaceDecl* itf, MethodDecl* m) {
        resetDebugScope();
        if (!m->body) return;
        std::vector<LType*> ptys{ifaceTy};
        for (auto& t : m->pty) ptys.push_back(lt(t));
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(m->cname, FunctionType::get(lt(m->rty), ptys, false))
                .getCallee());
        fn->setLinkage(itf->is_public ? GlobalValue::ExternalLinkage
                                      : GlobalValue::InternalLinkage);
        curClassTy = itf->ty;
        beginBody(fn, m->rty);
        thisSlot = entryAlloca(ifaceTy, "this");
        b.CreateStore(fn->getArg(0), thisSlot);
        bindParams(fn, 1, m->param_locals);
        ownParams(m->param_locals);
        block(m->body.get());
        popRefScope();
        finishBody(fn, itf->name + "." + m->name);
    }

}  // namespace muc_detail
