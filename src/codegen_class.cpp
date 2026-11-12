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
        std::string nm = "mu_" + cls->unit + "_ithunk_" + cls->name + "__" +
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
            method_cname(impl.owner->unit, impl.owner->name, impl.name, impl.params),
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
                                  "mu_" + cls->unit + "_itab_" + cls->name);
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
        if (cls->kind == TypeKind::Class && cls->base && cls->base->kind == TypeKind::Class) {
            out = vtableOf(cls->base);
            for (auto& m : cls->methods) {
                bool found = false;
                for (auto* s : out) {
                    if (same_sig(*s, m)) { found = true; break; }
                }
                if (!found) out.push_back(&m);
            }
        } else {
            for (auto& m : cls->methods) out.push_back(&m);
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

    FunctionCallee Emitter::methodFn(const MethodSig* sig) {
        std::vector<LType*> ptys{ptrTy()};
        for (auto& t : sig->params) ptys.push_back(lt(t));
        return mod->getOrInsertFunction(
            method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params),
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
            auto* slot = entryAlloca(lt(pl->rty), sane(pl->name), alignOf(pl->rty));
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
            refScopes.back().push_back(pl.get());
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
                    emitCheckPending();
                }
                Value* dp2 = b.CreateGEP(i8(), curThis, ConstantInt::get(i64(), 8));
                b.CreateStore(descOf(c->ty), dp2);
            }
        }
        ownParams(ctor->param_locals);
        block(ctor->body.get());
        popRefScope();
        finishBody(fn, c->name + " constructor");
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
        size_t own = c->fields.size();
        size_t baseCount = total >= own ? total - own : 0;
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
        std::string nm = "mu_" + c->ty->unit + "_destroy_" + c->name;
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
            b.CreateCall(libFn("_aligned_free", LType::getVoidTy(ctx), {ptrTy()}), {p});
        } else {
            b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {p});
        }
        finishBody(fn, c->name + " destroy function");
    }

    void Emitter::emitClassDesc(ClassDecl* c) {
        std::string destroyName = "mu_" + c->ty->unit + "_destroy_" + c->name;
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
        for (auto& t : ctor->pty) ptys.push_back(lt(t));
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
        block(ctor->body.get());
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
