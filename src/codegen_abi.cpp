// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// The C ABI of `extern` / `export` functions (B2): the one signature builder shared by declarations
// and call sites, plus the `byval` / `sret` / `align` attributes that make LLVM lower an aggregate
// the way C does.
//
// Without them the backend lowers a first-class aggregate by its own rule (on Win64 it splits a
// 24-byte struct into three registers, and returns a `{i32, i32}` in RAX *and* RDX, while C packs
// both halves into RAX). The two sides then disagree about where the bits are — silent memory
// corruption, not a compile error — so the declaration and every call site must be built from
// `cAbiSig`.
#include "codegen_impl.hpp"

namespace muc_detail {

    LType* Emitter::coercedType(AbiPass pass, const TypePtr& t) {
        if (pass != AbiPass::CoerceInt) return nullptr;
        return IntegerType::get(ctx, static_cast<unsigned>(sizeOf(t) * 8));
    }

    Value* Emitter::coerceToReg(Value* v, const TypePtr& t, LType* reg) {
        // The slot must satisfy the register type's alignment too: a packed aggregate only asks for
        // 1 byte, but an i64/f64 access through it claims its natural alignment.
        auto* tmp = entryAlloca(lt(t), "carg", std::max<uint64_t>(alignOf(t), sizeOf(t)));
        b.CreateStore(v, tmp);
        return b.CreateLoad(reg, tmp);
    }

    Value* Emitter::coerceFromReg(Value* v, const TypePtr& t) {
        auto* tmp = entryAlloca(lt(t), "cres", std::max<uint64_t>(alignOf(t), sizeOf(t)));
        b.CreateStore(v, tmp);
        return b.CreateLoad(lt(t), tmp);
    }

    Emitter::CAbiSig Emitter::cAbiSig(const std::vector<TypePtr>& ptys, const std::vector<bool>& refs,
                                      const TypePtr& rty) {
        CAbiSig sig;
        const AbiPass rpass = abi_class(rty, /*is_return=*/true, targetTriple);
        sig.ret = coercedType(rpass, rty);
        if (rpass == AbiPass::Sret) {
            // The result travels through a hidden first argument; the function returns void.
            sig.sret = true;
            sig.sret_ty = rty;
            sig.ret = LType::getVoidTy(ctx);
            sig.params.push_back(ptrTy());
        } else if (!sig.ret) {
            sig.ret = lt(rty);
        }
        for (size_t i = 0; i < ptys.size(); ++i) {
            const bool is_ref = i < refs.size() && refs[i];
            const AbiPass p =
                is_ref ? AbiPass::Direct : abi_class(ptys[i], /*is_return=*/false, targetTriple);
            sig.pass.push_back(p);
            LType* as = is_ref ? ptrTy() : coercedType(p, ptys[i]);
            if (p == AbiPass::ByVal) as = ptrTy();
            if (!as) as = lt(ptys[i]);
            sig.params.push_back(as);
        }
        return sig;
    }

    // Attribute set for one parameter of an `extern` / `export` symbol.
    static llvm::AttributeSet cParamAttrs(LLVMContext& ctx, AbiPass pass, LType* pointee,
                                          uint64_t alignment) {
        std::vector<llvm::Attribute> attrs;
        if (pass == AbiPass::ByVal) attrs.push_back(llvm::Attribute::getWithByValType(ctx, pointee));
        if (pass == AbiPass::Sret) {
            attrs.push_back(llvm::Attribute::getWithStructRetType(ctx, pointee));
            // the callee writes the result exactly once through this pointer, before it returns
            attrs.push_back(llvm::Attribute::get(ctx, llvm::Attribute::NoAlias));
        }
        if (alignment > 1) {
            attrs.push_back(llvm::Attribute::getWithAlignment(ctx, llvm::Align(alignment)));
        }
        return llvm::AttributeSet::get(ctx, attrs);
    }

    FunctionCallee Emitter::cAbiCallee(FuncDecl* f) {
        const CAbiSig sig = cAbiSig(f->pty, f->pty_ref, f->rty);
        FunctionCallee fc = mod->getOrInsertFunction(
            f->cname, FunctionType::get(sig.ret, sig.params, f->is_varargs));
        auto* fn = llvm::dyn_cast<Function>(fc.getCallee());
        if (!fn) return fc;  // a bitcast of an existing global: nothing to annotate
        std::vector<llvm::AttributeSet> argAttrs;
        if (sig.sret) {
            argAttrs.push_back(
                cParamAttrs(ctx, AbiPass::Sret, lt(sig.sret_ty), alignOf(sig.sret_ty)));
        }
        for (size_t i = 0; i < f->pty.size(); ++i) {
            LType* pointee = sig.pass[i] == AbiPass::ByVal ? lt(f->pty[i]) : nullptr;
            argAttrs.push_back(cParamAttrs(ctx, sig.pass[i], pointee,
                                           pointee ? alignOf(f->pty[i]) : 0));
        }
        fn->setAttributes(
            llvm::AttributeList::get(ctx, llvm::AttributeSet(), llvm::AttributeSet(), argAttrs));
        return fc;
    }

    void Emitter::applyCAbiCallAttrs(CallInst* ci, const CAbiSig& sig,
                                     const std::vector<TypePtr>& ptys,
                                     const std::vector<bool>& refs, const TypePtr& rty) {
        (void)rty;
        unsigned arg = 0;
        if (sig.sret) {
            ci->addParamAttr(arg, llvm::Attribute::getWithStructRetType(ctx, lt(sig.sret_ty)));
            ci->addParamAttr(arg, llvm::Attribute::get(ctx, llvm::Attribute::NoAlias));
            if (const uint64_t al = alignOf(sig.sret_ty); al > 1) {
                ci->addParamAttr(arg, llvm::Attribute::getWithAlignment(ctx, llvm::Align(al)));
            }
            ++arg;
        }
        for (size_t i = 0; i < ptys.size(); ++i, ++arg) {
            const bool is_ref = i < refs.size() && refs[i];
            LType* pointee = !is_ref && i < sig.pass.size() && sig.pass[i] == AbiPass::ByVal
                                 ? lt(ptys[i])
                                 : nullptr;
            if (!pointee) continue;
            ci->addParamAttr(arg, llvm::Attribute::getWithByValType(ctx, pointee));
            if (const uint64_t al = alignOf(ptys[i]); al > 1) {
                ci->addParamAttr(arg, llvm::Attribute::getWithAlignment(ctx, llvm::Align(al)));
            }
        }
    }

}  // namespace muc_detail
