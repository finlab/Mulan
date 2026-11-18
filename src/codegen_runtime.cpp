// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains dynamic arrays, the weak registry and local reference cleanup (Emitter member function definitions).

#include "codegen_impl.hpp"

#include <cmath>

#include "llvm/IR/Intrinsics.h"

namespace muc_detail {

    // ------------------------------------------------------------ dynamic arrays (T[] / T[,])
    // rank 1: { i64 rc; i64 len; data... }
    // rank r>=2: { i64 rc; i64 d0; ... d(r-1); data... }
    // There is no capacity field: a dynamic array never grows, so its length is its capacity.

    // PERF: the header is padded to 16 bytes, so element data is 16-byte aligned for every
    // supported element alignment (`alignof` above 16 is not a guarantee, SPEC 3.8).
    uint64_t Emitter::sliceDataOffset(int rank) {
        const uint64_t raw = 8ULL * (1 + (rank <= 1 ? 1 : static_cast<uint64_t>(rank)));
        return (raw + 15ULL) & ~15ULL;
    }

    Value* Emitter::sliceDataPtr(Value* h, int rank) {
        return b.CreateGEP(i8(), h, ConstantInt::get(i64(), sliceDataOffset(rank)));
    }

    Value* Emitter::sliceDimField(Value* h, int axis) {
        return b.CreateLoad(i64(), b.CreateGEP(i8(), h, ConstantInt::get(i64(), 8ULL * (1 + axis))));
    }

    // total element count (rank 1 uses len, rank >= 2 uses the product of the dimensions)
    Value* Emitter::sliceTotalCount(Value* h, int rank) {
        if (rank <= 1) return sliceDimField(h, 0);
        Value* total = sliceDimField(h, 0);
        for (int i = 1; i < rank; ++i) total = b.CreateMul(total, sliceDimField(h, i));
        return total;
    }

    // Calls a runtime function that always throws, then unwinds through the current handler: the cold
    // path shared by every check that reports through `std.exception`.
    void Emitter::emitColdThrow(FunctionCallee callee, const std::vector<Value*>& args, const char* tag) {
        b.CreateCall(callee, args);
        ensureEHSlot();
        const std::string pre(tag);
        BasicBlock* unwind = BasicBlock::Create(ctx, (pre + ".unwind").c_str(), curFn);
        BasicBlock* dead = BasicBlock::Create(ctx, (pre + ".dead").c_str(), curFn);
        b.CreateCondBr(b.CreateICmpNE(loadPendingFlag(), ConstantInt::get(i32(), 0)), unwind, dead);
        b.SetInsertPoint(unwind);
        emitUnwindCurrent();
        b.SetInsertPoint(dead);
        b.CreateUnreachable();
    }

    // Cold path of the bounds check: call std.exception.bounds_fail(index, length, axis), which throws.
    void Emitter::emitBoundsFail(Value* index, Value* length, Value* axis) {
        FunctionCallee callee = mod->getOrInsertFunction(
            mangle_unit("std.exception", "bounds__fail") + "$isize$isize$isize",
            FunctionType::get(LType::getVoidTy(ctx), {i64(), i64(), i64()}, false));
        emitColdThrow(callee, {index, length, axis}, "bf");
    }

    // A shift count must be in [0, width): LLVM's `shl`/`ashr` with any other count is poison, so the
    // checked build reports it instead of letting the optimizer produce a different answer per run
    // (SPEC 8.2 documents the unchecked operation as UB).
    void Emitter::emitShiftCountCheck(Value* count, unsigned width) {
        BasicBlock* ok = BasicBlock::Create(ctx, "sh.ok", curFn);
        BasicBlock* bad = BasicBlock::Create(ctx, "sh.bad", curFn);
        // Unsigned compare: a negative count is a huge unsigned value.
        b.CreateCondBr(b.CreateICmpUGE(count, ConstantInt::get(count->getType(), width)), bad, ok);
        b.SetInsertPoint(bad);
        FunctionCallee callee = mod->getOrInsertFunction(
            mangle_unit("std.exception", "shift__fail") + "$long$int",
            FunctionType::get(LType::getVoidTy(ctx), {i64(), i32()}, false));
        emitColdThrow(callee,
                      {b.CreateSExtOrTrunc(count, i64()), ConstantInt::get(i32(), width)}, "sh");
        b.SetInsertPoint(ok);
    }

    // A float->int `as` must be in the target type's range before `fptosi`/`fptoui` (which are poison
    // outside it). NaN fails every comparison, so the range test alone rejects it as well.
    void Emitter::emitFloatToIntCheck(Value* value, const TypePtr& to) {
        const unsigned bits = static_cast<unsigned>(sizeOf(to) * 8);
        const bool sgn = to->is_signed();
        const double lo = sgn ? -std::ldexp(1.0, static_cast<int>(bits) - 1) : 0.0;
        const double hi = sgn ? std::ldexp(1.0, static_cast<int>(bits) - 1)
                              : std::ldexp(1.0, static_cast<int>(bits));
        BasicBlock* ok = BasicBlock::Create(ctx, "cv.ok", curFn);
        BasicBlock* bad = BasicBlock::Create(ctx, "cv.bad", curFn);
        Value* ge = b.CreateFCmpOGE(value, ConstantFP::get(value->getType(), lo));
        Value* lt = b.CreateFCmpOLT(value, ConstantFP::get(value->getType(), hi));
        b.CreateCondBr(b.CreateAnd(ge, lt), ok, bad);
        b.SetInsertPoint(bad);
        FunctionCallee callee = mod->getOrInsertFunction(
            mangle_unit("std.exception", "cast__fail") + "$double$int",
            FunctionType::get(LType::getVoidTy(ctx), {b.getDoubleTy(), i32()}, false));
        emitColdThrow(callee, {value, ConstantInt::get(i32(), bits)}, "cv");
        b.SetInsertPoint(ok);
    }

    // Per-axis bounds checks for `s[i]` / `s[i,j]`; returns the linear index.
    Value* Emitter::emitSliceIndexChecked(Value* h, const std::vector<Value*>& idxs, const TypePtr& bt) {
        for (size_t k = 0; k < idxs.size(); ++k) {
            Value* dim = emitSliceDim(h, static_cast<int>(k));
            BasicBlock* ok = BasicBlock::Create(ctx, "idx.ok", curFn);
            BasicBlock* bad = BasicBlock::Create(ctx, "idx.bad", curFn);
            b.CreateCondBr(b.CreateICmpUGE(idxs[k], dim), bad, ok);
            b.SetInsertPoint(bad);
            emitBoundsFail(idxs[k], dim, ConstantInt::get(i64(), static_cast<int64_t>(k)));
            b.SetInsertPoint(ok);
        }
        Value* lin = idxs.empty() ? ConstantInt::get(i64(), 0) : idxs[0];
        for (size_t k = 1; k < idxs.size(); ++k) {
            Value* dk = sliceDimField(h, static_cast<int>(k));
            lin = b.CreateAdd(b.CreateMul(lin, dk), idxs[k]);
        }
        return lin;
    }

    // len(a, k): a null handle yields 0; k is a compile-time constant
    Value* Emitter::emitSliceDim(Value* h, int axis) {
        BasicBlock* nullBB = BasicBlock::Create(ctx, "alen.null", curFn);
        BasicBlock* okBB = BasicBlock::Create(ctx, "alen.ok", curFn);
        BasicBlock* doneBB = BasicBlock::Create(ctx, "alen.done", curFn);
        b.CreateCondBr(b.CreateICmpEQ(h, Constant::getNullValue(ptrTy())), nullBB, okBB);
        b.SetInsertPoint(nullBB);
        b.CreateBr(doneBB);
        b.SetInsertPoint(okBB);
        Value* len = sliceDimField(h, axis);
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
        auto* phi = b.CreatePHI(i64(), 2);
        phi->addIncoming(ConstantInt::get(i64(), 0), nullBB);
        phi->addIncoming(len, okBB);
        return phi;
    }

    // len(a, k): k is a runtime expression (with an out-of-bounds check)
    Value* Emitter::emitSliceDimDynamic(Value* h, Value* axis, int rank) {
        BasicBlock* checkBB = BasicBlock::Create(ctx, "adyn.check", curFn);
        BasicBlock* nullBB = BasicBlock::Create(ctx, "adyn.null", curFn);
        BasicBlock* badBB = BasicBlock::Create(ctx, "adyn.bad", curFn);
        BasicBlock* okBB = BasicBlock::Create(ctx, "adyn.ok", curFn);
        BasicBlock* doneBB = BasicBlock::Create(ctx, "adyn.done", curFn);
        b.CreateCondBr(b.CreateICmpEQ(h, Constant::getNullValue(ptrTy())), nullBB, checkBB);
        b.SetInsertPoint(nullBB);
        b.CreateBr(doneBB);
        b.SetInsertPoint(checkBB);
        b.CreateCondBr(b.CreateICmpUGE(axis, ConstantInt::get(i64(), rank)), badBB, okBB);
        b.SetInsertPoint(badBB);
        b.CreateCall(abortTrap(), {});
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
        Value* off = b.CreateAdd(ConstantInt::get(i64(), 8),
                                 b.CreateMul(axis, ConstantInt::get(i64(), 8)));
        Value* v = b.CreateLoad(i64(), b.CreateGEP(i8(), h, off));
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
        auto* phi = b.CreatePHI(i64(), 2);
        phi->addIncoming(ConstantInt::get(i64(), 0), nullBB);
        phi->addIncoming(v, okBB);
        return phi;
    }

    // total element count (a null handle yields 0)
    Value* Emitter::emitSliceTotal(Value* h, int rank) {
        if (rank <= 1) return emitSliceDim(h, 0);
        BasicBlock* nullBB = BasicBlock::Create(ctx, "atot.null", curFn);
        BasicBlock* okBB = BasicBlock::Create(ctx, "atot.ok", curFn);
        BasicBlock* doneBB = BasicBlock::Create(ctx, "atot.done", curFn);
        b.CreateCondBr(b.CreateICmpEQ(h, Constant::getNullValue(ptrTy())), nullBB, okBB);
        b.SetInsertPoint(nullBB);
        b.CreateBr(doneBB);
        b.SetInsertPoint(okBB);
        Value* total = sliceDimField(h, 0);
        for (int i = 1; i < rank; ++i) total = b.CreateMul(total, sliceDimField(h, i));
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
        auto* phi = b.CreatePHI(i64(), 2);
        phi->addIncoming(ConstantInt::get(i64(), 0), nullBB);
        phi->addIncoming(total, okBB);
        return phi;
    }

    // Allocate a dynamic array; dims holds the size of each dimension. Unless `uninit` is set the
    // whole block is zeroed (SPEC 3.3): a null handle is load-bearing for reference elements, and
    // `calloc` lets libc skip the zeroing entirely when the pages come fresh from the OS.
    Value* Emitter::emitSliceAlloc(const std::vector<Value*>& dims, const TypePtr& elem, bool uninit) {
        int rank = static_cast<int>(dims.size());
        uint64_t es = sizeOf(elem);
        // INVARIANT: every dimension is a non-negative i64 and the byte count does not overflow.
        // A negative dimension would sign-extend into a huge `usize` length while the wrapped byte
        // count makes `malloc` hand back a tiny block, so the bounds check (`index < length`) could
        // never fire; a wrapped byte count would under-allocate for a legitimate length. Both are
        // "the allocation cannot be satisfied" (SPEC 6.6), so they abort like a failed malloc.
        Value* bad = b.CreateICmpSLT(dims[0], ConstantInt::get(i64(), 0));
        for (int i = 1; i < rank; ++i) {
            bad = b.CreateOr(bad, b.CreateICmpSLT(dims[i], ConstantInt::get(i64(), 0)));
        }
        BasicBlock* dimsBadBB = BasicBlock::Create(ctx, "arr.baddims", curFn);
        BasicBlock* dimsOkBB = BasicBlock::Create(ctx, "arr.dims", curFn);
        b.CreateCondBr(bad, dimsBadBB, dimsOkBB);
        b.SetInsertPoint(dimsBadBB);
        b.CreateCall(abortTrap(), {});
        b.CreateUnreachable();
        b.SetInsertPoint(dimsOkBB);

        Value* total = ConstantInt::get(i64(), 1);
        Value* overflow = nullptr;
        // Exact overflow detection (llvm.umul.with.overflow): a wrapped byte count either
        // under-allocates a legitimate length or makes the header advertise a length the block
        // cannot hold. Either way the allocation cannot be satisfied, so it aborts like malloc.
        auto checkedMul = [&](Value* a, Value* c) {
            Value* pair = b.CreateIntrinsic(llvm::Intrinsic::umul_with_overflow, {i64()}, {a, c});
            Value* ov = b.CreateExtractValue(pair, 1);
            overflow = overflow ? b.CreateOr(overflow, ov) : ov;
            return b.CreateExtractValue(pair, 0);
        };
        for (Value* d : dims) total = checkedMul(total, d);
        if (es != 1) total = checkedMul(total, ConstantInt::get(i64(), es));
        Value* bytes = total;
        {
            Value* pair = b.CreateIntrinsic(llvm::Intrinsic::uadd_with_overflow, {i64()},
                                            {total, ConstantInt::get(i64(), sliceDataOffset(rank))});
            Value* ov = b.CreateExtractValue(pair, 1);
            overflow = b.CreateOr(overflow, ov);
            bytes = b.CreateExtractValue(pair, 0);
        }
        BasicBlock* sizeBadBB = BasicBlock::Create(ctx, "arr.badsize", curFn);
        BasicBlock* sizeOkBB = BasicBlock::Create(ctx, "arr.size", curFn);
        b.CreateCondBr(overflow, sizeBadBB, sizeOkBB);
        b.SetInsertPoint(sizeBadBB);
        b.CreateCall(abortTrap(), {});
        b.CreateUnreachable();
        b.SetInsertPoint(sizeOkBB);
        Value* h = uninit ? b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {bytes})
                          : b.CreateCall(libFn("calloc", ptrTy(), {i64(), i64()}),
                                         {ConstantInt::get(i64(), 1), bytes});
        BasicBlock* okBB = BasicBlock::Create(ctx, "arr.ok", curFn);
        BasicBlock* failBB = BasicBlock::Create(ctx, "arr.fail", curFn);
        b.CreateCondBr(b.CreateICmpEQ(h, Constant::getNullValue(ptrTy())), failBB, okBB);
        b.SetInsertPoint(failBB);
        b.CreateCall(abortTrap(), {});
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
        // `calloc` already zeroed the block; the header fields are written below either way, so an
        // `uninit` block only leaves the element data undefined.
        b.CreateStore(ConstantInt::get(i64(), 1),
                      b.CreateGEP(i8(), h, ConstantInt::get(i64(), kArrRc)));
        for (int i = 0; i < rank; ++i) {
            b.CreateStore(dims[i],
                          b.CreateGEP(i8(), h, ConstantInt::get(i64(), 8ULL * (1 + i))));
        }
        // Element construction (agents/value-semantics.md §7): `new T[n]` runs the default constructor of
        // every element. Zeroing already covers POD fields and the handles, so only a type with a
        // constructor to run reaches this loop. A throwing constructor releases the elements built so far
        // and the block before unwinding continues (SPEC 3.14 partial construction); the element that
        // failed is released by the ctor-progress cleanup, not here. Order on that path is prefix first,
        // then the failed element — observable only in destructor side effects of a throwing ctor.
        if (!uninit && elem->needs_ctor()) {
            Value* data = sliceDataPtr(h, rank);
            Value* count = sliceTotalCount(h, rank);
            AllocaInst* idx = entryAlloca(i64(), "arr.ctor.i");
            b.CreateStore(ConstantInt::get(i64(), 0), idx);
            BasicBlock* condBB = BasicBlock::Create(ctx, "arr.ctor.cond", curFn);
            BasicBlock* bodyBB = BasicBlock::Create(ctx, "arr.ctor.body", curFn);
            BasicBlock* doneBB = BasicBlock::Create(ctx, "arr.ctor.done", curFn);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            Value* i0 = b.CreateLoad(i64(), idx);
            b.CreateCondBr(b.CreateICmpULT(i0, count), bodyBB, doneBB);
            b.SetInsertPoint(bodyBB);
            Value* i1 = b.CreateLoad(i64(), idx);
            Value* pe = b.CreateGEP(lt(elem), data, i1);
            emitCtorGlue(pe, elem, false, [&] {
                AllocaInst* j = entryAlloca(i64(), "arr.ctor.j");
                b.CreateStore(i1, j);
                BasicBlock* rcBB = BasicBlock::Create(ctx, "arr.ctor.rc", curFn);
                BasicBlock* rbBB = BasicBlock::Create(ctx, "arr.ctor.rb", curFn);
                BasicBlock* reBB = BasicBlock::Create(ctx, "arr.ctor.re", curFn);
                b.CreateBr(rcBB);
                b.SetInsertPoint(rcBB);
                Value* j0 = b.CreateLoad(i64(), j);
                b.CreateCondBr(b.CreateICmpUGT(j0, ConstantInt::get(i64(), 0)), rbBB, reBB);
                b.SetInsertPoint(rbBB);
                Value* j1 = b.CreateLoad(i64(), j);
                Value* jm = b.CreateSub(j1, ConstantInt::get(i64(), 1));
                b.CreateStore(jm, j);
                emitReleaseValue(b.CreateGEP(lt(elem), data, jm), elem);
                b.CreateBr(rcBB);
                b.SetInsertPoint(reBB);
                b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {h});
            });
            b.CreateStore(b.CreateAdd(i1, ConstantInt::get(i64(), 1)), idx);
            b.CreateBr(condBB);
            b.SetInsertPoint(doneBB);
        }
        return h;
    }

    Value* Emitter::emitDynArrayAlloc(Value* n, const TypePtr& elem) {
        return emitSliceAlloc({n}, elem);
    }

    void Emitter::emitSliceRetain(Value* h) { emitRetain(h); }

    FunctionCallee Emitter::abortTrap() {
        if (abortTrapFn) {
            return FunctionCallee(abortTrapFn->getFunctionType(), abortTrapFn);
        }
        auto* ft = FunctionType::get(LType::getVoidTy(ctx), {}, false);
        Function* fn = Function::Create(ft, GlobalValue::InternalLinkage, "mu_abort", *mod);
        abortTrapFn = fn;

        Function* savedFn = curFn;
        IRBuilder<>::InsertPoint savedIP = b.saveIP();
        curFn = fn;
        b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
        // fflush(NULL) flushes every output stream (C standard), so nothing is left for the CRT to
        // write a second time while the process goes down.
        b.CreateCall(libFn("fflush", i32(), {ptrTy()}), {Constant::getNullValue(ptrTy())});
        b.CreateCall(libFn("_exit", LType::getVoidTy(ctx), {i32()}),
                     {ConstantInt::get(i32(), 3)});
        b.CreateUnreachable();
        curFn = savedFn;
        b.restoreIP(savedIP);
        return FunctionCallee(ft, fn);
    }

    // element release function: release the elements one by one (when they hold references), then free the header
    Function* Emitter::ensureSliceFree(const TypePtr& elem, int rank) {
        std::string key = type_code(elem) + "#" + std::to_string(rank);
        for (auto& ch : key)
            if (ch == '.') ch = '_';
        auto it = sliceFreeCache.find(key);
        if (it != sliceFreeCache.end()) return it->second;
        auto* ft = FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false);
        Function* fn = Function::Create(ft, GlobalValue::InternalLinkage,
                                        "mu_arr_free_" + key, *mod);
        sliceFreeCache[key] = fn;

        Function* savedFn = curFn;
        IRBuilder<>::InsertPoint savedIP = b.saveIP();
        curFn = fn;
        BasicBlock* entryBB = BasicBlock::Create(ctx, "entry", fn);
        b.SetInsertPoint(entryBB);
        Value* h = fn->getArg(0);
        if (elem->needs_cleanup()) {
            Value* total = sliceTotalCount(h, rank);
            Value* data = sliceDataPtr(h, rank);
            auto* idx = b.CreateAlloca(i64());
            b.CreateStore(ConstantInt::get(i64(), 0), idx);
            BasicBlock* condBB = BasicBlock::Create(ctx, "cond", fn);
            BasicBlock* bodyBB = BasicBlock::Create(ctx, "body", fn);
            BasicBlock* endBB = BasicBlock::Create(ctx, "end", fn);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            Value* i0 = b.CreateLoad(i64(), idx);
            b.CreateCondBr(b.CreateICmpULT(i0, total), bodyBB, endBB);
            b.SetInsertPoint(bodyBB);
            Value* i1 = b.CreateLoad(i64(), idx);
            Value* p = b.CreateGEP(lt(elem), data, i1);
            emitReleaseValue(p, elem);
            b.CreateStore(b.CreateAdd(i1, ConstantInt::get(i64(), 1)), idx);
            b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
        }
        b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {h});
        b.CreateRetVoid();
        b.restoreIP(savedIP);
        curFn = savedFn;
        return fn;
    }

    void Emitter::emitSliceRelease(Value* h, const TypePtr& sliceTy) {
        if (!h) return;
        BasicBlock* bodyBB = BasicBlock::Create(ctx, "arel.body", curFn);
        BasicBlock* destroyBB = BasicBlock::Create(ctx, "arel.destroy", curFn);
        BasicBlock* doneBB = BasicBlock::Create(ctx, "arel.done", curFn);
        b.CreateCondBr(b.CreateICmpEQ(h, Constant::getNullValue(ptrTy())), doneBB, bodyBB);
        b.SetInsertPoint(bodyBB);
        Value* rcp = b.CreateGEP(i8(), h, ConstantInt::get(i64(), kArrRc));
        // Release decrement + acquire fence before freeing: same rule as class objects (the array
        // header can be shared across tasks/threads just like a class handle). `--single-thread`
        // drops both, like mu_rt_release.
        Value* old = nullptr;
        if (singleThread) {
            old = b.CreateLoad(i64(), rcp);
            b.CreateStore(b.CreateSub(old, ConstantInt::get(i64(), 1)), rcp);
        } else {
            old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rcp, ConstantInt::get(i64(), 1),
                                    MaybeAlign(8), llvm::AtomicOrdering::Release);
        }
        b.CreateCondBr(b.CreateICmpEQ(old, ConstantInt::get(i64(), 1)), destroyBB, doneBB);
        b.SetInsertPoint(destroyBB);
        if (!singleThread) b.CreateFence(llvm::AtomicOrdering::Acquire);
        b.CreateCall(ensureSliceFree(sliceTy->elem, sliceTy->rank), {h});
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
    }

    // fixed-size array -> dynamic array (copied by nested dimension; when elements hold references, retain each element)
    Value* Emitter::emitDynArrayFromArray(Value* arrAddr, const TypePtr& arrTy,
                                          const TypePtr& dstTy) {
        std::vector<uint64_t> dims;
        TypePtr base = arrTy;
        while (base && base->kind == TypeKind::Array) {
            dims.push_back(base->array_size);
            base = base->elem;
        }
        std::vector<Value*> dv;
        uint64_t total = 1;
        for (uint64_t d : dims) {
            dv.push_back(ConstantInt::get(i64(), d));
            total *= d;
        }
        Value* h = emitSliceAlloc(dv, base);
        Value* dst = sliceDataPtr(h, static_cast<int>(dims.size()));
        Value* src = arrAddr;  // the only field of the array wrapper struct is at offset 0
        if (base->needs_cleanup()) {
            for (uint64_t i = 0; i < total; ++i) {
                Value* d = b.CreateGEP(lt(base), dst, ConstantInt::get(i64(), i));
                Value* s = b.CreateGEP(lt(base), src, ConstantInt::get(i64(), i));
                copyMem(d, s, sizeOf(base));
                emitRetainValue(d, base);
            }
        } else if (total > 0) {
            copyMem(dst, src, total * sizeOf(base));
        }
        return h;
    }

    // ------------------------------------------------------------ weak registry
    GlobalVariable* Emitter::weakHeadsGlobal() { return mod->getNamedGlobal("mu_weak_heads"); }
    GlobalVariable* Emitter::weakLocksGlobal() { return mod->getNamedGlobal("mu_weak_locks"); }

    Value* Emitter::weakShardSlot(IRBuilder<>& hb, GlobalVariable* arr, LType* elemTy, Value* target) {
        // Sharding by address keeps every node next to its target: `unreg` and the death notify both
        // derive the same shard from the target pointer, without adding a field to the object header.
        Value* n = hb.CreateLShr(hb.CreatePtrToInt(target, i64()), ConstantInt::get(i64(), 4));
        Value* idx = hb.CreateAnd(n, ConstantInt::get(i64(), kWeakShards - 1));
        return hb.CreateGEP(llvm::ArrayType::get(elemTy, kWeakShards), arr,
                            {ConstantInt::get(i64(), 0), idx});
    }

    void Emitter::emitWeakLock(IRBuilder<>& hb, Value* lockSlot) {
        // `--single-thread`: the registry is only touched by this thread, so the spin lock is dead
        // weight on every destruction (see EmitOptions::single_thread).
        if (singleThread) return;
        auto* fn = hb.GetInsertBlock()->getParent();
        auto* spin = BasicBlock::Create(ctx, "weak.spin", fn);
        auto* got = BasicBlock::Create(ctx, "weak.got", fn);
        hb.CreateBr(spin);
        hb.SetInsertPoint(spin);
        // Acquire on lock, Release on unlock: the registry must not be read while another thread
        // is in the middle of modifying it (this used to be relaxed, which only worked on x86-TSO).
        auto* old = hb.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, lockSlot,
                                       ConstantInt::get(i32(), 1), MaybeAlign(4),
                                       llvm::AtomicOrdering::Acquire);
        hb.CreateCondBr(hb.CreateICmpEQ(old, ConstantInt::get(i32(), 0)), got, spin);
        hb.SetInsertPoint(got);
    }

    void Emitter::emitWeakUnlock(IRBuilder<>& hb, Value* lockSlot) {
        if (singleThread) return;
        hb.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, lockSlot,
                           ConstantInt::get(i32(), 0), MaybeAlign(4),
                           llvm::AtomicOrdering::Release);
    }

    void Emitter::emitWeakEmptyCheck(IRBuilder<>& hb, Value* headSlot, BasicBlock* emptyBB,
                                     BasicBlock* busyBB) {
        // Acquire: pairs with the release store of `mu_rt_weak_reg`, so a non-null head also makes the
        // node it points at visible. Under `--single-thread` nothing else can touch the registry, so
        // the ordering (and the atomic itself) has nothing to order against.
        auto* head = hb.CreateAlignedLoad(ptrTy(), headSlot, MaybeAlign(8));
        if (!singleThread) head->setAtomic(llvm::AtomicOrdering::Acquire);
        hb.CreateCondBr(hb.CreateICmpEQ(head, Constant::getNullValue(ptrTy())), emptyBB, busyBB);
    }

    Function* Emitter::ensureWeakReg() {
        if (weakRegFn) return weakRegFn;
        auto* ft = FunctionType::get(LType::getVoidTy(ctx), {ptrTy(), ptrTy()}, false);
        auto* fn = cast<Function>(mod->getOrInsertFunction("mu_rt_weak_reg", ft).getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        IRBuilder<> hb(ctx);
        auto* entry = BasicBlock::Create(ctx, "entry", fn);
        hb.SetInsertPoint(entry);
        auto* H = weakShardSlot(hb, weakHeadsGlobal(), ptrTy(), fn->getArg(0));
        auto* L = weakShardSlot(hb, weakLocksGlobal(), i32(), fn->getArg(0));
        emitWeakLock(hb, L);
        auto* node = hb.CreateCall(libFn("malloc", ptrTy(), {i64()}),
                                   {ConstantInt::get(i64(), 24)});
        hb.CreateStore(fn->getArg(0), hb.CreateStructGEP(weakNodeTy, node, 0));  // target
        hb.CreateStore(fn->getArg(1), hb.CreateStructGEP(weakNodeTy, node, 1));  // field
        auto* head = hb.CreateLoad(ptrTy(), H);
        hb.CreateStore(head, hb.CreateStructGEP(weakNodeTy, node, 2));  // next
        hb.CreateStore(node, H);
        emitWeakUnlock(hb, L);
        hb.CreateRetVoid();
        weakRegFn = fn;
        return fn;
    }

    Function* Emitter::ensureWeakUnreg() {
        if (weakUnregFn) return weakUnregFn;
        auto* ft = FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false);
        auto* fn = cast<Function>(mod->getOrInsertFunction("mu_rt_weak_unreg", ft).getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        IRBuilder<> hb(ctx);
        auto* entry = BasicBlock::Create(ctx, "entry", fn);
        auto* loop = BasicBlock::Create(ctx, "loop", fn);
        auto* body = BasicBlock::Create(ctx, "body", fn);
        auto* remove = BasicBlock::Create(ctx, "remove", fn);
        auto* rmHead = BasicBlock::Create(ctx, "rm.head", fn);
        auto* rmNode = BasicBlock::Create(ctx, "rm.node", fn);
        auto* removed = BasicBlock::Create(ctx, "removed", fn);
        auto* keep = BasicBlock::Create(ctx, "keep", fn);
        // Two distinct exits: the shard was empty (no lock held) and the locked path, which must
        // release the spin lock. Sharing one block made the empty path execute an unlock it never
        // took — an `xchg 0` on a global cache line, on every object destruction.
        auto* emptyDone = BasicBlock::Create(ctx, "weak.empty", fn);
        auto* lockedDone = BasicBlock::Create(ctx, "weak.done", fn);

        hb.SetInsertPoint(entry);
        auto* H = weakShardSlot(hb, weakHeadsGlobal(), ptrTy(), fn->getArg(0));
        auto* L = weakShardSlot(hb, weakLocksGlobal(), i32(), fn->getArg(0));
        auto* lockBB = BasicBlock::Create(ctx, "weak.lock", fn);
        emitWeakEmptyCheck(hb, H, emptyDone, lockBB);
        hb.SetInsertPoint(lockBB);
        emitWeakLock(hb, L);
        auto* curS = hb.CreateAlloca(ptrTy());
        auto* prevS = hb.CreateAlloca(ptrTy());
        hb.CreateStore(hb.CreateLoad(ptrTy(), H), curS);
        hb.CreateStore(Constant::getNullValue(ptrTy()), prevS);
        hb.CreateBr(loop);

        hb.SetInsertPoint(loop);
        auto* cur = hb.CreateLoad(ptrTy(), curS);
        hb.CreateCondBr(hb.CreateICmpEQ(cur, Constant::getNullValue(ptrTy())), lockedDone, body);

        hb.SetInsertPoint(body);
        auto* f = hb.CreateLoad(ptrTy(), hb.CreateStructGEP(weakNodeTy, cur, 1));
        auto* nxt = hb.CreateLoad(ptrTy(), hb.CreateStructGEP(weakNodeTy, cur, 2));
        hb.CreateCondBr(hb.CreateICmpEQ(f, fn->getArg(0)), remove, keep);

        hb.SetInsertPoint(remove);
        auto* prev = hb.CreateLoad(ptrTy(), prevS);
        hb.CreateCondBr(hb.CreateICmpEQ(prev, Constant::getNullValue(ptrTy())), rmHead, rmNode);
        hb.SetInsertPoint(rmHead);
        hb.CreateStore(nxt, H);
        hb.CreateBr(removed);
        hb.SetInsertPoint(rmNode);
        hb.CreateStore(nxt, hb.CreateStructGEP(weakNodeTy, prev, 2));
        hb.CreateBr(removed);
        hb.SetInsertPoint(removed);
        hb.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {cur});
        hb.CreateStore(nxt, curS);
        hb.CreateBr(loop);

        hb.SetInsertPoint(keep);
        hb.CreateStore(cur, prevS);
        hb.CreateStore(nxt, curS);
        hb.CreateBr(loop);

        hb.SetInsertPoint(lockedDone);
        emitWeakUnlock(hb, L);
        hb.CreateRetVoid();

        hb.SetInsertPoint(emptyDone);
        hb.CreateRetVoid();
        weakUnregFn = fn;
        return fn;
    }

    Function* Emitter::ensureWeakNotify() {
        if (weakNotifyFn) return weakNotifyFn;
        auto* ft = FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false);
        auto* fn = cast<Function>(mod->getOrInsertFunction("mu_rt_weak_notify", ft).getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        IRBuilder<> hb(ctx);
        auto* entry = BasicBlock::Create(ctx, "entry", fn);
        auto* loop = BasicBlock::Create(ctx, "loop", fn);
        auto* body = BasicBlock::Create(ctx, "body", fn);
        auto* hit = BasicBlock::Create(ctx, "hit", fn);
        auto* nullStore = BasicBlock::Create(ctx, "nullstore", fn);
        auto* remove = BasicBlock::Create(ctx, "remove", fn);
        auto* rmHead = BasicBlock::Create(ctx, "rm.head", fn);
        auto* rmNode = BasicBlock::Create(ctx, "rm.node", fn);
        auto* removed = BasicBlock::Create(ctx, "removed", fn);
        auto* keep = BasicBlock::Create(ctx, "keep", fn);
        auto* emptyDone = BasicBlock::Create(ctx, "weak.empty", fn);
        auto* lockedDone = BasicBlock::Create(ctx, "weak.done", fn);

        hb.SetInsertPoint(entry);
        auto* H = weakShardSlot(hb, weakHeadsGlobal(), ptrTy(), fn->getArg(0));
        auto* L = weakShardSlot(hb, weakLocksGlobal(), i32(), fn->getArg(0));
        auto* lockBB = BasicBlock::Create(ctx, "weak.lock", fn);
        emitWeakEmptyCheck(hb, H, emptyDone, lockBB);
        hb.SetInsertPoint(lockBB);
        emitWeakLock(hb, L);
        auto* curS = hb.CreateAlloca(ptrTy());
        auto* prevS = hb.CreateAlloca(ptrTy());
        hb.CreateStore(hb.CreateLoad(ptrTy(), H), curS);
        hb.CreateStore(Constant::getNullValue(ptrTy()), prevS);
        hb.CreateBr(loop);

        hb.SetInsertPoint(loop);
        auto* cur = hb.CreateLoad(ptrTy(), curS);
        hb.CreateCondBr(hb.CreateICmpEQ(cur, Constant::getNullValue(ptrTy())), lockedDone, body);

        hb.SetInsertPoint(body);
        auto* target = hb.CreateLoad(ptrTy(), hb.CreateStructGEP(weakNodeTy, cur, 0));
        auto* nxt = hb.CreateLoad(ptrTy(), hb.CreateStructGEP(weakNodeTy, cur, 2));
        hb.CreateCondBr(hb.CreateICmpEQ(target, fn->getArg(0)), hit, keep);

        hb.SetInsertPoint(hit);
        auto* fa = hb.CreateLoad(ptrTy(), hb.CreateStructGEP(weakNodeTy, cur, 1));
        auto* cv = hb.CreateLoad(ptrTy(), fa);
        hb.CreateCondBr(hb.CreateICmpEQ(cv, fn->getArg(0)), nullStore, remove);

        hb.SetInsertPoint(nullStore);
        hb.CreateStore(Constant::getNullValue(ptrTy()), fa);
        hb.CreateBr(remove);

        hb.SetInsertPoint(remove);
        auto* prev = hb.CreateLoad(ptrTy(), prevS);
        hb.CreateCondBr(hb.CreateICmpEQ(prev, Constant::getNullValue(ptrTy())), rmHead, rmNode);
        hb.SetInsertPoint(rmHead);
        hb.CreateStore(nxt, H);
        hb.CreateBr(removed);
        hb.SetInsertPoint(rmNode);
        hb.CreateStore(nxt, hb.CreateStructGEP(weakNodeTy, prev, 2));
        hb.CreateBr(removed);
        hb.SetInsertPoint(removed);
        hb.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {cur});
        hb.CreateStore(nxt, curS);
        hb.CreateBr(loop);

        hb.SetInsertPoint(keep);
        hb.CreateStore(cur, prevS);
        hb.CreateStore(nxt, curS);
        hb.CreateBr(loop);

        hb.SetInsertPoint(lockedDone);
        emitWeakUnlock(hb, L);
        hb.CreateRetVoid();

        hb.SetInsertPoint(emptyDone);
        hb.CreateRetVoid();
        weakNotifyFn = fn;
        return fn;
    }

    // Write a weak field: unregister the old registration first, store the new value, and register it with the target object if it is non-null
    void Emitter::emitWeakStore(Value* addr, Value* val) {
        b.CreateCall(ensureWeakUnreg(), {addr});
        b.CreateStore(val, addr);
        BasicBlock* nn = BasicBlock::Create(ctx, "weak.nn", curFn);
        BasicBlock* done = BasicBlock::Create(ctx, "weak.done", curFn);
        b.CreateCondBr(b.CreateICmpEQ(val, Constant::getNullValue(ptrTy())), done, nn);
        b.SetInsertPoint(nn);
        b.CreateCall(ensureWeakReg(), {val, addr});
        b.CreateBr(done);
        b.SetInsertPoint(done);
    }

    bool Emitter::isRefKind(const TypePtr& t) {
        return t && (t->kind == TypeKind::Class || t->kind == TypeKind::Interface);
    }

    // the object pointer inside a reference value (a class handle or an interface value)
    Value* Emitter::refObj(Value* v, const TypePtr& ty) {
        if (ty && ty->kind == TypeKind::Interface) return b.CreateExtractValue(v, {0});
        // a delegate's RC object is its environment (the second word)
        if (ty && ty->kind == TypeKind::Delegate) return b.CreateExtractValue(v, {1});
        return v;
    }

    void Emitter::emitRetainRef(Value* v, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Interface || ty->kind == TypeKind::Delegate) {
            emitRetain(refObj(v, ty));
        } else {
            emitRetain(v);
        }
    }

    void Emitter::emitReleaseRef(Value* v, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Interface || ty->kind == TypeKind::Delegate) {
            emitRelease(refObj(v, ty));
        } else {
            emitRelease(v);
        }
    }

    // ------------------------------------------------------------ local reference cleanup
    // whether the value's lifetime needs RC cleanup (including recursive struct/array fields)
    bool Emitter::needsCleanup(const TypePtr& t) {
        return t && t->needs_cleanup();
    }

    void Emitter::releaseLocalValue(const LocalVar* lv) {
        auto it = slots.find(lv);
        if (it == slots.end()) return;

        if (isRefKind(lv->rty)) {
            Value* v = b.CreateLoad(lt(lv->rty), it->second);
            emitReleaseRef(v, lv->rty);
        } else if (lv->rty->kind == TypeKind::Struct && lv->rty->has_dtor) {
            FunctionCallee dtor = mod->getOrInsertFunction(
                dtor_cname(lv->rty->unit, lv->rty->name),
                FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
            b.CreateCall(dtor, {it->second});
        } else {
            emitReleaseValue(it->second, lv->rty);
        }
    }

    // Release one local on the way out of the scope(s) that own it. A local that can be `move`d carries a
    // runtime moved flag, so the decision is made on the path actually taken: the branch that moved it
    // skips the destruction, a sibling branch that did not still destroys the value (#21, #23). Only a
    // local that never got a flag (a capture, a synthetic owner) falls back to the compile-time answer.
    void Emitter::releaseLocal(const LocalVar* lv) {
        if (!lv || !needsCleanup(lv->rty) || lv->by_ref) return;
        AllocaInst* flag = movedFlagFor(lv);
        if (!flag) {
            if (!movedLocals.count(lv)) releaseLocalValue(lv);
            return;
        }
        BasicBlock* rel = BasicBlock::Create(ctx, "release.local", curFn);
        BasicBlock* done = BasicBlock::Create(ctx, "release.next", curFn);
        b.CreateCondBr(b.CreateLoad(i1(), flag), done, rel);
        b.SetInsertPoint(rel);
        releaseLocalValue(lv);
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(done);
        b.SetInsertPoint(done);
    }

    // Build `dst` from `src` and leave `src` an empty shell. A type with a move constructor runs it (the
    // constructor owns the transfer rules for whatever it holds); every other movable value is transferred
    // by copying the bytes and zeroing the source, which is exactly right for a handle and harmless for a
    // scalar (agents/value-semantics.md §8.2-§8.3).
    void Emitter::emitMoveInto(Value* dst, const TypePtr& ty, MoveExpr* mv) {
        LocalVar* src = mv->moved;
        Value* from = addrOf(mv->operand.get());
        if (ty->kind == TypeKind::Struct && ty->has_move_ctor) {
            FunctionCallee ctor = mod->getOrInsertFunction(
                move_ctor_cname(ty->unit, ty->name),
                FunctionType::get(LType::getVoidTy(ctx), {ptrTy(), ptrTy()}, false));
            b.CreateCall(ctor, {dst, from});
            emitCheckPending();  // if the move constructor throws, the source still owns its value
            // The constructor defines the source's final state, and the source is a live object: its
            // destructor runs at scope exit like any other. A move constructor that forgets to empty a
            // field therefore double-frees (loudly) instead of leaking silently (SPEC 3.13).
            return;
        }
        storeAt(b.CreateLoad(lt(ty), from), dst, false);
        zeroMem(from, ty);
        noteMovedLocal(src);
    }

    // Build `dst` as a deep copy of the object at `src` through the user's copy constructor.
    void Emitter::emitCopyInto(Value* dst, Value* src, const TypePtr& ty) {
        FunctionCallee ctor = mod->getOrInsertFunction(
            copy_ctor_cname(ty->unit, ty->name),
            FunctionType::get(LType::getVoidTy(ctx), {ptrTy(), ptrTy()}, false));
        b.CreateCall(ctor, {dst, src});
        emitCheckPending();
    }

    // `b = a` for a type with a user destructor: copy-and-swap. The new value is built in a temporary,
    // the temporaries are swapped byte by byte, and the temporary (which now holds the old value) is
    // destroyed. A throwing copy constructor therefore leaves the destination untouched, and a source that
    // aliases the destination is still intact when it is read (agents/value-semantics.md §8.11).
    void Emitter::emitAssignSwap(Value* dst, Expr* e, const TypePtr& ty) {
        Value* tmp = entryAlloca(lt(ty), "assign.tmp", alignOf(ty));
        if (auto mv = dynamic_cast<MoveExpr*>(e)) {
            emitMoveInto(tmp, ty, mv);
        } else {
            Value* v = coerce(e, ty);
            Value* src = entryAlloca(lt(ty), "assign.src", alignOf(ty));
            b.CreateStore(v, src);
            emitCopyInto(tmp, src, ty);
        }
        const uint64_t n = sizeOf(ty);
        if (n > 0) {
            Value* scratch = entryAlloca(llvm::ArrayType::get(i8(), n), "assign.swap");
            copyMem(scratch, dst, n);
            copyMem(dst, tmp, n);
            copyMem(tmp, scratch, n);
        }
        // The temporary now owns the destination's previous value.
        emitReleaseValue(tmp, ty);
    }

    // ------------------------------------------------------------ partially constructed objects
    size_t Emitter::findCtorProgress(const TypePtr& ty) const {
        for (size_t i = ctorProgress.size(); i > 0; --i) {
            if (!ctorProgress[i - 1].array && ctorProgress[i - 1].ty == ty) return i - 1;
        }
        return kNoCtorProgress;
    }

    // `first` is the first subobject the sequence owns: a class's inherited fields belong to the level's
    // partial destructor, which releases the (complete) base subobject.
    size_t Emitter::pushCtorProgress(Value* self, const TypePtr& ty, size_t first, bool unaligned) {
        CtorProgress p;
        p.self = self;
        p.ty = ty;
        p.first = first;
        p.next = first;
        p.unaligned = unaligned || ty->is_packed;
        p.array = ty->kind == TypeKind::Array;
        p.released = entryAlloca(i1(), "ctor.rel.done", 1);
        b.CreateStore(ConstantInt::get(i1(), 0), p.released);
        ctorProgress.push_back(p);
        return ctorProgress.size() - 1;
    }

    void Emitter::setCtorProgressNext(size_t entry, size_t next) {
        if (entry >= ctorProgress.size()) return;
        ctorProgress[entry].next = next;
    }

    void Emitter::markCtorProgressBody(size_t entry) {
        if (entry >= ctorProgress.size()) return;
        ctorProgress[entry].inBody = true;
        ctorProgress[entry].next = ctorProgressCount(ctorProgress[entry]);
    }

    Value* Emitter::ctorProgressSlot(const CtorProgress& p, size_t idx) {
        if (p.array) {
            auto* st = cast<LStruct>(lt(p.ty));
            return b.CreateGEP(st, p.self, {ConstantInt::get(i64(), 0),
                                            ConstantInt::get(i32(), 0),
                                            ConstantInt::get(i64(), idx)});
        }
        return fieldAddr(p.self, p.ty, static_cast<int>(idx));
    }

    const TypePtr& Emitter::ctorProgressSub(const CtorProgress& p, size_t idx) {
        return p.array ? p.ty->elem : p.ty->fields[idx].type;
    }

    size_t Emitter::ctorProgressCount(const CtorProgress& p) {
        return p.array ? p.ty->array_size : p.ty->fields.size();
    }

    bool Emitter::ctorProgressWeak(const CtorProgress& p, size_t idx) {
        return !p.array && p.ty->fields[idx].weak;
    }

    void Emitter::emitReleaseFieldZeroed(Value* addr, const TypePtr& ty, bool weak, bool unaligned) {
        if (!ty) return;
        if (weak) {
            b.CreateCall(ensureWeakUnreg(), {addr});
        } else {
            emitReleaseValue(addr, ty, unaligned);
        }
        zeroMem(addr, ty, unaligned);
    }

    // Release everything the interrupted construction sequences have built, innermost first. Subobjects
    // are released in reverse construction order: the ones that finished get their full destructor, the
    // one under construction only its partial destructor (its constructor never returned). Every released
    // subobject is left zeroed, so the enclosing level's partial release - which cannot know how far this
    // constructor got - destroys an empty value.
    void Emitter::emitCtorProgressCleanup() {
        for (size_t k = ctorProgress.size(); k > 0; --k) {
            CtorProgress& p = ctorProgress[k - 1];
            const size_t total = ctorProgressCount(p);
            BasicBlock* skip = BasicBlock::Create(ctx, "ctor.rel.skip", curFn);
            BasicBlock* run = BasicBlock::Create(ctx, "ctor.rel", curFn);
            b.CreateCondBr(b.CreateLoad(i1(), p.released), skip, run);
            b.SetInsertPoint(run);
            // PERF: one guard per object, so an unwind that crosses several checks (the try dispatch
            // re-checks on the no-match path) releases each object exactly once.
            b.CreateStore(ConstantInt::get(i1(), 1), p.released);
            if (!p.inBody && p.next < total) {
                Value* slot = ctorProgressSlot(p, p.next);
                const TypePtr& sub = ctorProgressSub(p, p.next);
                emitReleasePartialValue(slot, sub);
                zeroMem(slot, sub, p.unaligned);
            }
            const size_t done = p.inBody ? total : p.next;
            // PERF: unrolled, like the construction it mirrors; an array's element count is the same
            // compile-time constant, and only elements with a constructor get here at all.
            for (size_t idx = done; idx > p.first; --idx) {
                const TypePtr& sub = ctorProgressSub(p, idx - 1);
                if (!sub) continue;
                emitReleaseFieldZeroed(ctorProgressSlot(p, idx - 1), sub,
                                       ctorProgressWeak(p, idx - 1), p.unaligned);
            }
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(skip);
            b.SetInsertPoint(skip);
        }
    }

    // Run the user constructors an implicit construction has to invoke. Zeroing is the caller's job (it
    // happens whenever the type needs cleanup), so a POD value emits nothing at all and `int[1000] a;`
    // stays as cheap as it was.
    //
    // A sequence of subobject constructions opens a `CtorProgress` entry: an exception leaving it releases
    // the completed subobjects and the one being built (see emitCtorProgressCleanup). A user constructor
    // does that for its own members, so this only covers the inline sequences built here.
    void Emitter::emitCtorGlue(Value* addr, const TypePtr& ty, bool unaligned,
                               const std::function<void()>& onThrow) {
        if (!ty || !ty->needs_ctor()) return;
        if (ty->kind == TypeKind::Struct) {
            if (ty->has_default_ctor()) {
                const std::vector<TypePtr> noArgs;
                FunctionCallee ctor = mod->getOrInsertFunction(
                    ctor_cname(ty->unit, ty->name, noArgs),
                    FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                b.CreateCall(ctor, {addr});
                // PERF: unconditional check - the may-throw summary is per callee and a constructor tree
                // can mix throwing and non-throwing members.
                emitCheckPendingWith(onThrow);
                return;
            }
            const size_t mark = ctorProgress.size();
            const size_t entry = ty->needs_cleanup()
                                     ? pushCtorProgress(addr, ty, 0, unaligned)
                                     : kNoCtorProgress;
            for (size_t i = 0; i < ty->fields.size(); ++i) {
                const Field& f = ty->fields[i];
                if (f.weak || !f.type || !f.type->needs_ctor()) continue;
                setCtorProgressNext(entry, i);
                emitCtorGlue(fieldAddr(addr, ty, static_cast<int>(i)), f.type,
                             unaligned || ty->is_packed, onThrow);
            }
            ctorProgress.resize(mark);
            return;
        }
        if (ty->kind == TypeKind::Array) {
            // PERF: unrolled because the count is a compile-time constant; switch to a loop if arrays of
            // constructor-having elements ever show up with a large size.
            const size_t mark = ctorProgress.size();
            const size_t entry = ty->needs_cleanup()
                                     ? pushCtorProgress(addr, ty, 0, unaligned)
                                     : kNoCtorProgress;
            auto* st = cast<LStruct>(lt(ty));
            for (size_t i = 0; i < ty->array_size; ++i) {
                Value* ep = b.CreateGEP(st, addr, {ConstantInt::get(i64(), 0),
                                                   ConstantInt::get(i32(), 0),
                                                   ConstantInt::get(i64(), i)});
                setCtorProgressNext(entry, i);
                emitCtorGlue(ep, ty->elem, unaligned, onThrow);
            }
            ctorProgress.resize(mark);
        }
    }

    // Record that ownership left this local: `move` zeroes the slot, and the scope must not destroy it.
    // Re-assigning the variable re-arms it (`noteRevivedLocal`).
    void Emitter::noteMovedLocal(const LocalVar* lv) {
        if (!lv) return;
        if (AllocaInst* flag = movedFlagFor(lv)) {
            b.CreateStore(ConstantInt::get(i1(), 1), flag);
            return;
        }
        movedLocals.insert(lv);
        for (auto& scope : refScopes) {
            scope.erase(std::remove(scope.begin(), scope.end(), lv), scope.end());
        }
    }

    void Emitter::noteRevivedLocal(const LocalVar* lv) {
        if (!lv) return;
        if (AllocaInst* flag = movedFlagFor(lv)) {
            b.CreateStore(ConstantInt::get(i1(), 0), flag);
            return;
        }
        if (!movedLocals.count(lv)) return;
        movedLocals.erase(lv);
        if (!needsCleanup(lv->rty) || lv->by_ref || refScopes.empty()) return;
        // Register back into the scope the local was declared in, not into the innermost one: a
        // re-assignment inside a nested block must not shorten the variable's lifetime (#21). A
        // declaration scope that is no longer active would mean the variable outlived its own block, so
        // the outermost still-active scope is used instead and the cleanup is never dropped.
        size_t idx = 0;
        auto it = localDeclScope.find(lv);
        if (it != localDeclScope.end() && it->second < refScopes.size()) idx = it->second;
        refScopes[idx].push_back(lv);
    }

    AllocaInst* Emitter::movedFlagFor(const LocalVar* lv) {
        if (!curFn || !lv) return nullptr;
        auto fn = movedFlags.find(curFn);
        if (fn == movedFlags.end()) return nullptr;
        auto it = fn->second.find(lv);
        return it == fn->second.end() ? nullptr : it->second;
    }

    AllocaInst* Emitter::createMovedFlag(const LocalVar* lv) {
        if (!curFn || !lv) return nullptr;
        AllocaInst*& flag = movedFlags[curFn][lv];
        if (!flag) flag = entryAlloca(i1(), "moved");
        // Cleared at the declaration, so a local declared inside a loop is re-armed every iteration;
        // every release site of that local is dominated by its declaration.
        b.CreateStore(ConstantInt::get(i1(), 0), flag);
        return flag;
    }

    void Emitter::registerLocalCleanup(const LocalVar* lv) {
        if (!lv || !needsCleanup(lv->rty) || lv->by_ref || refScopes.empty()) return;
        createMovedFlag(lv);
        localDeclScope[lv] = refScopes.size() - 1;
        refScopes.back().push_back(lv);
    }

    // Debug aid (MUC_VERIFY_MEM=1): catch writes that run past a stack slot. The LLVM verifier accepts an
    // over-wide store (it is UB, not invalid IR), which is why that class of bug only shows up later as
    // "the optimizer deleted my code". Only an access that provably starts at an alloca is checked, so heap
    // and parameter memory never produces a false positive.
    void Emitter::verifyStackWrites() {
        const llvm::DataLayout& dl = mod->getDataLayout();
        for (llvm::Function& f : *mod) {
            if (f.isDeclaration()) continue;
            std::unordered_map<const llvm::AllocaInst*, uint64_t> size;
            for (llvm::BasicBlock& bb : f) {
                for (llvm::Instruction& i : bb) {
                    auto* ai = llvm::dyn_cast<llvm::AllocaInst>(&i);
                    if (ai && !ai->isArrayAllocation()) {
                        size[ai] = dl.getTypeAllocSize(ai->getAllocatedType());
                    }
                }
            }
            if (size.empty()) continue;

            // Walk a constant GEP / bitcast chain down to the alloca it starts from (null for anything else).
            auto baseOf = [&](const llvm::Value* p, uint64_t& off) -> const llvm::AllocaInst* {
                for (;;) {
                    if (auto* g = llvm::dyn_cast<llvm::GEPOperator>(p)) {
                        llvm::APInt c(dl.getIndexTypeSizeInBits(g->getType()), 0);
                        if (!g->accumulateConstantOffset(dl, c)) return nullptr;
                        if (!c.isNonNegative()) return nullptr;
                        off += c.getZExtValue();
                        p = g->getPointerOperand();
                        continue;
                    }
                    if (auto* bc = llvm::dyn_cast<llvm::BitCastOperator>(p)) {
                        p = bc->getOperand(0);
                        continue;
                    }
                    return llvm::dyn_cast<llvm::AllocaInst>(p);
                }
            };

            for (llvm::BasicBlock& bb : f) {
                for (llvm::Instruction& i : bb) {
                    const llvm::Value* dst = nullptr;
                    uint64_t len = 0;
                    if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&i)) {
                        dst = st->getPointerOperand();
                        len = dl.getTypeStoreSize(st->getValueOperand()->getType());
                    } else if (auto* ms = llvm::dyn_cast<llvm::MemSetInst>(&i)) {
                        dst = ms->getDest();
                        auto* c = llvm::dyn_cast<llvm::ConstantInt>(ms->getLength());
                        if (!c) continue;
                        len = c->getZExtValue();
                    } else if (auto* mc = llvm::dyn_cast<llvm::MemCpyInst>(&i)) {
                        dst = mc->getDest();
                        auto* c = llvm::dyn_cast<llvm::ConstantInt>(mc->getLength());
                        if (!c) continue;
                        len = c->getZExtValue();
                    } else {
                        continue;
                    }
                    uint64_t off = 0;
                    const llvm::AllocaInst* ai = baseOf(dst, off);
                    if (!ai) continue;
                    auto it = size.find(ai);
                    if (it == size.end()) continue;
                    if (off + len > it->second) {
                        std::string what;
                        llvm::raw_string_ostream os(what);
                        os << "stack write out of bounds in `" << f.getName().str() << "`: " << (off + len)
                           << " bytes at offset " << off << " into a " << it->second << "-byte slot";
                        throw MuError(os.str());
                    }
                }
            }
        }
    }

    void Emitter::pushRefScope() { refScopes.emplace_back(); }    void Emitter::popRefScope() {
        if (refScopes.empty()) return;
        auto scope = refScopes.back();
        if (!hasTerm(b.GetInsertBlock())) {
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
        refScopes.pop_back();
    }

    // release the references of inner scopes before leaving the current block/loop
    void Emitter::releaseScopesDeeperThan(size_t depth) {
        if (depth >= refScopes.size()) return;
        for (size_t i = refScopes.size(); i > depth; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // owned dynamic array temporaries materialized inside a statement: released at the end of the statement
    void Emitter::trackOwnedSliceTemp(Expr* e, Value* v, const TypePtr& ty) {
        if (!ty || ty->kind != TypeKind::Slice || !valueOwned(e)) return;
        auto* tmp = entryAlloca(lt(ty), "owned.tmp", alignOf(ty));
        b.CreateStore(v, tmp);
        stmtTemps.emplace_back(tmp, ty);
    }

    // owned Str rvalue: materialized into a statement temporary, with the owner released at the end of the statement
    void Emitter::releaseAllScopes() {
        for (size_t i = refScopes.size(); i > 0; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // Release the owned rvalues materialized during statement execution (those registered after mark), keeping reverse order
    void Emitter::destroyTempsFrom(size_t mark) {
        if (stmtTemps.size() <= mark) return;
        emitReleaseTempsFrom(mark);
        stmtTemps.resize(mark);
    }

    // whether the expression already holds a transferable strong reference (new / a call returning a class reference)
    bool Emitter::ownsReference(Expr* e) {
        if (!e || !isRefKind(e->ty)) return false;
        return valueOwned(e);
    }

    // whether the expression result already holds a transferable strong reference (not limited to class handles; also used for reference arrays)
    bool Emitter::valueOwned(Expr* e) {
        if (!e) return false;
        if (dynamic_cast<NewExpr*>(e)) return true;
        // `move x` hands over the value: the source is zeroed and no longer owns it
        if (dynamic_cast<MoveExpr*>(e)) return true;
        if (dynamic_cast<Call*>(e)) return true;
        // a capturing lambda materializes a fresh environment (rc = 1), so its value is owned; a
        // non-capturing lambda is a bare function address and carries no ownership either way
        if (dynamic_cast<Lambda*>(e)) return true;
        if (auto b = dynamic_cast<Binary*>(e); b && b->op_func && !b->by_ref) return true;
        if (dynamic_cast<Ternary*>(e)) return true;  // the result slot is already materialized with owned semantics
        if (auto u = dynamic_cast<Unary*>(e); u && u->op_func) return true;
        if (auto c = dynamic_cast<Cast*>(e)) return valueOwned(c->expr.get());
        return false;
    }

    // whether the expression result already holds a transferable ownership under the target type.
    // A fixed-size array -> dynamic array conversion allocates a new array in coerce, so it counts as owned too.
    bool Emitter::convOwns(Expr* e, const TypePtr& dst) {
        if (dst && dst->kind == TypeKind::Slice && e && e->ty &&
            e->ty->kind == TypeKind::Array) {
            return true;
        }
        return valueOwned(e);
    }

    // Turn a borrow into a new strong reference when returning a class reference
    Value* Emitter::prepareReturn(Expr* e) {
        // `ref T f()`: the element address (a borrow) is returned, neither copied nor retained
        if (is_borrow_type(curRetTy)) return addrOf(e);
        Value* v = coerce(e, curRetTy);
        if (isRefKind(curRetTy)) {
            if (!ownsReference(e)) emitRetainRef(v, curRetTy);
            return v;
        }
        if (curRetTy && curRetTy->kind == TypeKind::Array && needsCleanup(curRetTy)) {
            if (valueOwned(e)) return v;
            auto* tmp = entryAlloca(lt(curRetTy), "ret.arr", alignOf(curRetTy));
            b.CreateStore(v, tmp);
            emitRetainValue(tmp, curRetTy);
            return b.CreateLoad(lt(curRetTy), tmp);
        }
        if (curRetTy && curRetTy->kind == TypeKind::Struct && needsCleanup(curRetTy)) {
            // the return value transfers as owned: a borrowed value is retained field by field first, and the local is released later in releaseAllScopes
            if (valueOwned(e)) return v;
            auto* tmp = entryAlloca(lt(curRetTy), "ret.struct", alignOf(curRetTy));
            b.CreateStore(v, tmp);
            emitRetainValue(tmp, curRetTy);
            return b.CreateLoad(lt(curRetTy), tmp);
        }
        if (curRetTy && curRetTy->kind == TypeKind::Slice) {
            if (convOwns(e, curRetTy)) return v;
            auto* tmp = entryAlloca(lt(curRetTy), "ret.arr", alignOf(curRetTy));
            b.CreateStore(v, tmp);
            emitRetainValue(tmp, curRetTy);
            return b.CreateLoad(lt(curRetTy), tmp);
        }
        return v;
    }

}  // namespace muc_detail

