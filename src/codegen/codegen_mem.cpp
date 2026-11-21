// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Memory access and initialization into a destination slot (Emitter member function definitions).
//
// owns:      the load/store primitives (storeAt / loadAt / storeValue / zeroMem / copyMem) with the
//            packed-struct alignment rule, and initialization of a type at an address
//            (initInto / initFromExpr: move, copy constructor, aggregate, array, slice, string)
// not:       statement dispatch, expression evaluation, the cleanup / move / copy helpers it calls
// pass:      emission, after every type and body check
// concept:   codegen-memory

#include "codegen_impl.hpp"

namespace muc_detail {

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
            // `S b = move a;`: build the value straight into the destination, then leave the source an
            // empty shell (agents/value-semantics.md §8.3).
            if (auto mv = dynamic_cast<MoveExpr*>(e)) {
                emitMoveInto(addr, ty, mv);
                return;
            }
            // `S b = a;` on a type with a user copy constructor: deep copy through it
            // (agents/value-semantics.md §8.11). A fresh construction (`S(args)`) is handled below, and a
            // value that is already owned (a call result, `new`, `move x`) is transferred as-is instead of
            // being copied a second time.
            if (ty->kind == TypeKind::Struct && ty->has_copy_ctor && !valueOwned(e) &&
                !(dynamic_cast<Call*>(e) && static_cast<Call*>(e)->callee_kind == "struct_ctor")) {
                Value* v = coerce(e, ty);
                Value* src = entryAlloca(lt(ty), "copy.src", alignOf(ty));
                b.CreateStore(v, src);
                emitCopyInto(addr, src, ty);
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
                // A cleanup-free struct has no later owner that would notice the pending flag,
                // so check here.  Structs with cleanup are handled by the LocalVar post-init
                // check or by the enclosing object's destructor.
                if (!st->needs_cleanup() &&
                    mayThrow.mayThrowCname(ctor_cname(st->unit, st->name, *cpty))) {
                    emitCheckPending();
                }
                return;
            }
            if (ty->kind == TypeKind::Array && sizeOf(ty->elem) == 1 &&
                dynamic_cast<StringLit*>(e)) {
                if (std::getenv("MUC_LLVM_TRACE")) std::cerr << "[llvm]       char-array from string\n";
                const std::string& bytes = static_cast<StringLit*>(e)->value;
                Value* sp = stringConst(bytes);   // `const char*` to the static buffer
                zeroMem(addr, ty, unaligned);
                b.CreateMemCpy(addr, MaybeAlign(unaligned ? 1 : alignOf(ty)), sp, MaybeAlign(1),
                               ConstantInt::get(i64(), bytes.size()));
                return;
            }
            initFromExpr(addr, ty, e, unaligned);
            return;
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            // empty braces `{}`: value-initialization - zero, then run the member constructors
            if (ia->items.empty()) {
                zeroMem(addr, ty, unaligned);
                emitCtorGlue(addr, ty, unaligned);
                return;
            }
            // A constructor that throws while an element is being built must not leave the aggregate
            // behind: every part not reached yet is zero (the whole aggregate is zeroed below, a
            // dynamic array's block comes from `calloc`), so releasing the whole thing is zero-value
            // safe (agents/value-semantics.md §5 rule 5). Only the outermost aggregate emits the
            // cleanup — a nested one propagates the pending flag to its host's check, and releasing
            // that host covers the nested part exactly once.
            struct AggDepth {
                int& d;
                explicit AggDepth(int& x) : d(x) { ++d; }
                ~AggDepth() { --d; }
            } aggDepth(aggInitDepth_);
            const bool outermost = aggInitDepth_ == 1;
            BasicBlock* aggUnwind = nullptr;
            auto elementBuilt = [&](const TypePtr& et) {
                if (!outermost || !et || !et->needs_cleanup()) return;
                if (!aggUnwind) {
                    ensureEHSlot();
                    aggUnwind = BasicBlock::Create(ctx, "agg.unwind", curFn);
                }
                BasicBlock* ok = BasicBlock::Create(ctx, "agg.ok", curFn);
                b.CreateCondBr(b.CreateICmpNE(loadPendingFlag(), ConstantInt::get(i32(), 0)), aggUnwind,
                               ok);
                b.SetInsertPoint(ok);
            };
            auto finishAggregate = [&]() {
                if (!aggUnwind) return;
                BasicBlock* cont = BasicBlock::Create(ctx, "agg.done", curFn);
                b.CreateBr(cont);
                b.SetInsertPoint(aggUnwind);
                emitUnwindCurrent([&] { emitReleaseValue(addr, ty, unaligned); });
                b.SetInsertPoint(cont);
            };
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
                // Sema requires every member here, so no member is left to default-initialize
                // (`tests/errors` locks the "missing initializers" diagnostic).
                for (auto& [idx, sub] : slot) {
                    Value* fp = fieldSlot(st, ty, addr, idx);
                    const TypePtr& ft = ty->fields[idx].type;
                    initInto(fp, ft, sub, unaligned || ty->is_packed);
                    elementBuilt(ft);
                }
                finishAggregate();
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
                    elementBuilt(ty->elem);
                    ++i;
                }
                // Elements beyond the initializer are default-initialized.
                for (; i < ty->array_size; ++i) {
                    if (!ty->elem || !ty->elem->needs_ctor()) break;
                    Value* p = gep(lt(ty->elem), inner, ConstantInt::get(i64(), i));
                    emitCtorGlue(p, ty->elem, unaligned);
                    elementBuilt(ty->elem);
                }
                finishAggregate();
                return;
            }
            if (ty->kind == TypeKind::Slice) {
                std::vector<Init*> elems;
                for (auto& it : ia->items) elems.push_back(it.second.get());
                // The elements are built by `initInto` below; running the element default constructor
                // here first was a double construction (and released nothing) (#239).
                Value* h = emitSliceAlloc({ConstantInt::get(i64(), elems.size())}, ty->elem,
                                          /*uninit=*/false, /*construct_elems=*/false);
                // The handle lives in the destination before the elements are built, so the shared
                // unwind block below can release the header and the constructed prefix.
                storeAt(h, addr, unaligned);
                Value* data = sliceDataPtr(h, ty->rank);
                for (size_t i = 0; i < elems.size(); ++i) {
                    Value* p = b.CreateGEP(lt(ty->elem), data, ConstantInt::get(i64(), i));
                    initInto(p, ty->elem, elems[i], unaligned);
                    elementBuilt(ty->elem);
                }
                finishAggregate();
                return;
            }
        }
        fail("LLVM backend: unsupported initializer", init->line, init->col);
    }

    void Emitter::initFromExpr(Value* addr, const TypePtr& ty, Expr* e, bool unaligned) {
        if (!e || !ty) return;
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
        if ((ty->kind == TypeKind::Struct || ty->kind == TypeKind::Sum) && needsCleanup(ty)) {
            // borrowed source: retain field by field (a sum retains its active payload);
            // owned source (a call result / a construction result): ownership transfer
            storeValue(v, addr, ty, unaligned);
            if (!valueOwned(e)) emitRetainValue(addr, ty, unaligned);
            return;
        }
        if (ty->kind == TypeKind::Slice) {
            // borrowed source (another dynamic array): retain; new / array conversion / call result:
            // ownership transfer
            storeValue(v, addr, ty, unaligned);
            if (!convOwns(e, ty)) emitRetainValue(addr, ty, unaligned);
            return;
        }
        if (ty->kind == TypeKind::Delegate) {
            // `{code, env}`: a borrowed source retains the environment, an owned one transfers it
            storeValue(v, addr, ty, unaligned);
            if (!valueOwned(e)) emitRetainValue(addr, ty, unaligned);
            return;
        }
        storeValue(v, addr, ty, unaligned);
    }

}  // namespace muc_detail
