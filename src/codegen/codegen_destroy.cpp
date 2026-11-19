// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Reference retain/release of values and the destructor family (Emitter member function definitions).
//
// owns:      `emitReleaseValue` / `emitRetainValue` / `emitReleasePartialValue` (field-by-field RC),
//            and the destructor chain: emitDtor / emitDestroy / emitDtorPartial /
//            emitDestroyPartialFn / emitPartialDestroyCall
// not:       the interface table, the vtable, class descriptors and method bodies
//            (codegen_class.cpp); the RC runtime itself
// pass:      emission
// concept:   codegen-destroy

#include "codegen_impl.hpp"

namespace muc_detail {


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

}  // namespace muc_detail
