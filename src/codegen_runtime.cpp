// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// 本文件包含动态数组、weak 注册表与局部引用清理（Emitter 成员函数定义）。

#include "codegen_impl.hpp"

namespace muc_detail {

    // ------------------------------------------------------------ 动态数组（T[] / T[,]）
    // rank 1：{ i64 rc; i64 len; i64 cap; data... }
    // rank r≥2：{ i64 rc; i64 d0; ... d(r-1); data... }

    uint64_t Emitter::sliceDataOffset(int rank) {
        return rank <= 1 ? 24 : 8ULL * (1 + rank);
    }

    Value* Emitter::sliceDataPtr(Value* h, int rank) {
        return b.CreateGEP(i8(), h, ConstantInt::get(i64(), sliceDataOffset(rank)));
    }

    Value* Emitter::sliceDimField(Value* h, int axis) {
        return b.CreateLoad(i64(), b.CreateGEP(i8(), h, ConstantInt::get(i64(), 8ULL * (1 + axis))));
    }

    // 元素总数（rank1 用 len，rank≥2 用各维乘积）
    Value* Emitter::sliceTotalCount(Value* h, int rank) {
        if (rank <= 1) return sliceDimField(h, 0);
        Value* total = sliceDimField(h, 0);
        for (int i = 1; i < rank; ++i) total = b.CreateMul(total, sliceDimField(h, i));
        return total;
    }

    // len(a, k)：null 句柄为 0；k 为编译期常量
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

    // len(a, k)：k 为运行期表达式（带越界检查）
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
        b.CreateCall(libFn("abort", LType::getVoidTy(ctx), {}), {});
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

    // 元素总数（null 句柄为 0）
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

    // 分配动态数组并清零；dims 为各维尺寸
    Value* Emitter::emitSliceAlloc(const std::vector<Value*>& dims, const TypePtr& elem) {
        int rank = static_cast<int>(dims.size());
        uint64_t es = sizeOf(elem);
        Value* total = ConstantInt::get(i64(), 1);
        for (Value* d : dims) total = b.CreateMul(total, d);
        if (es != 1) total = b.CreateMul(total, ConstantInt::get(i64(), es));
        Value* bytes = b.CreateAdd(total,
                                   ConstantInt::get(i64(), sliceDataOffset(rank)));
        Value* h = b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {bytes});
        BasicBlock* okBB = BasicBlock::Create(ctx, "arr.ok", curFn);
        BasicBlock* failBB = BasicBlock::Create(ctx, "arr.fail", curFn);
        b.CreateCondBr(b.CreateICmpEQ(h, Constant::getNullValue(ptrTy())), failBB, okBB);
        b.SetInsertPoint(failBB);
        b.CreateCall(libFn("abort", LType::getVoidTy(ctx), {}), {});
        b.CreateUnreachable();
        b.SetInsertPoint(okBB);
        b.CreateMemSet(h, ConstantInt::get(i8(), 0), bytes, MaybeAlign(8));
        b.CreateStore(ConstantInt::get(i64(), 1),
                      b.CreateGEP(i8(), h, ConstantInt::get(i64(), kArrRc)));
        for (int i = 0; i < rank; ++i) {
            b.CreateStore(dims[i],
                          b.CreateGEP(i8(), h, ConstantInt::get(i64(), 8ULL * (1 + i))));
        }
        if (rank == 1) {
            // 预留 cap，便于将来 append 扩容
            b.CreateStore(dims[0],
                          b.CreateGEP(i8(), h, ConstantInt::get(i64(), 16)));
        }
        return h;
    }

    Value* Emitter::emitDynArrayAlloc(Value* n, const TypePtr& elem) {
        return emitSliceAlloc({n}, elem);
    }

    void Emitter::emitSliceRetain(Value* h) { emitRetain(h); }

    // 元素释放函数：逐一 release 元素（如有引用），然后 free 头部
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
        Value* old = b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rcp,
                                       ConstantInt::get(i64(), 1), MaybeAlign(8),
                                       llvm::AtomicOrdering::Monotonic);
        b.CreateCondBr(b.CreateICmpEQ(old, ConstantInt::get(i64(), 1)), destroyBB, doneBB);
        b.SetInsertPoint(destroyBB);
        b.CreateCall(ensureSliceFree(sliceTy->elem, sliceTy->rank), {h});
        b.CreateBr(doneBB);
        b.SetInsertPoint(doneBB);
    }

    // 定长数组 -> 动态数组（按嵌套维度复制；元素含引用时逐元素 retain）
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
        Value* src = arrAddr;  // 数组包装 struct 的唯一字段在偏移 0
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

    // ------------------------------------------------------------ weak 注册表
    GlobalVariable* Emitter::weakHeadGlobal() { return mod->getNamedGlobal("mu_weak_head"); }
    GlobalVariable* Emitter::weakLockGlobal() { return mod->getNamedGlobal("mu_weak_lock"); }

    void Emitter::emitWeakLock(IRBuilder<>& hb) {
        auto* fn = hb.GetInsertBlock()->getParent();
        auto* spin = BasicBlock::Create(ctx, "weak.spin", fn);
        auto* got = BasicBlock::Create(ctx, "weak.got", fn);
        hb.CreateBr(spin);
        hb.SetInsertPoint(spin);
        auto* old = hb.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, weakLockGlobal(),
                                       ConstantInt::get(i32(), 1), MaybeAlign(4),
                                       llvm::AtomicOrdering::Monotonic);
        hb.CreateCondBr(hb.CreateICmpEQ(old, ConstantInt::get(i32(), 0)), got, spin);
        hb.SetInsertPoint(got);
    }

    void Emitter::emitWeakUnlock(IRBuilder<>& hb) {
        hb.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, weakLockGlobal(),
                           ConstantInt::get(i32(), 0), MaybeAlign(4),
                           llvm::AtomicOrdering::Monotonic);
    }

    Function* Emitter::ensureWeakReg() {
        if (weakRegFn) return weakRegFn;
        auto* ft = FunctionType::get(LType::getVoidTy(ctx), {ptrTy(), ptrTy()}, false);
        auto* fn = cast<Function>(mod->getOrInsertFunction("mu_rt_weak_reg", ft).getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        IRBuilder<> hb(ctx);
        auto* entry = BasicBlock::Create(ctx, "entry", fn);
        hb.SetInsertPoint(entry);
        emitWeakLock(hb);
        auto* node = hb.CreateCall(libFn("malloc", ptrTy(), {i64()}),
                                   {ConstantInt::get(i64(), 24)});
        hb.CreateStore(fn->getArg(0), hb.CreateStructGEP(weakNodeTy, node, 0));  // target
        hb.CreateStore(fn->getArg(1), hb.CreateStructGEP(weakNodeTy, node, 1));  // field
        auto* H = weakHeadGlobal();
        auto* head = hb.CreateLoad(ptrTy(), H);
        hb.CreateStore(head, hb.CreateStructGEP(weakNodeTy, node, 2));  // next
        hb.CreateStore(node, H);
        emitWeakUnlock(hb);
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
        auto* H = weakHeadGlobal();
        auto* entry = BasicBlock::Create(ctx, "entry", fn);
        auto* loop = BasicBlock::Create(ctx, "loop", fn);
        auto* body = BasicBlock::Create(ctx, "body", fn);
        auto* remove = BasicBlock::Create(ctx, "remove", fn);
        auto* rmHead = BasicBlock::Create(ctx, "rm.head", fn);
        auto* rmNode = BasicBlock::Create(ctx, "rm.node", fn);
        auto* removed = BasicBlock::Create(ctx, "removed", fn);
        auto* keep = BasicBlock::Create(ctx, "keep", fn);
        auto* done = BasicBlock::Create(ctx, "done", fn);

        hb.SetInsertPoint(entry);
        emitWeakLock(hb);
        auto* curS = hb.CreateAlloca(ptrTy());
        auto* prevS = hb.CreateAlloca(ptrTy());
        hb.CreateStore(hb.CreateLoad(ptrTy(), H), curS);
        hb.CreateStore(Constant::getNullValue(ptrTy()), prevS);
        hb.CreateBr(loop);

        hb.SetInsertPoint(loop);
        auto* cur = hb.CreateLoad(ptrTy(), curS);
        hb.CreateCondBr(hb.CreateICmpEQ(cur, Constant::getNullValue(ptrTy())), done, body);

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

        hb.SetInsertPoint(done);
        emitWeakUnlock(hb);
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
        auto* H = weakHeadGlobal();
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
        auto* done = BasicBlock::Create(ctx, "done", fn);

        hb.SetInsertPoint(entry);
        emitWeakLock(hb);
        auto* curS = hb.CreateAlloca(ptrTy());
        auto* prevS = hb.CreateAlloca(ptrTy());
        hb.CreateStore(hb.CreateLoad(ptrTy(), H), curS);
        hb.CreateStore(Constant::getNullValue(ptrTy()), prevS);
        hb.CreateBr(loop);

        hb.SetInsertPoint(loop);
        auto* cur = hb.CreateLoad(ptrTy(), curS);
        hb.CreateCondBr(hb.CreateICmpEQ(cur, Constant::getNullValue(ptrTy())), done, body);

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

        hb.SetInsertPoint(done);
        emitWeakUnlock(hb);
        hb.CreateRetVoid();
        weakNotifyFn = fn;
        return fn;
    }

    // 写 weak 字段：先注销旧登记，存储新值，非空则登记到目标对象
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

    // 引用值（class 句柄或 interface 值）里的对象指针
    Value* Emitter::refObj(Value* v, const TypePtr& ty) {
        if (ty && ty->kind == TypeKind::Interface) return b.CreateExtractValue(v, {0});
        return v;
    }

    void Emitter::emitRetainRef(Value* v, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Interface) {
            emitRetain(b.CreateExtractValue(v, {0}));
        } else {
            emitRetain(v);
        }
    }

    void Emitter::emitReleaseRef(Value* v, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Interface) {
            emitRelease(b.CreateExtractValue(v, {0}));
        } else {
            emitRelease(v);
        }
    }

    // ------------------------------------------------------------ 局部引用清理
    // 值的生命周期里是否需要 RC 清理（含递归的 struct/array 字段）
    bool Emitter::needsCleanup(const TypePtr& t) {
        return t && t->needs_cleanup();
    }

    void Emitter::releaseLocal(const LocalVar* lv) {
        if (!lv || !needsCleanup(lv->rty) || lv->by_ref) return;
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

    void Emitter::pushRefScope() { refScopes.emplace_back(); }

    void Emitter::popRefScope() {
        if (refScopes.empty()) return;
        auto scope = refScopes.back();
        if (!hasTerm(b.GetInsertBlock())) {
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
        refScopes.pop_back();
    }

    // 跳出当前块/循环前释放内层作用域的引用
    void Emitter::releaseScopesDeeperThan(size_t depth) {
        if (depth >= refScopes.size()) return;
        for (size_t i = refScopes.size(); i > depth; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // 语句内物化的 owned 动态数组临时值：语句结束时释放
    void Emitter::trackOwnedSliceTemp(Expr* e, Value* v, const TypePtr& ty) {
        if (!ty || ty->kind != TypeKind::Slice || !valueOwned(e)) return;
        auto* tmp = entryAlloca(lt(ty), "owned.tmp", alignOf(ty));
        b.CreateStore(v, tmp);
        stmtTemps.emplace_back(tmp, ty);
    }

    // owned 的 Str 右值：物化到语句临时并在语句结束释放 owner
    Value* Emitter::evalStrValue(Expr* e) {
        Value* v = eval(e);
        if (valueOwned(e)) {
            auto* tmp = entryAlloca(lt(strType()), "str.tmp", alignOf(strType()));
            b.CreateStore(v, tmp);
            stmtTemps.emplace_back(tmp, strType());
        }
        return v;
    }

    void Emitter::releaseAllScopes() {
        for (size_t i = refScopes.size(); i > 0; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // 释放语句执行期间物化的 owned 右值（从 mark 之后注册的），保持逆序
    void Emitter::destroyTempsFrom(size_t mark) {
        if (stmtTemps.size() <= mark) return;
        for (size_t i = stmtTemps.size(); i > mark; --i) {
            emitReleaseValue(stmtTemps[i - 1].first, stmtTemps[i - 1].second);
        }
        stmtTemps.resize(mark);
    }

    // 表达式是否已经持有一份可转移的强引用（new / 返回 class 引用的调用）
    bool Emitter::ownsReference(Expr* e) {
        if (!e || !isRefKind(e->ty)) return false;
        return valueOwned(e);
    }

    // 表达式结果是否已持有一份可转移的强引用（不限于 class 句柄，也用于引用数组）
    bool Emitter::valueOwned(Expr* e) {
        if (!e) return false;
        if (dynamic_cast<NewExpr*>(e)) return true;
        if (dynamic_cast<Call*>(e)) return true;
        if (auto b = dynamic_cast<Binary*>(e); b && b->op_func) return true;
        if (dynamic_cast<Ternary*>(e)) return true;  // 结果槽已按 owned 语义物化
        if (auto u = dynamic_cast<Unary*>(e); u && u->op_func) return true;
        if (auto ix = dynamic_cast<Index*>(e); ix && ix->get_func) return true;
        if (auto c = dynamic_cast<Cast*>(e)) return valueOwned(c->expr.get());
        return false;
    }

    // 表达式结果在目标类型下是否已持有一份可转移的所有权。
    // 定长数组 -> 动态数组的转换会在 coerce 里新建数组，因此也算 owned。
    bool Emitter::convOwns(Expr* e, const TypePtr& dst) {
        if (dst && dst->kind == TypeKind::Slice && e && e->ty &&
            e->ty->kind == TypeKind::Array) {
            return true;
        }
        return valueOwned(e);
    }

    // 返回 class 引用时把借用变成一份新的强引用
    Value* Emitter::prepareReturn(Expr* e) {
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
            // 返回值按 owned 转移：借用值先逐字段 retain，局部随后在 releaseAllScopes 释放
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
