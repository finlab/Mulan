// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains dynamic arrays, the weak registry and the RC runtime (Emitter member function definitions).
//
// owns:      the RC runtime (retain/release), dynamic arrays and slices, the weak registry, the
//            cold-path throw helpers (bounds / null / div / shift / float-to-int) and the index checks
// not:       cleanup of locals and temporaries, copy/move helpers (codegen_cleanup.cpp); dispatch
//            tables, the layout mirror, expression or statement dispatch
// pass:      emission
// concept:   codegen-rc, codegen-runtime

#include "codegen_impl.hpp"

#include <cmath>

#include "llvm/IR/Intrinsics.h"

#include "llvm/ADT/APInt.h"

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

    // Null-reference check for one dereference of a class handle / interface object word. A null handle
    // has no fields and no vtable, so the access that follows would read address 0 + offset (SPEC 8.2);
    // the checked build throws `std.exception.NullReferenceError` with a description of the access.
    void Emitter::emitNullCheck(Value* obj, const std::string& what) {
        if (!nullCheck || !obj || !obj->getType()->isPointerTy()) return;
        BasicBlock* ok = BasicBlock::Create(ctx, "nn.ok", curFn);
        BasicBlock* bad = BasicBlock::Create(ctx, "nn.bad", curFn);
        b.CreateCondBr(b.CreateICmpEQ(obj, Constant::getNullValue(ptrTy())), bad, ok);
        b.SetInsertPoint(bad);
        FunctionCallee callee = mod->getOrInsertFunction(
            mangle_unit("std.exception", "null__fail") + "$p$K$char",
            FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
        emitColdThrow(callee, {stringConst(what)}, "nn");
        b.SetInsertPoint(ok);
    }

    // Integer division / remainder traps: `x / 0`, `x % 0` and `MIN / -1` (the last one also for `%`).
    // All three are UB (SPEC 8.2) and the hardware faults on this platform, so the checked build
    // reports them instead. A constant divisor that cannot fail emits no code at all.
    void Emitter::emitDivCheck(Value* lhs, Value* rhs, bool is_signed) {
        if (!boundsCheck || !lhs || !rhs) return;
        LType* ty = rhs->getType();
        if (!ty->isIntegerTy()) return;
        auto* c = dyn_cast<ConstantInt>(rhs);
        const bool check_zero = !c || c->isZero();
        const bool check_overflow = is_signed && (!c || c->isMinusOne());
        if (!check_zero && !check_overflow) return;
        if (check_zero) {
            BasicBlock* bad = BasicBlock::Create(ctx, "dv.zero", curFn);
            BasicBlock* ok = BasicBlock::Create(ctx, "dv.ok", curFn);
            b.CreateCondBr(b.CreateICmpEQ(rhs, ConstantInt::get(ty, 0)), bad, ok);
            b.SetInsertPoint(bad);
            emitDivFail(lhs, 0, is_signed);
            b.SetInsertPoint(ok);
        }
        if (check_overflow) {
            BasicBlock* bad = BasicBlock::Create(ctx, "dv.ovf", curFn);
            BasicBlock* ok = BasicBlock::Create(ctx, "dv.ok", curFn);
            const unsigned bits = ty->getIntegerBitWidth();
            // The sign bit alone is the minimum value: an exact constant for every width, without APInt.
            Value* is_min = b.CreateICmpEQ(lhs, ConstantInt::get(ty, uint64_t(1) << (bits - 1)));
            Value* is_neg1 = c ? b.getTrue()
                               : b.CreateICmpEQ(rhs, ConstantInt::get(ty, uint64_t(-1), /*isSigned=*/true));
            b.CreateCondBr(b.CreateAnd(is_min, is_neg1), bad, ok);
            b.SetInsertPoint(bad);
            emitDivFail(lhs, 1, is_signed);
            b.SetInsertPoint(ok);
        }
    }

    // Cold path of the division checks: call std.exception.div_fail(dividend, kind), which throws.
    void Emitter::emitDivFail(Value* dividend, int kind, bool is_signed) {
        FunctionCallee callee = mod->getOrInsertFunction(
            mangle_unit("std.exception", "div__fail") + "$long$int",
            FunctionType::get(LType::getVoidTy(ctx), {i64(), i32()}, false));
        Value* v = b.CreateIntCast(dividend, i64(), /*isSigned=*/is_signed);
        emitColdThrow(callee, {v, ConstantInt::get(i32(), kind)}, "dv");
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
    Value* Emitter::emitSliceAlloc(const std::vector<Value*>& dims, const TypePtr& elem, bool uninit,
                                bool construct_elems) {
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
        if (construct_elems && !uninit && elem->needs_ctor()) {
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


}  // namespace muc_detail

