// LLVM 后端：用 LLVM C++ API 直接构造 IR，产出 COFF 目标文件（由 gcc 链接）。
//
// 策略：所有局部变量用 alloca 承载（配合 -O 时可被 mem2reg 优化）。
#include "codegen_llvm.hpp"

#include "common.hpp"
#include "mangle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <typeinfo>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

using llvm::AllocaInst;
using llvm::BasicBlock;
using llvm::CodeGenOptLevel;
using llvm::Constant;
using llvm::ConstantAggregateZero;
using llvm::ConstantArray;
using llvm::ConstantDataArray;
using llvm::ConstantFP;
using llvm::ConstantInt;
using llvm::ConstantStruct;
using llvm::Function;
using llvm::FunctionCallee;
using llvm::FunctionType;
using llvm::GlobalValue;
using llvm::GlobalVariable;
using llvm::IRBuilder;
using llvm::IntegerType;
using llvm::LLVMContext;
using llvm::MaybeAlign;
using llvm::Module;
using llvm::ModuleAnalysisManager;
using llvm::ModulePassManager;
using llvm::PassBuilder;
using llvm::CGSCCAnalysisManager;
using llvm::FunctionAnalysisManager;
using llvm::LoopAnalysisManager;
using llvm::PHINode;
using llvm::PointerType;
using llvm::Target;
using llvm::TargetMachine;
using llvm::TargetOptions;
using llvm::TargetRegistry;
using llvm::UndefValue;
using llvm::Value;
using llvm::raw_fd_ostream;
using llvm::raw_string_ostream;
using LType = llvm::Type;
using LStruct = llvm::StructType;
using LArray = llvm::ArrayType;

#ifndef MULANC_VERSION
#define MULANC_VERSION "0.0.0"
#endif

namespace {

std::string sane(std::string s) {
    for (char& c : s) {
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_')) c = '_';
    }
    return s;
}

bool is_zero_literal(Init* init) {
    if (auto ie = dynamic_cast<InitExpr*>(init)) {
        if (auto il = dynamic_cast<IntLit*>(ie->expr.get())) return il->value == 0;
    }
    return false;
}

bool is_integer(const TypePtr& t) {
    if (!t) return false;
    if (t->kind == TypeKind::Enum) return true;
    return t->kind == TypeKind::Prim && t->is_integer();
}

bool is_numeric(const TypePtr& t) {
    if (!t) return false;
    if (t->kind == TypeKind::Enum) return true;
    return t->kind == TypeKind::Prim && t->is_numeric();
}

bool is_void(const TypePtr& t) { return t && t->kind == TypeKind::Void; }

struct Emitter {
    explicit Emitter(Program& p) : prog(p) {}

    Program& prog;
    LLVMContext ctx;
    std::unique_ptr<Module> mod;
    std::unique_ptr<TargetMachine> tm;
    IRBuilder<> b{ctx};
    IRBuilder<> ab{ctx};

    TypePtr strMeta;  // std.string.string 的语义类型（惰性查找）
    LType* sliceTy = nullptr;  // 动态数组句柄
    std::map<std::string, Function*> sliceFreeCache;  // 元素类型 -> 释放函数
    LStruct* descTy = nullptr;
    LStruct* ifaceTy = nullptr;  // {ptr obj, ptr desc}
    std::map<std::string, LStruct*> named;
    std::map<std::string, std::vector<unsigned>> fieldMap;  // 结构体：模型字段索引 -> LLVM 索引
    std::map<std::string, Constant*> strCache;

    BasicBlock* allocaBB = nullptr;
    BasicBlock* bodyBB = nullptr;
    Function* curFn = nullptr;
    std::string curUnit;
    std::map<const LocalVar*, AllocaInst*> slots;
    std::vector<std::pair<BasicBlock*, BasicBlock*>> loops;  // (continue, break)
    TypePtr curClassTy;   // 当前方法/构造器/析构器所属 class
    Value* curThis = nullptr;  // 当前 this 指针（方法入口的第一个参数）
    AllocaInst* thisSlot = nullptr;
    Function* rcRetainFn = nullptr;
    Function* rcReleaseFn = nullptr;
    Function* weakRegFn = nullptr;
    Function* weakUnregFn = nullptr;
    Function* weakNotifyFn = nullptr;
    LStruct* weakNodeTy = nullptr;
    std::vector<std::vector<const LocalVar*>> refScopes;  // 需要释放的引用局部变量
    std::vector<size_t> loopScopeDepth;
    // 语句内被取地址物化的 owned struct/array 临时值，语句结束时统一释放
    std::vector<std::pair<Value*, TypePtr>> stmtTemps;

    // ------------------------------------------------------------ 基础工具
    LType* ptrTy() { return PointerType::get(ctx, 0); }
    LType* i1() { return LType::getInt1Ty(ctx); }
    LType* i8() { return LType::getInt8Ty(ctx); }
    LType* i32() { return LType::getInt32Ty(ctx); }
    LType* i64() { return LType::getInt64Ty(ctx); }

    [[noreturn]] void fail(const std::string& m, int line = 0, int col = 0) {
        throw MuError(m, line, col, prog.units.empty() ? "" : prog.units.front()->filename);
    }

    LType* primTy(const std::string& n) {
        if (n == "bool") return i1();
        if (n == "byte" || n == "char") return i8();
        if (n == "short" || n == "ushort") return LType::getInt16Ty(ctx);
        if (n == "int" || n == "uint") return i32();
        if (n == "long" || n == "ulong" || n == "usize" || n == "isize") return i64();
        if (n == "float") return LType::getFloatTy(ctx);
        if (n == "double") return LType::getDoubleTy(ctx);
        fail("LLVM 后端：未知基本类型 " + n);
    }

    LStruct* structOf(const TypePtr& t) {
        std::string key = "s:" + t->unit + ":" + t->name;
        auto it = named.find(key);
        if (it != named.end()) return it->second;
        auto* st = LStruct::create(ctx, "mu_" + sane(t->unit) + "_" + sane(t->name));
        named[key] = st;
        const auto& dl = mod->getDataLayout();
        std::vector<LType*> fs;
        std::vector<unsigned> map;  // 模型字段索引 -> LLVM 字段索引
        uint64_t llvmCur = 0;       // LLVM 视角的当前偏移
        uint64_t modelCur = 0;      // 语义层（types.cpp）的当前偏移
        for (auto& f : t->fields) {
            LType* ft = lt(f.type);
            if (!ft->isSized()) {
                fail("LLVM 后端：结构体 `" + t->str() + "` 的字段 `" + f.name + "` 不是定长类型");
            }
            uint64_t fsz = dl.getTypeAllocSize(ft);
            uint64_t fa = alignOf(f.type);
            uint64_t la = dl.getABITypeAlign(ft).value();
            uint64_t modelOff = t->is_packed ? modelCur : (modelCur + fa - 1) / fa * fa;
            uint64_t llvmOff = t->is_packed ? llvmCur : (llvmCur + la - 1) / la * la;
            if (llvmOff < modelOff) {
                // 显式填充：让 LLVM 偏移与语义层一致（align(N) 字段在嵌套结构里靠它对齐）
                fs.push_back(LArray::get(i8(), modelOff - llvmOff));
                llvmCur = modelOff;
                llvmOff = modelOff;
            } else if (llvmOff > modelOff) {
                fail("LLVM 后端：结构体 `" + t->str() + "` 的字段 `" + f.name +
                     "` 布局与语义层不一致");
            }
            map.push_back(static_cast<unsigned>(fs.size()));
            fs.push_back(ft);
            llvmCur = llvmOff + fsz;
            modelCur = modelOff + sizeOf(f.type);
        }
        // 尾部填充：LLVM 结构体大小必须等于语义层 sizeof（数组元素跨度依赖该不变量）
        uint64_t structAlign = 1;
        if (!t->is_packed) {
            for (auto* f : fs) {
                structAlign = std::max<uint64_t>(structAlign, dl.getABITypeAlign(f).value());
            }
        }
        uint64_t llvmEnd = (llvmCur + structAlign - 1) / structAlign * structAlign;
        uint64_t modelSize = sizeOf(t);
        if (llvmEnd < modelSize) {
            fs.push_back(LArray::get(i8(), modelSize - llvmEnd));
        } else if (llvmEnd > modelSize) {
            fail("LLVM 后端：结构体 `" + t->str() + "` 的大小与语义层不一致（" +
                 std::to_string(llvmEnd) + " / " + std::to_string(modelSize) + "）");
        }
        st->setBody(fs, t->is_packed);
        fieldMap[key] = std::move(map);
        return st;
    }

    // 模型字段索引 -> LLVM 字段索引（align(N) 会在字段间插入显式 [k x i8] 填充）
    unsigned llvmFieldIndex(const TypePtr& t, int model_idx) {
        const char* prefix = t->kind == TypeKind::Class ? "o:" : "s:";
        auto it = fieldMap.find(prefix + t->unit + ":" + t->name);
        if (it == fieldMap.end() || model_idx < 0 ||
            static_cast<size_t>(model_idx) >= it->second.size()) {
            return static_cast<unsigned>(model_idx);
        }
        return it->second[static_cast<size_t>(model_idx)];
    }

    LStruct* arrayOf(const TypePtr& t) {
        std::string key = "a:" + t->str();
        auto it = named.find(key);
        if (it != named.end()) return it->second;
        auto* st = LStruct::create(ctx, "mu_arr_" + sane(t->str()));
        named[key] = st;
        st->setBody({LArray::get(lt(t->elem), t->array_size)}, false);
        return st;
    }

    static uint64_t alignUp(uint64_t v, uint64_t a) {
        return a <= 1 ? v : (v + a - 1) / a * a;
    }

    // class 对象布局（模型）：{ i64 rc, ptr desc } 固定 16 字节，字段从 16 开始按 type_align 排列。
    static constexpr uint64_t kObjHeader = 16;

    uint64_t classObjAlign(const TypePtr& cls) {
        uint64_t a = kObjHeader;  // 堆分配至少保证 16 字节
        for (auto& f : cls->fields) a = std::max(a, alignOf(f.type));
        return a;
    }

    uint64_t classFieldOffset(const TypePtr& cls, int idx) {
        uint64_t off = kObjHeader;
        for (int i = 0; i < idx; ++i) {
            off = alignUp(off, alignOf(cls->fields[i].type));
            off += sizeOf(cls->fields[i].type);
        }
        return alignUp(off, alignOf(cls->fields[idx].type));
    }

    uint64_t classObjSize(const TypePtr& cls) {
        uint64_t off = kObjHeader;
        for (auto& f : cls->fields) {
            off = alignUp(off, alignOf(f.type));
            off += sizeOf(f.type);
        }
        return alignUp(off, classObjAlign(cls));
    }

    // 对象布局：{ i64 rc, ptr desc, [填充], fields... }（fields 已在 sema 扁平化，基类字段在前）
    // 与 structOf 同样插入显式 [k x i8] 填充，保证 LLVM 偏移与语义层 type_align/type_size 一致。
    LStruct* objStruct(const TypePtr& cls) {
        std::string key = "o:" + cls->unit + ":" + cls->name;
        auto it = named.find(key);
        if (it != named.end()) return it->second;
        auto* st = LStruct::create(ctx, "mu_obj_" + sane(cls->unit) + "_" + sane(cls->name));
        named[key] = st;
        const auto& dl = mod->getDataLayout();
        std::vector<LType*> fs{i64(), ptrTy()};
        std::vector<unsigned> map;  // 模型字段索引 -> LLVM 字段索引
        uint64_t llvmCur = kObjHeader;
        uint64_t modelCur = kObjHeader;
        for (auto& f : cls->fields) {
            LType* ft = lt(f.type);
            if (!ft->isSized()) {
                fail("LLVM 后端：class `" + cls->str() + "` 的字段 `" + f.name + "` 不是定长类型");
            }
            uint64_t fsz = dl.getTypeAllocSize(ft);
            uint64_t fa = alignOf(f.type);
            uint64_t la = dl.getABITypeAlign(ft).value();
            uint64_t modelOff = alignUp(modelCur, fa);
            uint64_t llvmOff = alignUp(llvmCur, la);
            if (llvmOff < modelOff) {
                // 显式填充：让字段偏移满足 align(N)（LLVM 自身只按类型的自然对齐排布）
                fs.push_back(LArray::get(i8(), modelOff - llvmOff));
                llvmCur = modelOff;
                llvmOff = modelOff;
            } else if (llvmOff > modelOff) {
                fail("LLVM 后端：class `" + cls->str() + "` 的字段 `" + f.name +
                     "` 布局与语义层不一致");
            }
            map.push_back(static_cast<unsigned>(fs.size()));
            fs.push_back(ft);
            llvmCur = llvmOff + fsz;
            modelCur = modelOff + sizeOf(f.type);
        }
        // 尾部补齐到对象对齐（不要依赖 LLVM 结构的自然对齐：字段全是 8 字节类型时它只有 8）
        uint64_t modelSize = alignUp(modelCur, classObjAlign(cls));
        if (llvmCur < modelSize) {
            fs.push_back(LArray::get(i8(), modelSize - llvmCur));
        } else if (llvmCur > modelSize) {
            fail("LLVM 后端：class `" + cls->str() + "` 的大小与语义层不一致（" +
                 std::to_string(llvmCur) + " / " + std::to_string(modelSize) + "）");
        }
        st->setBody(fs, false);
        if (dl.getTypeAllocSize(st) != modelSize) {
            fail("LLVM 后端：class `" + cls->str() + "` 的 LLVM 大小与语义层不一致（" +
                 std::to_string(dl.getTypeAllocSize(st)) + " / " + std::to_string(modelSize) +
                 "）");
        }
        fieldMap[key] = std::move(map);
        return st;
    }

    static constexpr unsigned kObjRc = 0;
    static constexpr unsigned kObjDesc = 1;

    uint64_t objectSize(const TypePtr& cls) { return classObjSize(cls); }

    uint64_t objectAlign(const TypePtr& cls) { return classObjAlign(cls); }

    // align(N) > 16 的 class 对象要显式对齐分配（malloc 只保证 16 字节）；释放时必须配对
    bool classNeedsAlignedAlloc(const TypePtr& cls) { return classObjAlign(cls) > 16; }

    Value* fieldAddr(Value* objPtr, const TypePtr& cls, int field_index) {
        if (cls->kind == TypeKind::Struct) {
            // 先取 LLVM 类型（可能触发 structOf 填充字段映射表），再查映射
            LStruct* st = cast<LStruct>(lt(cls));
            unsigned idx = llvmFieldIndex(cls, field_index);
            return b.CreateStructGEP(st, objPtr, idx);
        }
        // 先建对象类型（填充 fieldMap），再按映射索引（align(N) 字段前有显式填充）
        LStruct* st = objStruct(cls);
        unsigned idx = llvmFieldIndex(cls, field_index);
        return b.CreateStructGEP(st, objPtr, idx);
    }

    Value* descOf(const TypePtr& cls) {
        return mod->getOrInsertGlobal(class_desc_name(cls->unit, cls->name), descTy);
    }

    // std.string.string：编译器识别的规范字符串结构体（字段 p/n/owner）
    TypePtr strType() {
        if (strMeta) return strMeta;
        for (auto& u : prog.units) {
            if (u->name != "std.string") continue;
            for (auto& d : u->decls) {
                auto sd = dynamic_cast<StructDecl*>(d.get());
                if (sd && sd->name == "string" && sd->ty) {
                    strMeta = sd->ty;
                    return strMeta;
                }
            }
        }
        fail("std.string.string 未定义");
    }

    bool isStrType(const TypePtr& t) {
        TypePtr s = strType();
        return t && s && same_type(t, s);
    }

    LType* lt(const TypePtr& t) {
        if (!t) return i32();
        switch (t->kind) {
            case TypeKind::Void: return LType::getVoidTy(ctx);
            case TypeKind::Prim: return primTy(t->name);
            case TypeKind::Ptr: return ptrTy();
            case TypeKind::Slice: return sliceTy;
            case TypeKind::Array: return arrayOf(t);
            case TypeKind::Struct: return structOf(t);
            case TypeKind::Interface: return ifaceTy ? ifaceTy : ptrTy();
            case TypeKind::Enum: return i32();
            default: return ptrTy();
        }
    }

    // ------------------------------------------------------------ 布局
    // 唯一实现在 types.cpp（type_align/type_size），这里只做转接，避免两份布局漂移。
    uint64_t alignOf(const TypePtr& t) { return type_align(t); }
    uint64_t sizeOf(const TypePtr& t) { return type_size(t); }

    bool zeroable(const TypePtr& t) {
        return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Array ||
                     t->kind == TypeKind::Slice);
    }

    // ------------------------------------------------------------ 函数/全局
    FunctionCallee libFn(const char* name, LType* ret, std::vector<LType*> params) {
        return mod->getOrInsertFunction(name, FunctionType::get(ret, params, false));
    }

    Constant* zeroConst(LType* ty, const TypePtr& mt) {
        if (mt && mt->kind == TypeKind::Slice) return Constant::getNullValue(ty);
        if (mt && zeroable(mt)) return ConstantAggregateZero::get(ty);
        return Constant::getNullValue(ty);
    }

    Value* stringConst(const std::string& bytes) {
        auto it = strCache.find(bytes);
        if (it != strCache.end()) return it->second;
        Constant* arr = ConstantDataArray::getString(ctx, bytes, true);
        auto* gv = new GlobalVariable(*mod, arr->getType(), true, GlobalValue::PrivateLinkage,
                                      arr, ".str");
        TypePtr st = strType();
        Constant* c = ConstantStruct::get(
            cast<LStruct>(lt(st)),
            {gv, ConstantInt::get(i64(), bytes.size()), Constant::getNullValue(ptrTy())});
        strCache[bytes] = c;
        return c;
    }

    // ------------------------------------------------------------ 引用计数运行时
    Function* ensureRetain() {
        if (rcRetainFn) return rcRetainFn;
        auto* ft = FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false);
        auto* fn = cast<Function>(mod->getOrInsertFunction("mu_rt_retain", ft).getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        IRBuilder<> hb(ctx);
        auto* entry = BasicBlock::Create(ctx, "entry", fn);
        auto* inc = BasicBlock::Create(ctx, "inc", fn);
        auto* done = BasicBlock::Create(ctx, "done", fn);
        hb.SetInsertPoint(entry);
        auto* p = fn->getArg(0);
        hb.CreateCondBr(hb.CreateICmpEQ(p, Constant::getNullValue(ptrTy())), done, inc);
        hb.SetInsertPoint(inc);
        auto* rc = hb.CreateGEP(i8(), p, ConstantInt::get(i64(), 0));
        hb.CreateAtomicRMW(llvm::AtomicRMWInst::Add, rc, ConstantInt::get(i64(), 1),
                           MaybeAlign(8), llvm::AtomicOrdering::Monotonic);
        hb.CreateBr(done);
        hb.SetInsertPoint(done);
        hb.CreateRetVoid();
        rcRetainFn = fn;
        return fn;
    }

    Function* ensureRelease() {
        if (rcReleaseFn) return rcReleaseFn;
        auto* ft = FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false);
        auto* fn = cast<Function>(mod->getOrInsertFunction("mu_rt_release", ft).getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        IRBuilder<> hb(ctx);
        auto* entry = BasicBlock::Create(ctx, "entry", fn);
        auto* body = BasicBlock::Create(ctx, "body", fn);
        auto* destroy = BasicBlock::Create(ctx, "destroy", fn);
        auto* done = BasicBlock::Create(ctx, "done", fn);
        hb.SetInsertPoint(entry);
        auto* p = fn->getArg(0);
        hb.CreateCondBr(hb.CreateICmpEQ(p, Constant::getNullValue(ptrTy())), done, body);
        hb.SetInsertPoint(body);
        auto* rc = hb.CreateGEP(i8(), p, ConstantInt::get(i64(), 0));
        auto* old = hb.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rc, ConstantInt::get(i64(), 1),
                                       MaybeAlign(8), llvm::AtomicOrdering::Monotonic);
        hb.CreateCondBr(hb.CreateICmpEQ(old, ConstantInt::get(i64(), 1)), destroy, done);
        hb.SetInsertPoint(destroy);
        auto* descp = hb.CreateGEP(i8(), p, ConstantInt::get(i64(), 8));
        auto* desc = hb.CreateLoad(ptrTy(), descp);
        auto* dfnp = hb.CreateGEP(i8(), desc, ConstantInt::get(i64(), 8));
        auto* dfn = hb.CreateLoad(ptrTy(), dfnp);
        // destroy 函数负责调用析构链并 free，这里不再 free（否则双重释放）
        hb.CreateCall(FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false), dfn, {p});
        hb.CreateBr(done);
        hb.SetInsertPoint(done);
        hb.CreateRetVoid();
        rcReleaseFn = fn;
        return fn;
    }

    void emitRetain(Value* p) {
        if (!p) return;
        b.CreateCall(ensureRetain(), {p});
    }

    void emitRelease(Value* p) {
        if (!p) return;
        b.CreateCall(ensureRelease(), {p});
    }

    // ------------------------------------------------------------ 动态数组（T[] / T[,]）
    // rank 1：{ i64 rc; i64 len; i64 cap; data... }
    // rank r≥2：{ i64 rc; i64 d0; ... d(r-1); data... }
    static constexpr uint64_t kArrRc = 0;

    static uint64_t sliceDataOffset(int rank) {
        return rank <= 1 ? 24 : 8ULL * (1 + rank);
    }

    Value* sliceDataPtr(Value* h, int rank) {
        return b.CreateGEP(i8(), h, ConstantInt::get(i64(), sliceDataOffset(rank)));
    }

    Value* sliceDimField(Value* h, int axis) {
        return b.CreateLoad(i64(), b.CreateGEP(i8(), h, ConstantInt::get(i64(), 8ULL * (1 + axis))));
    }

    // 元素总数（rank1 用 len，rank≥2 用各维乘积）
    Value* sliceTotalCount(Value* h, int rank) {
        if (rank <= 1) return sliceDimField(h, 0);
        Value* total = sliceDimField(h, 0);
        for (int i = 1; i < rank; ++i) total = b.CreateMul(total, sliceDimField(h, i));
        return total;
    }

    // len(a, k)：null 句柄为 0；k 为编译期常量
    Value* emitSliceDim(Value* h, int axis) {
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
    Value* emitSliceDimDynamic(Value* h, Value* axis, int rank) {
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
    Value* emitSliceTotal(Value* h, int rank) {
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
    Value* emitSliceAlloc(const std::vector<Value*>& dims, const TypePtr& elem) {
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

    Value* emitDynArrayAlloc(Value* n, const TypePtr& elem) {
        return emitSliceAlloc({n}, elem);
    }

    void emitSliceRetain(Value* h) { emitRetain(h); }

    // 元素释放函数：逐一 release 元素（如有引用），然后 free 头部
    Function* ensureSliceFree(const TypePtr& elem, int rank) {
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

    void emitSliceRelease(Value* h, const TypePtr& sliceTy) {
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
    Value* emitDynArrayFromArray(Value* arrAddr, const TypePtr& arrTy,
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
    GlobalVariable* weakHeadGlobal() { return mod->getNamedGlobal("mu_weak_head"); }
    GlobalVariable* weakLockGlobal() { return mod->getNamedGlobal("mu_weak_lock"); }

    void emitWeakLock(IRBuilder<>& hb) {
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

    void emitWeakUnlock(IRBuilder<>& hb) {
        hb.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, weakLockGlobal(),
                           ConstantInt::get(i32(), 0), MaybeAlign(4),
                           llvm::AtomicOrdering::Monotonic);
    }

    Function* ensureWeakReg() {
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

    Function* ensureWeakUnreg() {
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

    Function* ensureWeakNotify() {
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
    void emitWeakStore(Value* addr, Value* val) {
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

    bool isRefKind(const TypePtr& t) {
        return t && (t->kind == TypeKind::Class || t->kind == TypeKind::Interface);
    }

    // 引用值（class 句柄或 interface 值）里的对象指针
    Value* refObj(Value* v, const TypePtr& ty) {
        if (ty && ty->kind == TypeKind::Interface) return b.CreateExtractValue(v, {0});
        return v;
    }

    void emitRetainRef(Value* v, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Interface) {
            emitRetain(b.CreateExtractValue(v, {0}));
        } else {
            emitRetain(v);
        }
    }

    void emitReleaseRef(Value* v, const TypePtr& ty) {
        if (!ty) return;
        if (ty->kind == TypeKind::Interface) {
            emitRelease(b.CreateExtractValue(v, {0}));
        } else {
            emitRelease(v);
        }
    }

    // ------------------------------------------------------------ 局部引用清理
    // 值的生命周期里是否需要 RC 清理（含递归的 struct/array 字段）
    static bool needsCleanup(const TypePtr& t) {
        return t && t->needs_cleanup();
    }

    void releaseLocal(const LocalVar* lv) {
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

    void pushRefScope() { refScopes.emplace_back(); }

    void popRefScope() {
        if (refScopes.empty()) return;
        auto scope = refScopes.back();
        if (!hasTerm(b.GetInsertBlock())) {
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
        refScopes.pop_back();
    }

    // 跳出当前块/循环前释放内层作用域的引用
    void releaseScopesDeeperThan(size_t depth) {
        if (depth >= refScopes.size()) return;
        for (size_t i = refScopes.size(); i > depth; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // 语句内物化的 owned 动态数组临时值：语句结束时释放
    void trackOwnedSliceTemp(Expr* e, Value* v, const TypePtr& ty) {
        if (!ty || ty->kind != TypeKind::Slice || !valueOwned(e)) return;
        auto* tmp = entryAlloca(lt(ty), "owned.tmp", alignOf(ty));
        b.CreateStore(v, tmp);
        stmtTemps.emplace_back(tmp, ty);
    }

    // owned 的 Str 右值：物化到语句临时并在语句结束释放 owner
    Value* evalStrValue(Expr* e) {
        Value* v = eval(e);
        if (valueOwned(e)) {
            auto* tmp = entryAlloca(lt(strType()), "str.tmp", alignOf(strType()));
            b.CreateStore(v, tmp);
            stmtTemps.emplace_back(tmp, strType());
        }
        return v;
    }

    void releaseAllScopes() {
        for (size_t i = refScopes.size(); i > 0; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // 释放语句执行期间物化的 owned 右值（从 mark 之后注册的），保持逆序
    void destroyTempsFrom(size_t mark) {
        if (stmtTemps.size() <= mark) return;
        for (size_t i = stmtTemps.size(); i > mark; --i) {
            emitReleaseValue(stmtTemps[i - 1].first, stmtTemps[i - 1].second);
        }
        stmtTemps.resize(mark);
    }

    // 表达式是否已经持有一份可转移的强引用（new / 返回 class 引用的调用）
    bool ownsReference(Expr* e) {
        if (!e || !isRefKind(e->ty)) return false;
        return valueOwned(e);
    }

    // 表达式结果是否已持有一份可转移的强引用（不限于 class 句柄，也用于引用数组）
    bool valueOwned(Expr* e) {
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
    bool convOwns(Expr* e, const TypePtr& dst) {
        if (dst && dst->kind == TypeKind::Slice && e && e->ty &&
            e->ty->kind == TypeKind::Array) {
            return true;
        }
        return valueOwned(e);
    }

    // 返回 class 引用时把借用变成一份新的强引用
    Value* prepareReturn(Expr* e) {
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

    // ------------------------------------------------------------ 分配
    // model_align：语义层对齐（align(N) 可能大于 LLVM 类型的自然对齐）
    AllocaInst* entryAlloca(LType* ty, const std::string& name, uint64_t model_align = 0) {
        IRBuilder<>::InsertPointGuard g(ab);
        if (allocaBB) ab.SetInsertPoint(allocaBB);
        auto* a = ab.CreateAlloca(ty, nullptr, name);
        if (model_align > 1) a->setAlignment(llvm::Align(model_align));
        return a;
    }

    AllocaInst* slotFor(const LocalVar* lv) {
        auto it = slots.find(lv);
        if (it != slots.end()) return it->second;
        LType* ty = lt(lv->rty);
        if (lv->by_ref) ty = ptrTy();
        auto* a = entryAlloca(ty, sane(lv->name), alignOf(lv->rty));
        slots[lv] = a;
        return a;
    }

    // ------------------------------------------------------------ 常量
    Constant* constValue(const TypePtr& ty, Init* init) {
        LType* ct = lt(ty);
        if (!init) return zeroConst(ct, ty);
        if (is_zero_literal(init)) return zeroConst(ct, ty);
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            if (ia->items.empty() && zeroable(ty)) return zeroConst(ct, ty);
            if (ty->kind == TypeKind::Struct) {
                std::vector<Constant*> vals(ty->fields.size(), nullptr);
                bool named = false;
                for (auto& it : ia->items) {
                    if (!it.first.empty()) named = true;
                }
                if (named) {
                    for (auto& it : ia->items) {
                        int idx = ty->field_index(it.first);
                        if (idx >= 0)
                            vals[idx] = constValue(ty->fields[idx].type, it.second.get());
                    }
                } else {
                    for (size_t i = 0; i < ia->items.size() && i < vals.size(); ++i)
                        vals[i] = constValue(ty->fields[i].type, ia->items[i].second.get());
                }
                for (size_t i = 0; i < vals.size(); ++i) {
                    if (!vals[i]) vals[i] = zeroConst(lt(ty->fields[i].type), ty->fields[i].type);
                }
                // LLVM 结构体可能有显式填充字段（packed / align(N)），按映射表展开
                auto* st = cast<LStruct>(ct);
                std::vector<Constant*> out(st->getNumElements(), nullptr);
                for (size_t i = 0; i < vals.size(); ++i) {
                    out[llvmFieldIndex(ty, static_cast<int>(i))] = vals[i];
                }
                for (size_t i = 0; i < out.size(); ++i) {
                    if (!out[i]) out[i] = Constant::getNullValue(st->getElementType(i));
                }
                return ConstantStruct::get(st, out);
            }
            if (ty->kind == TypeKind::Array) {
                std::vector<Constant*> vals;
                for (size_t i = 0; i < ty->array_size; ++i) {
                    if (i < ia->items.size())
                        vals.push_back(constValue(ty->elem, ia->items[i].second.get()));
                    else
                        vals.push_back(zeroConst(lt(ty->elem), ty->elem));
                }
                auto* inner = ConstantArray::get(LArray::get(lt(ty->elem), ty->array_size), vals);
                return ConstantStruct::get(cast<LStruct>(ct), {inner});
            }
            if (ty->kind == TypeKind::Slice) {
                std::vector<Constant*> vals;
                for (auto& it : ia->items) vals.push_back(constValue(ty->elem, it.second.get()));
                auto* arrTy = LArray::get(lt(ty->elem), vals.size());
                auto* data = ConstantArray::get(arrTy, vals);
                auto* hdrTy = LStruct::get(ctx, {i64(), i64(), i64(), arrTy}, false);
                Constant* hdr = ConstantStruct::get(
                    hdrTy, {ConstantInt::get(i64(), 1LL << 40),
                            ConstantInt::get(i64(), vals.size()),
                            ConstantInt::get(i64(), vals.size()), data});
                auto* gv = new GlobalVariable(*mod, hdrTy, /*isConstant=*/false,
                                              GlobalValue::PrivateLinkage, hdr, ".arr");
                return gv;
            }
            fail("LLVM 后端：不支持的全局聚合初始化", ia->line, ia->col);
        }
        auto ie = dynamic_cast<InitExpr*>(init);
        if (!ie) fail("LLVM 后端：未知初始化器");
        Expr* e = ie->expr.get();
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (isStrType(ty)) {
                fail("LLVM 后端：string 不能用整数初始化");
            }
            if (ty->kind == TypeKind::Slice && il->value == 0) {
                return Constant::getNullValue(ct);
            }
            // 整数字面量按上下文适配到浮点类型（SPEC §3.6）
            if (ty->kind == TypeKind::Prim && ty->is_float())
                return ConstantFP::get(ct, static_cast<double>(il->value));
            auto* it = IntegerType::get(ctx, static_cast<unsigned>(sizeOf(ty) * 8));
            return ConstantInt::get(it, static_cast<uint64_t>(il->value), /*isSigned=*/true);
        }
        if (auto fl = dynamic_cast<FloatLit*>(e))
            return ConstantFP::get(ct, fl->value);
        if (dynamic_cast<BoolLit*>(e))
            return ConstantInt::get(i1(), static_cast<BoolLit*>(e)->value ? 1 : 0);
        if (auto cl = dynamic_cast<CharLit*>(e)) return ConstantInt::get(i8(), cl->value);
        if (dynamic_cast<NullLit*>(e)) return Constant::getNullValue(ct);
        if (auto sl = dynamic_cast<StringLit*>(e)) {
            if (isStrType(ty)) return cast<Constant>(stringConst(sl->value));
            if (ty->kind == TypeKind::Array && sizeOf(ty->elem) == 1) {
                std::vector<uint8_t> buf(ty->array_size, 0);
                for (size_t i = 0; i < sl->value.size() && i + 1 < buf.size(); ++i)
                    buf[i] = static_cast<uint8_t>(sl->value[i]);
                auto* arrTy = LArray::get(i8(), ty->array_size);
                auto* inner = ConstantDataArray::get(ctx, buf);
                (void)arrTy;
                return ConstantStruct::get(cast<LStruct>(ct), {inner});
            }
        }
        fail("LLVM 后端：全局初始化器必须是常量", e->line, e->col);
    }

    // ------------------------------------------------------------ 表达式
    Value* convertTo(Value* v, const TypePtr& from, const TypePtr& to, int line = 0,
                     int col = 0) {
        if (!from || !to || same_type(from, to)) return v;
        LType* ft = v->getType();
        LType* tt = lt(to);
        if (ft == tt) return v;

        if (isStrType(from) && to->kind == TypeKind::Ptr)
            return b.CreateExtractValue(v, {0});
        if (from->kind == TypeKind::Null) return Constant::getNullValue(tt);
        if (from->kind == TypeKind::Class && to->kind == TypeKind::Interface) {
            Value* dp = b.CreateGEP(i8(), v, ConstantInt::get(i64(), 8));
            Value* d = b.CreateLoad(ptrTy(), dp);
            Value* iv = UndefValue::get(tt);
            iv = b.CreateInsertValue(iv, v, {0});
            iv = b.CreateInsertValue(iv, d, {1});
            return iv;
        }
        if (from->kind == TypeKind::Enum && to->kind == TypeKind::Prim)
            return convertTo(v, Type::make_prim("int"), to, line, col);

        if (is_integer(from) && is_integer(to)) {
            if (sizeOf(from) < sizeOf(to))
                return from && from->is_signed() ? b.CreateSExt(v, tt) : b.CreateZExt(v, tt);
            if (sizeOf(from) > sizeOf(to)) return b.CreateTrunc(v, tt);
            return v;
        }
        if (is_numeric(from) && is_numeric(to)) {
            if (from->kind == TypeKind::Prim && from->is_float() &&
                to->kind == TypeKind::Prim && to->is_float()) {
                return sizeOf(from) < sizeOf(to) ? b.CreateFPExt(v, tt) : b.CreateFPTrunc(v, tt);
            }
            if (from->kind == TypeKind::Prim && from->is_float()) {
                return from->is_signed() ? b.CreateFPToSI(v, tt) : b.CreateFPToUI(v, tt);
            }
            return to->kind == TypeKind::Prim && to->is_float()
                       ? (from->is_signed() ? b.CreateSIToFP(v, tt) : b.CreateUIToFP(v, tt))
                       : v;
        }
        if (from->kind == TypeKind::Ptr && is_integer(to)) return b.CreatePtrToInt(v, tt);
        if (is_integer(from) && to->kind == TypeKind::Ptr) return b.CreateIntToPtr(v, tt);
        if (from->kind == TypeKind::Ptr && to->kind == TypeKind::Ptr) return v;
        fail("LLVM 后端：无法转换 " + from->str() + " → " + to->str(), line, col);
    }

    bool isLvalue(Expr* e) {
        if (dynamic_cast<Name*>(e)) return true;
        if (dynamic_cast<Member*>(e)) return true;
        if (dynamic_cast<Index*>(e)) return true;
        if (auto u = dynamic_cast<Unary*>(e)) return u->op == "*";
        return false;
    }

    // 该表达式的地址是否可能未满足其类型的自然对齐（packed 结构链上的字段/数组元素）。
    // 保守判定：取不到时按自然对齐处理（规范里 `&p.field` 的解引用本身就是 UB）。
    bool unalignedExpr(Expr* e) {
        if (!e) return false;
        if (auto m = dynamic_cast<Member*>(e)) {
            TypePtr ot = m->obj->ty;
            if (ot && ot->kind == TypeKind::Struct) {
                if (ot->is_packed) return true;
                return unalignedExpr(m->obj.get());
            }
            if (ot && ot->kind == TypeKind::Class && m->field_index >= 0 &&
                m->field_index < static_cast<int>(ot->fields.size())) {
                // class 字段按模型对齐排布；若该字段仍落在未满足自身对齐的偏移上（理论上不该发生），
                // 保守按未对齐处理（例如含 packed 结构体字段时其成员访问）。
                uint64_t fa = alignOf(ot->fields[m->field_index].type);
                if (fa > 1 && classFieldOffset(ot, m->field_index) % fa != 0) return true;
                return unalignedExpr(m->obj.get());
            }
            return false;
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            TypePtr bt = ix->base->ty;
            if (!bt) return false;
            if (bt->kind == TypeKind::Array || bt->kind == TypeKind::Ptr) {
                TypePtr el = bt->elem;
                if (el && el->kind == TypeKind::Struct && el->is_packed) return true;
                return unalignedExpr(ix->base.get());
            }
            if (bt->kind == TypeKind::Slice) {
                TypePtr el = bt->elem;
                return el && el->kind == TypeKind::Struct && el->is_packed;
            }
            return false;
        }
        if (auto n = dynamic_cast<Name*>(e)) {
            // struct 方法内直接访问字段：this 指向的 packed 结构可能未对齐
            if (n->kind == "field" && n->implicit_this) {
                return curClassTy && curClassTy->kind == TypeKind::Struct && curClassTy->is_packed;
            }
            if (n->ident == "this") {
                return curClassTy && curClassTy->kind == TypeKind::Struct && curClassTy->is_packed;
            }
            return false;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op == "*" || u->op == "&") return unalignedExpr(u->operand.get());
            return false;
        }
        return false;
    }

    // 带校验的 GEP：把 LLVM 的断言错误变成可读的诊断。
    // 注意：第 0 个下标是"指针步进"，从第 1 个下标起才按聚合类型逐层索引。
    Value* gep(LType* ty, Value* ptr, std::initializer_list<Value*> idxs, int line = 0,
               int col = 0) {
        LType* cur = ty;
        size_t i = 0;
        for (auto it = idxs.begin(); it != idxs.end(); ++it, ++i) {
            if (i == 0) continue;
            if (auto* st = dyn_cast<LStruct>(cur)) {
                auto* ci = dyn_cast<ConstantInt>(*it);
                if (!ci) fail("LLVM 后端：结构体下标必须是常量", line, col);
                unsigned f = static_cast<unsigned>(ci->getZExtValue());
                if (f >= st->getNumElements())
                    fail("LLVM 后端：结构体字段下标越界（" + std::to_string(f) + "/" +
                             std::to_string(st->getNumElements()) + "）",
                         line, col);
                cur = st->getElementType(f);
            } else if (auto* at = dyn_cast<LArray>(cur)) {
                cur = at->getElementType();
            } else {
                fail("LLVM 后端：GEP 下标过多（基类型不是聚合）", line, col);
            }
        }
        return b.CreateGEP(ty, ptr, idxs);
    }

    Value* gep(LType* ty, Value* ptr, Value* idx, int line = 0, int col = 0) {
        return gep(ty, ptr, {idx}, line, col);
    }

    Value* addrOf(Expr* e) {
        if (auto n = dynamic_cast<Name*>(e)) {
            if (n->ident == "this") {
                // `this` 是当前对象的引用：struct 的接收者本身是对象地址，
                // class/interface 的接收者是句柄（值），定位到句柄槽即可。
                if (curClassTy && curClassTy->kind == TypeKind::Struct)
                    return b.CreateLoad(ptrTy(), thisSlot);
                return thisSlot;
            }
            if (n->kind == "field" && n->implicit_this) {
                if (!curClassTy || !thisSlot) fail("LLVM 后端：隐式字段访问缺少 this", n->line, n->col);
                return fieldAddr(b.CreateLoad(ptrTy(), thisSlot), curClassTy, n->field_index);
            }
            if (auto lv = std::get_if<LocalVar*>(&n->sym)) {
                AllocaInst* a = slotFor(*lv);
                if ((*lv)->by_ref) return b.CreateLoad(ptrTy(), a);
                return a;
            }
            if (auto gv = std::get_if<GlobalVar*>(&n->sym)) {
                return mod->getOrInsertGlobal((*gv)->cname, lt((*gv)->rty));
            }
            fail("LLVM 后端：名字 `" + n->ident + "` 不可寻址", n->line, n->col);
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            if (m->kind == "global") {
                auto* gv = std::get_if<GlobalVar*>(&m->sym);
                if (gv) return mod->getOrInsertGlobal((*gv)->cname, lt((*gv)->rty));
            }
            TypePtr ot = m->obj->ty;
            if (ot->kind == TypeKind::Class) {
                Value* p = eval(m->obj.get());
                return fieldAddr(p, ot, m->field_index);
            }
            Value* base = addrOf(m->obj.get());
            if (ot->kind != TypeKind::Struct)
                fail("LLVM 后端：成员访问的基类型不是结构体", m->line, m->col);
            auto* st = cast<LStruct>(lt(ot));
            return b.CreateStructGEP(st, base, llvmFieldIndex(ot, m->field_index));
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            TypePtr bt = ix->base->ty;
            std::vector<Value*> idxs;
            for (auto& ie : ix->indices) {
                idxs.push_back(convertTo(eval(ie.get()), ie->ty,
                                         Type::make_prim("usize"), ix->line, ix->col));
            }
            if (isStrType(bt)) {
                if (idxs.size() != 1) fail("LLVM 后端：string 只接受一个下标", ix->line, ix->col);
                Value* sv = evalStrValue(ix->base.get());
                Value* p = b.CreateExtractValue(sv, {0});
                return b.CreateGEP(i8(), p, idxs[0]);
            }
            if (bt->kind == TypeKind::Array) {
                Value* addr = addrOf(ix->base.get());
                TypePtr curTy = bt;
                for (size_t k = 0; k < idxs.size(); ++k) {
                    auto* st = cast<LStruct>(lt(curTy));
                    addr = gep(st, addr, {ConstantInt::get(i64(), 0),
                                          ConstantInt::get(i32(), 0), idxs[k]});
                    curTy = curTy->elem;
                }
                return addr;
            }
            if (bt->kind == TypeKind::Slice) {
                Value* h = eval(ix->base.get());
                trackOwnedSliceTemp(ix->base.get(), h, bt);
                Value* lin = idxs.empty() ? ConstantInt::get(i64(), 0) : idxs[0];
                for (size_t k = 1; k < idxs.size(); ++k) {
                    Value* dk = sliceDimField(h, static_cast<int>(k));
                    lin = b.CreateAdd(b.CreateMul(lin, dk), idxs[k]);
                }
                Value* data = sliceDataPtr(h, bt->rank);
                return b.CreateGEP(lt(bt->elem), data, lin);
            }
            if (bt->kind == TypeKind::Ptr) {
                if (idxs.size() != 1) fail("LLVM 后端：指针下标数量错误", ix->line, ix->col);
                return gep(lt(bt->elem), eval(ix->base.get()), idxs[0]);
            }
            if (bt->kind == TypeKind::String) {
                Value* sv = eval(ix->base.get());
                return gep(i8(), b.CreateExtractValue(sv, {0}), idxs[0]);
            }
            fail("LLVM 后端：不可下标访问", ix->line, ix->col);
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op == "*") return eval(u->operand.get());
        }
        // 其它表达式：先求值到临时量
        Value* v = eval(e);
        auto* tmp = entryAlloca(v->getType(), "tmp", e->ty ? alignOf(e->ty) : 0);
        b.CreateStore(v, tmp);
        // owned 的 struct/array 右值被物化后，语句结束时要释放其引用字段
        if (e->ty && needsCleanup(e->ty) && valueOwned(e) &&
            (e->ty->kind == TypeKind::Struct || e->ty->kind == TypeKind::Array ||
             e->ty->kind == TypeKind::Slice)) {
            stmtTemps.emplace_back(tmp, e->ty);
        }
        return tmp;
    }

    Value* compareOp(const std::string& op, Value* l, Value* r, bool is_signed, bool is_float) {
        if (is_float) {
            if (op == "==") return b.CreateFCmpOEQ(l, r);
            if (op == "!=") return b.CreateFCmpUNE(l, r);
            if (op == "<") return b.CreateFCmpOLT(l, r);
            if (op == "<=") return b.CreateFCmpOLE(l, r);
            if (op == ">") return b.CreateFCmpOGT(l, r);
            return b.CreateFCmpOGE(l, r);
        }
        if (op == "==") return b.CreateICmpEQ(l, r);
        if (op == "!=") return b.CreateICmpNE(l, r);
        if (op == "<") return is_signed ? b.CreateICmpSLT(l, r) : b.CreateICmpULT(l, r);
        if (op == "<=") return is_signed ? b.CreateICmpSLE(l, r) : b.CreateICmpULE(l, r);
        if (op == ">") return is_signed ? b.CreateICmpSGT(l, r) : b.CreateICmpUGT(l, r);
        return is_signed ? b.CreateICmpSGE(l, r) : b.CreateICmpUGE(l, r);
    }

    Value* coerce(Expr* e, const TypePtr& to) {
        if (e && to && to->kind == TypeKind::Slice && e->ty &&
            e->ty->kind == TypeKind::Array) {
            // 定长数组 -> 动态数组：按嵌套维度分配并复制（不再提供借用视图）
            return emitDynArrayFromArray(addrOf(e), e->ty, to);
        }
        return convertTo(eval(e), e->ty, to);
    }

    Value* evalBinary(Binary* e) {
        const std::string& op = e->op;
        TypePtr lt_ = e->left->ty;
        TypePtr rt_ = e->right->ty;

        if (e->op_func) {
            return emitUserCall(e->op_func, {e->left.get(), e->right.get()});
        }

        if (op == "string_eq_helper") return nullptr;

        if (op == "and" || op == "or") {
            Value* l = eval(e->left.get());
            BasicBlock* rhsBB = BasicBlock::Create(ctx, "sc.rhs", curFn);
            BasicBlock* endBB = BasicBlock::Create(ctx, "sc.end", curFn);
            BasicBlock* lhsEnd = b.GetInsertBlock();
            if (op == "and")
                b.CreateCondBr(l, rhsBB, endBB);
            else
                b.CreateCondBr(l, endBB, rhsBB);
            b.SetInsertPoint(rhsBB);
            Value* r = eval(e->right.get());
            BasicBlock* rhsEnd = b.GetInsertBlock();
            if (!hasTerm(rhsEnd)) b.CreateBr(endBB);
            b.SetInsertPoint(endBB);
            PHINode* phi = b.CreatePHI(i1(), 2);
            phi->addIncoming(ConstantInt::get(i1(), op == "and" ? 0 : 1), lhsEnd);
            phi->addIncoming(r, rhsEnd);
            return phi;
        }

        // string == / !=
        if ((op == "==" || op == "!=") && isStrType(lt_) && isStrType(rt_)) {
            Value* l = evalStrValue(e->left.get());
            Value* r = evalStrValue(e->right.get());
            Value* lp = b.CreateExtractValue(l, {0});
            Value* ll = b.CreateExtractValue(l, {1});
            Value* rp = b.CreateExtractValue(r, {0});
            Value* rl = b.CreateExtractValue(r, {1});
            auto memcmpFn = libFn("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
            Value* cmp = b.CreateCall(memcmpFn, {lp, rp, b.CreateSelect(
                                                            b.CreateICmpULT(ll, rl), ll, rl)});
            Value* eq = b.CreateAnd(b.CreateICmpEQ(ll, rl), b.CreateICmpEQ(cmp, ConstantInt::get(i32(), 0)));
            return op == "==" ? eq : b.CreateNot(eq);
        }

        // 操作数只求值一次（后面所有分支复用）
        Value* l = eval(e->left.get());
        Value* r = eval(e->right.get());

        // 指针算术
        if ((op == "+" || op == "-") && lt_->kind == TypeKind::Ptr && is_integer(rt_)) {
            Value* n = convertTo(r, rt_, Type::make_prim("isize"));
            return gep(lt(lt_->elem), l, n);
        }
        if (op == "+" && is_integer(lt_) && rt_->kind == TypeKind::Ptr) {
            Value* n = convertTo(l, lt_, Type::make_prim("isize"));
            return gep(lt(rt_->elem), r, n);
        }
        if (op == "-" && lt_->kind == TypeKind::Ptr && rt_->kind == TypeKind::Ptr) {
            Value* a = b.CreatePtrToInt(l, i64());
            Value* c = b.CreatePtrToInt(r, i64());
            Value* d = b.CreateSub(a, c);
            uint64_t sz = sizeOf(lt_->elem);
            return b.CreateSDiv(d, ConstantInt::get(i64(), sz ? sz : 1));
        }

        if (op == "==" || op == "!=") {
            bool lref = isRefKind(lt_) || lt_->kind == TypeKind::Null;
            bool rref = isRefKind(rt_) || rt_->kind == TypeKind::Null;
            if (lref && rref && (isRefKind(lt_) || isRefKind(rt_))) {
                Value* lobj = (isRefKind(lt_) && lt_->kind != TypeKind::Null) ? refObj(l, lt_) : l;
                Value* robj = (isRefKind(rt_) && rt_->kind != TypeKind::Null) ? refObj(r, rt_) : r;
                if (lt_->kind == TypeKind::Null) lobj = Constant::getNullValue(ptrTy());
                if (rt_->kind == TypeKind::Null) robj = Constant::getNullValue(ptrTy());
                Value* cmp = compareOp(op, lobj, robj, false, false);
                // 比较后释放临时的 owned 引用（new / 返回引用的调用）
                if (ownsReference(e->left.get())) emitReleaseRef(l, lt_);
                if (ownsReference(e->right.get())) emitReleaseRef(r, rt_);
                return cmp;
            }
        }
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (lt_->kind == TypeKind::Ptr || rt_->kind == TypeKind::Ptr) {
                Value* lv = l;
                Value* rv = r;
                if (lt_->kind == TypeKind::Null) lv = Constant::getNullValue(ptrTy());
                if (rt_->kind == TypeKind::Null) rv = Constant::getNullValue(ptrTy());
                return compareOp(op, lv, rv, false, false);
            }
            if (same_type(lt_, rt_) && lt_->kind == TypeKind::Prim && lt_->is_bool()) {
                return compareOp(op, l, r, false, false);
            }
            // 枚举：只与同一枚举比较，按底层 int 的有符号序比较（SPEC §3.5）
            if (lt_->kind == TypeKind::Enum && rt_->kind == TypeKind::Enum &&
                same_type(lt_, rt_)) {
                l = convertTo(l, lt_, Type::make_prim("int"));
                r = convertTo(r, rt_, Type::make_prim("int"));
                return compareOp(op, l, r, true, false);
            }
            TypePtr common = usual_arith(lt_, rt_);
            if (!common) fail("LLVM 后端：比较操作数类型不一致", e->line, e->col);
            l = convertTo(l, lt_, common);
            r = convertTo(r, rt_, common);
            bool fl = common->kind == TypeKind::Prim && common->is_float();
            bool sg = common->kind == TypeKind::Prim && common->is_signed();
            return compareOp(op, l, r, sg, fl);
        }

        if (op == "<<" || op == ">>") {
            l = convertTo(l, lt_, e->ty);
            r = convertTo(r, rt_, e->ty);
            return op == "<<" ? b.CreateShl(l, r) : b.CreateAShr(l, r);
        }

        TypePtr res = e->ty;
        l = convertTo(l, lt_, res);
        r = convertTo(r, rt_, res);
        bool fl = res->kind == TypeKind::Prim && res->is_float();
        bool sg = res->kind == TypeKind::Prim && res->is_signed();
        if (op == "+") return fl ? b.CreateFAdd(l, r) : b.CreateAdd(l, r);
        if (op == "-") return fl ? b.CreateFSub(l, r) : b.CreateSub(l, r);
        if (op == "*") return fl ? b.CreateFMul(l, r) : b.CreateMul(l, r);
        if (op == "/") return fl ? b.CreateFDiv(l, r) : (sg ? b.CreateSDiv(l, r) : b.CreateUDiv(l, r));
        if (op == "%") return fl ? b.CreateFRem(l, r) : (sg ? b.CreateSRem(l, r) : b.CreateURem(l, r));
        if (op == "&") return b.CreateAnd(l, r);
        if (op == "|") return b.CreateOr(l, r);
        if (op == "^") return b.CreateXor(l, r);
        fail("LLVM 后端：未知二元运算 " + op, e->line, e->col);
    }

    // 条件表达式：只求值被选中的分支，结果物化到 entry 块的结果槽后按 owned 值返回。
    Value* evalTernary(Ternary* e) {
        Value* cond = eval(e->cond.get());
        BasicBlock* thenBB = BasicBlock::Create(ctx, "tern.then", curFn);
        BasicBlock* elseBB = BasicBlock::Create(ctx, "tern.else", curFn);
        BasicBlock* endBB = BasicBlock::Create(ctx, "tern.end", curFn);
        b.CreateCondBr(cond, thenBB, elseBB);

        const TypePtr& ty = e->ty;
        AllocaInst* res = is_void(ty) ? nullptr : entryAlloca(lt(ty), "tern.res", alignOf(ty));

        b.SetInsertPoint(thenBB);
        emitTernaryArm(res, e->then_e.get(), ty);
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);

        b.SetInsertPoint(elseBB);
        emitTernaryArm(res, e->else_e.get(), ty);
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);

        b.SetInsertPoint(endBB);
        if (!res) return nullptr;
        return loadAt(lt(ty), res);
    }

    // 把分支值写入结果槽：借用源逐字段 retain，owned 源直接转移所有权（与 initInto 一致）
    void emitTernaryArm(Value* addr, Expr* src, const TypePtr& ty) {
        if (!addr) {  // void 分支：只求值
            eval(src);
            return;
        }
        Value* v = coerce(src, ty);
        if (isRefKind(ty)) {
            if (!ownsReference(src)) emitRetainRef(v, ty);
            storeValue(v, addr, ty);
            return;
        }
        if ((ty->kind == TypeKind::Array || ty->kind == TypeKind::Struct) &&
            needsCleanup(ty)) {
            storeValue(v, addr, ty);
            if (!valueOwned(src)) emitRetainValue(addr, ty);
            return;
        }
        if (ty->kind == TypeKind::Slice) {
            storeValue(v, addr, ty);
            if (!convOwns(src, ty)) emitRetainValue(addr, ty);
            return;
        }
        storeValue(v, addr, ty);
    }

    // 调用参数：求值 + 记录需要释放的临时强引用（new / 返回引用的调用）
    struct ArgPack {
        std::vector<Value*> vals;
        std::vector<std::pair<Value*, TypePtr>> owned;
    };

    ArgPack prepareArgs(const std::vector<Expr*>& args, const std::vector<TypePtr>& ptys) {
        ArgPack pack;
        for (size_t i = 0; i < args.size() && i < ptys.size(); ++i) {
            Value* v = coerce(args[i], ptys[i]);
            // 参数按借用传递（callee 不释放）；只有调用者持有所有权的实参临时值才需在调用后清理
            if (needsCleanup(ptys[i]) && convOwns(args[i], ptys[i])) {
                pack.owned.emplace_back(v, ptys[i]);
            }
            pack.vals.push_back(v);
        }
        return pack;
    }

    void releaseTemps(ArgPack& pack) {
        for (auto& [v, ty] : pack.owned) {
            if (isRefKind(ty)) {
                emitReleaseRef(v, ty);
            } else if (ty && needsCleanup(ty)) {
                auto* tmp = entryAlloca(lt(ty), "arg.tmp", alignOf(ty));
                b.CreateStore(v, tmp);
                emitReleaseValue(tmp, ty);
            }
        }
        pack.owned.clear();
    }

    // 调用自由函数/运算符/方法（静态派发）
    Value* emitUserCall(FuncDecl* f, const std::vector<Expr*>& args) {
        std::vector<LType*> ptys;
        for (auto& t : f->pty) ptys.push_back(lt(t));
        FunctionCallee callee =
            mod->getOrInsertFunction(f->cname, FunctionType::get(lt(f->rty), ptys, false));
        ArgPack pack = prepareArgs(args, f->pty);
        Value* r = b.CreateCall(callee, pack.vals);
        releaseTemps(pack);
        return r;
    }

    Value* evalNew(NewExpr* ne) {
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
            // _aligned_malloc(size, alignment)：对象对齐超过 malloc 的 16 字节保证（对应 _aligned_free）
            p = b.CreateCall(libFn("_aligned_malloc", ptrTy(), {i64(), i64()}),
                             {ConstantInt::get(i64(), sz),
                              ConstantInt::get(i64(), objectAlign(cls))});
        } else {
            p = b.CreateCall(libFn("malloc", ptrTy(), {i64()}), {ConstantInt::get(i64(), sz)});
        }
        // 分配失败直接 abort（语言无异常）
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
        } else {
            // 隐式默认构造：调用最近祖先声明的无参构造器（它会继续链式调用自己的基类）
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
                    }
                    break;
                }
                b0 = b0->base;
            }
            storeDesc();  // 祖先构造器可能改写了 desc，恢复为最终类型
        }
        return p;
    }

    Value* evalCall(Call* c) {
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
            return b.CreateLoad(lt(st), tmp);
        }
        if (c->callee_kind == "struct_method") {
            const MethodSig* sig = c->method;
            if (!sig || !sig->owner) fail("LLVM 后端：struct 方法缺少签名", c->line, c->col);
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
            return r;
        }
        if (c->callee_kind == "method") {
            const MethodSig* sig = c->method;
            if (!sig || !sig->owner) fail("LLVM 后端：方法调用缺少签名", c->line, c->col);
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
            std::vector<LType*> ptys{ptrTy()};
            for (auto& t : sig->params) ptys.push_back(lt(t));
            auto* fnTy = FunctionType::get(lt(sig->ret), ptys, false);
            std::vector<Expr*> rawArgs;
            for (auto& a : c->args) rawArgs.push_back(a.get());
            ArgPack pack = prepareArgs(rawArgs, sig->params);

            bool ifaceCall = (recvTy && recvTy->kind == TypeKind::Interface) ||
                             (sig->owner && sig->owner->kind == TypeKind::Interface);
            if (ifaceCall) {
                auto it = islotIndex.find(sigKey(*sig));
                if (it == islotIndex.end()) {
                    fail("LLVM 后端：接口方法未进入 itable", c->line, c->col);
                }
                int idx = it->second;
                Value* obj = nullptr;
                Value* dsc = nullptr;
                if (recvTy && recvTy->kind == TypeKind::Interface) {
                    obj = b.CreateExtractValue(recv, {0});
                    dsc = b.CreateExtractValue(recv, {1});
                } else {
                    // 类变量上调用接口默认方法：现场构造接口值
                    obj = recv;
                    Value* dpp = b.CreateGEP(i8(), recv, ConstantInt::get(i64(), 8));
                    dsc = b.CreateLoad(ptrTy(), dpp);
                }
                Value* itp = b.CreateGEP(i8(), dsc, ConstantInt::get(i64(), 24));
                Value* itab = b.CreateLoad(ptrTy(), itp);
                Value* fnp = b.CreateGEP(ptrTy(), itab, ConstantInt::get(i64(), idx));
                Value* fn = b.CreateLoad(ptrTy(), fnp);
                std::vector<LType*> ip{ptrTy(), ptrTy()};
                for (auto& t : sig->params) ip.push_back(lt(t));
                auto* ifnTy = FunctionType::get(lt(sig->ret), ip, false);
                std::vector<Value*> iargs{obj, dsc};
                for (auto* v : pack.vals) iargs.push_back(v);
                Value* r = b.CreateCall(ifnTy, fn, iargs);
                releaseTemps(pack);
                if (recvOwned) emitReleaseRef(recv, recvTy);
                return r;
            }

            int idx = slotOf(recvTy, sig);
            if (idx >= 0) {
                Value* dp = b.CreateGEP(i8(), recv, ConstantInt::get(i64(), 8));
                Value* desc = b.CreateLoad(ptrTy(), dp);
                Value* vtp = b.CreateGEP(i8(), desc, ConstantInt::get(i64(), 16));
                Value* vt = b.CreateLoad(ptrTy(), vtp);
                Value* fnp = b.CreateGEP(ptrTy(), vt, ConstantInt::get(i64(), idx));
                Value* fn = b.CreateLoad(ptrTy(), fnp);
                std::vector<Value*> vals{recv};
                for (auto* v : pack.vals) vals.push_back(v);
                Value* r = b.CreateCall(fnTy, fn, vals);
                releaseTemps(pack);
                if (recvOwned) emitReleaseRef(recv, recvTy);
                return r;
            }
            // 未进虚表：静态调用声明处实现
            FunctionCallee callee = methodFn(sig);
            std::vector<Value*> vals{recv};
            for (auto* v : pack.vals) vals.push_back(v);
            Value* r = b.CreateCall(callee, vals);
            releaseTemps(pack);
            if (recvOwned) emitReleaseRef(recv, recvTy);
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
                return r;
            }
            // super(...)：调用基类构造器，然后恢复派生类的 desc
            TypePtr base = curClassTy ? curClassTy->base : nullptr;
            if (!base || base->kind != TypeKind::Class)
                fail("LLVM 后端：super(...) 缺少基类", c->line, c->col);
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
            }
            if (curClassTy) {
                Value* p = b.CreateLoad(ptrTy(), thisSlot);
                Value* dp = b.CreateGEP(i8(), p, ConstantInt::get(i64(), 8));
                b.CreateStore(descOf(curClassTy), dp);
            }
            return nullptr;
        }
        // 普通函数调用
        FuncDecl* f = c->func;
        if (!f) fail("LLVM 后端：调用缺少目标函数", c->line, c->col);
        std::vector<LType*> ptys;
        for (auto& t : f->pty) ptys.push_back(lt(t));
        FunctionCallee callee = mod->getOrInsertFunction(
            f->cname, FunctionType::get(lt(f->rty), ptys, false));
        std::vector<Expr*> rawArgs;
        for (auto& a : c->args) rawArgs.push_back(a.get());
        ArgPack pack = prepareArgs(rawArgs, f->pty);
        Value* r = b.CreateCall(callee, pack.vals);
        releaseTemps(pack);
        return r;
    }

    // 块是否已有终结指令（getTerminator 在空块上会断言）
    static bool hasTerm(BasicBlock* bb) {
        return bb && !bb->empty() && bb->back().isTerminator();
    }

    // 引用类型向下/向上转换（运行期检查 desc 链；失败得 null）
    Value* emitDowncast(Value* src, const TypePtr& from, const TypePtr& to) {
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

    Value* eval(Expr* e) {
        if (auto v = dynamic_cast<IntLit*>(e)) {
            if (isRefKind(e->ty)) return Constant::getNullValue(lt(e->ty));
            if (e->ty && e->ty->kind == TypeKind::Null) return Constant::getNullValue(ptrTy());
            LType* ty = lt(e->ty);
            // 整数字面量按上下文适配到浮点类型（SPEC §3.6）
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
            if (n->kind == "func") fail("LLVM 后端：函数不能作为值", e->line, e->col);
            return loadAt(lt(e->ty), addrOf(e), unalignedExpr(e));
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            if (m->kind == "enum_const") {
                auto* ec = std::get_if<EnumConst*>(&m->sym);
                if (!ec) fail("LLVM 后端：缺少枚举常量", e->line, e->col);
                return ConstantInt::get(i32(), static_cast<uint64_t>((*ec)->value), true);
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
            if (ix->get_func) {
                return emitUserCall(ix->get_func, {ix->base.get(), ix->indices[0].get()});
            }
            return loadAt(lt(e->ty), addrOf(e), unalignedExpr(e));
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
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
        if (auto v = dynamic_cast<Call*>(e)) return evalCall(v);
        if (auto v = dynamic_cast<NewExpr*>(e)) return evalNew(v);
        if (auto v = dynamic_cast<SizeofExpr*>(e)) {
            // resolve spec via sema 已写入 e->ty=usize；尺寸需要类型本身：由 spec 推断
            TypePtr t = specType(v->spec.get());
            uint64_t n = v->which == "alignof" ? alignOf(t) : sizeOf(t);
            return ConstantInt::get(i64(), n);
        }
        if (auto v = dynamic_cast<Len*>(e)) {
            TypePtr st = v->seq->ty;
            int axis = 0;
            Value* axisV = nullptr;
            if (isStrType(st)) {
                Value* s = evalStrValue(v->seq.get());
                return b.CreateExtractValue(s, {1});
            }
            if (v->dim) {
                if (auto il = dynamic_cast<IntLit*>(v->dim.get())) {
                    axis = static_cast<int>(il->value);
                } else if (st->kind == TypeKind::Slice && st->rank > 1) {
                    axisV = convertTo(eval(v->dim.get()), v->dim->ty,
                                      Type::make_prim("usize"));
                } else {
                    fail("静态数组/一维数组的 `len(seq, k)` 维度必须是整数字面量",
                         v->dim->line, v->dim->col);
                }
            }
            if (st->kind == TypeKind::Array) {
                TypePtr cur = st;
                for (int i = 0; i < axis; ++i) {
                    if (!cur || cur->kind != TypeKind::Array)
                        fail("`len` 维度越界", v->line, v->col);
                    cur = cur->elem;
                }
                if (!cur || cur->kind != TypeKind::Array)
                    fail("`len` 维度越界", v->line, v->col);
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
        fail("LLVM 后端：未知表达式", e->line, e->col);
    }

    // sizeof/alignof 的 TypeSpec 解析（只依赖已解析的类型模型）
    TypePtr specType(TypeSpec* spec) {
        if (!spec) return nullptr;
        curSpecUnitFix();
        switch (spec->kind) {
            case TypeSpec::K::Named: {
                if (spec->name == "void") return Type::make_void();
                if (spec->name == "string") return Type::make_string();
                if (is_prim_name(spec->name)) return Type::make_prim(spec->name);
                if (spec->name.find('.') != std::string::npos) {
                    auto p = spec->name.find('.');
                    std::string un = spec->name.substr(0, p), tn = spec->name.substr(p + 1);
                    auto it = prog.infos.find(un);
                    if (it != prog.infos.end()) {
                        auto s = it->second.structs.find(tn);
                        if (s != it->second.structs.end()) return s->second->ty;
                        auto c = it->second.classes.find(tn);
                        if (c != it->second.classes.end()) return c->second->ty;
                        auto i2 = it->second.interfaces.find(tn);
                        if (i2 != it->second.interfaces.end()) return i2->second->ty;
                        auto e = it->second.enums.find(tn);
                        if (e != it->second.enums.end()) return e->second->ty;
                    }
                }
                auto it = prog.infos[curUnit].structs.find(spec->name);
                if (it != prog.infos[curUnit].structs.end()) return it->second->ty;
                auto c = prog.infos[curUnit].classes.find(spec->name);
                if (c != prog.infos[curUnit].classes.end()) return c->second->ty;
                auto i2 = prog.infos[curUnit].interfaces.find(spec->name);
                if (i2 != prog.infos[curUnit].interfaces.end()) return i2->second->ty;
                auto e = prog.infos[curUnit].enums.find(spec->name);
                if (e != prog.infos[curUnit].enums.end()) return e->second->ty;
                fail("LLVM 后端：sizeof/alignof 未知类型 " + spec->name);
            }
            case TypeSpec::K::Ptr:
                return Type::make_ptr(specType(spec->elem.get()), spec->is_const);
            case TypeSpec::K::Slice:
                return Type::make_slice(specType(spec->elem.get()), spec->is_const);
            case TypeSpec::K::Array:
                return Type::make_array(specType(spec->elem.get()), spec->size);
        }
        fail("LLVM 后端：未知类型 spec");
    }
    void curSpecUnitFix() {}

    // ------------------------------------------------------------ 语句
    // 访问对齐：packed 结构链上的字段/元素只保证 1 字节对齐，必须把 load/store
    // 的 align 显式降到 1，否则 LLVM 会按脚点类型对齐生成错误的非对齐访问。
    void storeAt(Value* v, Value* addr, bool unaligned = false) {
        if (!v) return;
        auto* si = b.CreateStore(v, addr);
        if (unaligned) si->setAlignment(llvm::Align(1));
    }

    Value* loadAt(LType* ty, Value* addr, bool unaligned = false) {
        auto* li = b.CreateLoad(ty, addr);
        if (unaligned) li->setAlignment(llvm::Align(1));
        return li;
    }

    void storeValue(Value* v, Value* addr, const TypePtr& ty, bool unaligned = false) {
        if (!v) return;
        if (ty->kind == TypeKind::Void) return;
        storeAt(v, addr, unaligned);
    }

    void zeroMem(Value* addr, const TypePtr& ty, bool unaligned = false) {
        uint64_t n = sizeOf(ty);
        if (n == 0) return;
        b.CreateMemSet(addr, ConstantInt::get(i8(), 0), ConstantInt::get(i64(), n),
                       MaybeAlign(unaligned ? 1ULL : static_cast<uint64_t>(alignOf(ty))));
    }

    void copyMem(Value* dst, Value* src, uint64_t n) {
        if (n == 0) return;
        // 只承诺 1 字节对齐：源/目的可能是 packed 结构里的字段
        b.CreateMemCpy(dst, MaybeAlign(1), src, MaybeAlign(1), ConstantInt::get(i64(), n));
    }

    void initInto(Value* addr, const TypePtr& ty, Init* init, bool unaligned = false) {
        if (std::getenv("MULANC_LLVM_TRACE"))
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
            if (auto call = dynamic_cast<Call*>(e);
                call && call->callee_kind == "struct_ctor") {
                TypePtr st = call->ctor;
                const auto* cpty = call->ctor_pty;
                // 构造器可能不写全部引用字段：先清零，保证析构时不会释放垃圾指针
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
                return;
            }
            if (ty->kind == TypeKind::Array && sizeOf(ty->elem) == 1 &&
                dynamic_cast<StringLit*>(e)) {
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       char-array from string\n";
                Value* s = stringConst(static_cast<StringLit*>(e)->value);
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       A\n";
                Value* sp = b.CreateExtractValue(s, {0});
                Value* sl = b.CreateExtractValue(s, {1});
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       B\n";
                zeroMem(addr, ty, unaligned);
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       C\n";
                b.CreateMemCpy(addr, MaybeAlign(unaligned ? 1 : alignOf(ty)), sp, MaybeAlign(1), sl);
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       D\n";
                return;
            }
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
            if (ty->kind == TypeKind::Struct && needsCleanup(ty)) {
                // 借用源：逐字段 retain；owned 源（调用返回/构造结果）：所有权转移
                storeValue(v, addr, ty, unaligned);
                if (!valueOwned(e)) emitRetainValue(addr, ty, unaligned);
                return;
            }
            if (ty->kind == TypeKind::Slice) {
                // 借用源（另一个动态数组）：retain；new/数组转换/调用返回：所有权转移
                storeValue(v, addr, ty, unaligned);
                if (!convOwns(e, ty)) emitRetainValue(addr, ty, unaligned);
                return;
            }
            storeValue(v, addr, ty, unaligned);
            return;
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            // 空花括号 `{}`：聚合零初始化（等价于旧的 `= 0`）
            if (ia->items.empty()) {
                zeroMem(addr, ty, unaligned);
                return;
            }
            // 与 C 一致：聚合初始化未覆盖的部分清零
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
                for (auto& [idx, sub] : slot) {
                    Value* fp =
                        b.CreateStructGEP(st, addr, llvmFieldIndex(ty, idx));
                    initInto(fp, ty->fields[idx].type, sub, unaligned || ty->is_packed);
                }
                return;
            }
            if (ty->kind == TypeKind::Array) {
                auto* st = cast<LStruct>(lt(ty));
                Value* inner = gep(st, addr, {ConstantInt::get(i64(), 0),
                                                       ConstantInt::get(i32(), 0)});
                for (size_t i = 0; i < ia->items.size() && i < ty->array_size; ++i) {
                    Value* p = gep(lt(ty->elem), inner, ConstantInt::get(i64(), i));
                    initInto(p, ty->elem, ia->items[i].second.get(), unaligned);
                }
                return;
            }
            if (ty->kind == TypeKind::Slice) {
                std::vector<Init*> elems;
                for (auto& it : ia->items) elems.push_back(it.second.get());
                Value* h = emitDynArrayAlloc(ConstantInt::get(i64(), elems.size()), ty->elem);
                Value* data = sliceDataPtr(h, ty->rank);
                for (size_t i = 0; i < elems.size(); ++i) {
                    Value* p = b.CreateGEP(lt(ty->elem), data, ConstantInt::get(i64(), i));
                    initInto(p, ty->elem, elems[i], unaligned);
                }
                storeAt(h, addr, unaligned);
                return;
            }
        }
        fail("LLVM 后端：不支持的初始化", init->line, init->col);
    }

    void block(Block* blk) {
        if (!blk) return;
        pushRefScope();
        for (auto& x : blk->stmts) stmt(x.get());
        popRefScope();
    }

    void stmt(Stmt* s) {
        size_t mark = stmtTemps.size();
        stmtImpl(s);
        destroyTempsFrom(mark);
    }

    void stmtImpl(Stmt* s) {
        if (std::getenv("MULANC_LLVM_TRACE")) {
            std::cerr << "[llvm]   stmt " << typeid(*s).name();
            if (auto lv = dynamic_cast<LocalVar*>(s)) std::cerr << " " << lv->name;
            std::cerr << "\n";
        }
        if (auto b0 = dynamic_cast<Block*>(s)) {
            block(b0);
            return;
        }
        if (auto lv = dynamic_cast<LocalVar*>(s)) {
            AllocaInst* slot = slotFor(lv);
            if (lv->init) {
                initInto(slot, lv->rty, lv->init.get());
            } else if (isRefKind(lv->rty)) {
                b.CreateStore(Constant::getNullValue(lt(lv->rty)), slot);
            } else if (lv->rty && lv->rty->kind == TypeKind::Struct) {
                if (!lv->rty->ctors.empty()) {
                    FunctionCallee ctor = mod->getOrInsertFunction(
                        ctor_cname(lv->rty->unit, lv->rty->name, {}),
                        FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
                    if (lv->rty->needs_cleanup()) zeroMem(slot, lv->rty);
                    b.CreateCall(ctor, {slot});
                } else if (needsCleanup(lv->rty)) {
                    zeroMem(slot, lv->rty);
                }
            } else if (lv->rty && lv->rty->kind == TypeKind::Slice) {
                b.CreateStore(Constant::getNullValue(ptrTy()), slot);
            } else if (lv->rty && lv->rty->kind == TypeKind::Array && needsCleanup(lv->rty)) {
                zeroMem(slot, lv->rty);
            }
            if (needsCleanup(lv->rty) && !lv->by_ref) refScopes.back().push_back(lv);
            return;
        }
        if (auto es = dynamic_cast<ExprStmt*>(s)) {
            Value* v = eval(es->expr.get());
            TypePtr ty = es->expr->ty;
            if (valueOwned(es->expr.get())) {
                if (isRefKind(ty)) {
                    emitReleaseRef(v, ty);
                } else if (ty && ty->kind == TypeKind::Array && needsCleanup(ty)) {
                    auto* tmp = entryAlloca(lt(ty), "expr.tmp", alignOf(ty));
                    b.CreateStore(v, tmp);
                    emitReleaseValue(tmp, ty);
                } else if (ty && needsCleanup(ty) &&
                           (ty->kind == TypeKind::Struct || ty->kind == TypeKind::Slice)) {
                    auto* tmp = entryAlloca(lt(ty), "expr.tmp", alignOf(ty));
                    b.CreateStore(v, tmp);
                    emitReleaseValue(tmp, ty);
                }
            }
            return;
        }
        if (auto as = dynamic_cast<AssignStmt*>(s)) {
            if (as->set_func) {
                auto* ix = dynamic_cast<Index*>(as->target.get());
                emitUserCall(as->set_func,
                             {ix->base.get(), ix->indices[0].get(), as->value.get()});
                return;
            }
            Value* addr = addrOf(as->target.get());
            const bool una = unalignedExpr(as->target.get());
            if (as->op == "=") {
                Value* nv = coerce(as->value.get(), as->target->ty);
                bool weakTarget = false;
                if (auto m = dynamic_cast<Member*>(as->target.get())) {
                    TypePtr ot = m->obj->ty;
                    if (ot && ot->kind == TypeKind::Class && m->field_index >= 0 &&
                        m->field_index < static_cast<int>(ot->fields.size())) {
                        weakTarget = ot->fields[m->field_index].weak;
                    }
                } else if (auto n = dynamic_cast<Name*>(as->target.get())) {
                    if (n->kind == "field" && n->implicit_this && curClassTy &&
                        n->field_index >= 0 &&
                        n->field_index < static_cast<int>(curClassTy->fields.size())) {
                        weakTarget = curClassTy->fields[n->field_index].weak;
                    }
                }
                if (weakTarget) {
                    emitWeakStore(addr, nv);
                    return;
                }
                if (isRefKind(as->target->ty)) {
                    Value* old = loadAt(lt(as->target->ty), addr, una);
                    if (!ownsReference(as->value.get())) emitRetainRef(nv, as->target->ty);
                    emitReleaseRef(old, as->target->ty);
                    storeAt(nv, addr, una);
                    return;
                }
                TypePtr tty = as->target->ty;
                if (!weakTarget && tty && needsCleanup(tty) &&
                    (tty->kind == TypeKind::Array || tty->kind == TypeKind::Struct ||
                     tty->kind == TypeKind::Slice)) {
                    auto* tmp = entryAlloca(lt(tty), "val.tmp", alignOf(tty));
                    b.CreateStore(nv, tmp);
                    if (!convOwns(as->value.get(), tty)) emitRetainValue(tmp, tty);
                    emitReleaseValue(addr, tty, una);
                    storeAt(b.CreateLoad(lt(tty), tmp), addr, una);
                    return;
                }
                storeAt(nv, addr, una);
                return;
            }
            Value* v = eval(as->value.get());
            std::string op = as->op.substr(0, as->op.size() - 1);
            Value* old = loadAt(lt(as->target->ty), addr, una);
            TypePtr rt = as->target->ty;
            if (rt->kind == TypeKind::Ptr) {
                Value* n = convertTo(v, as->value->ty, Type::make_prim("isize"));
                Value* np = gep(lt(rt->elem), old, op == "+" ? n : b.CreateNeg(n));
                storeAt(np, addr, una);
                return;
            }
            Value* rv = convertTo(v, as->value->ty, rt);
            bool fl = rt->kind == TypeKind::Prim && rt->is_float();
            bool sg = rt->kind == TypeKind::Prim && rt->is_signed();
            Value* res = nullptr;
            if (op == "+") res = fl ? b.CreateFAdd(old, rv) : b.CreateAdd(old, rv);
            else if (op == "-") res = fl ? b.CreateFSub(old, rv) : b.CreateSub(old, rv);
            else if (op == "*") res = fl ? b.CreateFMul(old, rv) : b.CreateMul(old, rv);
            else if (op == "/") res = fl ? b.CreateFDiv(old, rv) : (sg ? b.CreateSDiv(old, rv) : b.CreateUDiv(old, rv));
            else if (op == "%") res = fl ? b.CreateFRem(old, rv) : (sg ? b.CreateSRem(old, rv) : b.CreateURem(old, rv));
            else if (op == "&") res = b.CreateAnd(old, rv);
            else if (op == "|") res = b.CreateOr(old, rv);
            else if (op == "^") res = b.CreateXor(old, rv);
            else if (op == "<<") res = b.CreateShl(old, rv);
            else if (op == ">>") res = b.CreateAShr(old, rv);
            storeAt(res, addr, una);
            return;
        }
        if (auto is = dynamic_cast<IfStmt*>(s)) {
            Value* cond = eval(is->cond.get());
            BasicBlock* thenBB = BasicBlock::Create(ctx, "if.then", curFn);
            BasicBlock* elseBB = is->els ? BasicBlock::Create(ctx, "if.else", curFn)
                                         : BasicBlock::Create(ctx, "if.end", curFn);
            BasicBlock* endBB = is->els ? BasicBlock::Create(ctx, "if.end", curFn) : elseBB;
            b.CreateCondBr(cond, thenBB, elseBB);
            b.SetInsertPoint(thenBB);
            block(is->then.get());
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
            if (is->els) {
                b.SetInsertPoint(elseBB);
                stmt(is->els.get());
                if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
            }
            b.SetInsertPoint(endBB);
            return;
        }
        if (auto ws = dynamic_cast<WhileStmt*>(s)) {
            BasicBlock* condBB = BasicBlock::Create(ctx, "while.cond", curFn);
            BasicBlock* bodyBB2 = BasicBlock::Create(ctx, "while.body", curFn);
            BasicBlock* endBB = BasicBlock::Create(ctx, "while.end", curFn);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            size_t cmark = stmtTemps.size();
            Value* cond = eval(ws->cond.get());
            destroyTempsFrom(cmark);  // 条件里的 struct 右值每轮释放
            b.CreateCondBr(cond, bodyBB2, endBB);
            b.SetInsertPoint(bodyBB2);
            loops.push_back({condBB, endBB});
            loopScopeDepth.push_back(refScopes.size());
            block(ws->body.get());
            loopScopeDepth.pop_back();
            loops.pop_back();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            return;
        }
        if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            emitFor(fs);
            return;
        }
        if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            emitSwitch(sw);
            return;
        }
        if (auto d = dynamic_cast<DelStmt*>(s)) {
            if (d->value->ty && d->value->ty->kind == TypeKind::Null) return;  // del null
            Value* addr = addrOf(d->value.get());
            TypePtr ty = d->value->ty;
            Value* p = b.CreateLoad(lt(ty), addr);
            b.CreateStore(Constant::getNullValue(lt(ty)), addr);
            if (isRefKind(ty)) emitReleaseRef(p, ty);
            else emitRelease(p);
            return;
        }
        if (dynamic_cast<BreakStmt*>(s)) {
            if (loops.empty()) fail("LLVM 后端：break 不在循环中");
            destroyTempsFrom(0);
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().second);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (dynamic_cast<ContinueStmt*>(s)) {
            if (loops.empty()) fail("LLVM 后端：continue 不在循环中");
            destroyTempsFrom(0);
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().first);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            if (!rs->value) {
                destroyTempsFrom(0);
                releaseAllScopes();
                b.CreateRetVoid();
            } else {
                Value* rv = prepareReturn(rs->value.get());
                destroyTempsFrom(0);
                releaseAllScopes();
                b.CreateRet(rv);
            }
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        fail("LLVM 后端：未知语句");
    }

    TypePtr curRetTy;

    void emitFor(ForInStmt* s) {
        BasicBlock* condBB = BasicBlock::Create(ctx, "for.cond", curFn);
        BasicBlock* bodyBB2 = BasicBlock::Create(ctx, "for.body", curFn);
        BasicBlock* incBB = BasicBlock::Create(ctx, "for.inc", curFn);
        BasicBlock* endBB = BasicBlock::Create(ctx, "for.end", curFn);

        AllocaInst* varSlot = slotFor(s->lvar.get());

        if (s->loop_kind == "range") {
            auto* re = dynamic_cast<RangeExpr*>(s->seq.get());
            TypePtr vt = s->var_ty;
            Value* lo = convertTo(eval(re->lo.get()), re->lo->ty, vt);
            Value* hi = convertTo(eval(re->hi.get()), re->hi->ty, vt);
            b.CreateStore(lo, varSlot);
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            Value* cur = b.CreateLoad(lt(vt), varSlot);
            b.CreateCondBr(b.CreateICmpSLT(cur, hi), bodyBB2, endBB);
            b.SetInsertPoint(bodyBB2);
            loops.push_back({incBB, endBB});
            loopScopeDepth.push_back(refScopes.size());
            block(s->body.get());
            loopScopeDepth.pop_back();
            loops.pop_back();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
            b.SetInsertPoint(incBB);
            Value* c2 = b.CreateLoad(lt(vt), varSlot);
            b.CreateStore(b.CreateAdd(c2, ConstantInt::get(lt(vt), 1)), varSlot);
            b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            return;
        }

        TypePtr sty = s->seq_ty;
        Value* seqAddr = nullptr;
        if (auto n = dynamic_cast<Name*>(s->seq.get())) {
            (void)n;
        }
        // 简单左值直接取地址；否则先物化（只求值一次）
        bool simple = false;
        {
            Expr* e = s->seq.get();
            while (auto m = dynamic_cast<Member*>(e)) e = m->obj.get();
            simple = dynamic_cast<Name*>(e) != nullptr;
        }
        bool seqUnaligned = false;
        if (simple) {
            seqAddr = addrOf(s->seq.get());
            seqUnaligned = unalignedExpr(s->seq.get());
        } else {
            Value* v = eval(s->seq.get());
            seqAddr = entryAlloca(lt(sty), "seq.tmp", alignOf(sty));
            b.CreateStore(v, seqAddr);
            // owned 的动态数组/数组临时值：循环结束后释放
            if (needsCleanup(sty) && valueOwned(s->seq.get()))
                stmtTemps.emplace_back(seqAddr, sty);
        }

        // 长度与元素地址
        Value* len = nullptr;
        Value* elemPtr = nullptr;
        if (sty->kind == TypeKind::Array) {
            len = ConstantInt::get(i64(), sty->array_size);
            elemPtr = gep(cast<LStruct>(lt(sty)), seqAddr,
                                  {ConstantInt::get(i64(), 0), ConstantInt::get(i32(), 0),
                                   ConstantInt::get(i64(), 0)});
        } else if (sty->kind == TypeKind::Slice) {
            Value* h = b.CreateLoad(lt(sty), seqAddr);
            len = emitSliceTotal(h, sty->rank);
            elemPtr = sliceDataPtr(h, sty->rank);
        } else {
            Value* sv = b.CreateLoad(lt(sty), seqAddr);
            Value* data = b.CreateExtractValue(sv, {0});
            len = b.CreateExtractValue(sv, {1});
            elemPtr = data;  // string：byte 视图
        }

        auto* idx = entryAlloca(i64(), "for.idx");
        b.CreateStore(ConstantInt::get(i64(), 0), idx);
        b.CreateBr(condBB);
        b.SetInsertPoint(condBB);
        Value* ci = b.CreateLoad(i64(), idx);
        b.CreateCondBr(b.CreateICmpULT(ci, len), bodyBB2, endBB);
        b.SetInsertPoint(bodyBB2);
        Value* ci2 = b.CreateLoad(i64(), idx);
        TypePtr et = s->elem_ty;
        Value* ep = gep(lt(et), elemPtr, ci2);
        if (s->by_ref) {
            b.CreateStore(ep, varSlot);
        } else {
            b.CreateStore(loadAt(lt(et), ep, seqUnaligned), varSlot);
        }
        loops.push_back({incBB, endBB});
        loopScopeDepth.push_back(refScopes.size());
        block(s->body.get());
        loopScopeDepth.pop_back();
        loops.pop_back();
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
        b.SetInsertPoint(incBB);
        Value* ci3 = b.CreateLoad(i64(), idx);
        b.CreateStore(b.CreateAdd(ci3, ConstantInt::get(i64(), 1)), idx);
        b.CreateBr(condBB);
        b.SetInsertPoint(endBB);
    }

    // switch：值分支走 LLVM switch（跳表），区间分支走比较链，else 兜底。
    void emitSwitch(SwitchStmt* s) {
        Value* sv = eval(s->scrut.get());
        LType* sty = sv->getType();
        BasicBlock* endBB = BasicBlock::Create(ctx, "switch.end", curFn);

        std::vector<BasicBlock*> bodyBB;
        bodyBB.reserve(s->arms.size());
        for (size_t i = 0; i < s->arms.size(); ++i) {
            bodyBB.push_back(BasicBlock::Create(ctx, "switch.arm", curFn));
        }

        int elseIdx = -1;
        std::vector<std::pair<ConstantInt*, BasicBlock*>> cases;
        std::vector<std::pair<SwitchPat*, BasicBlock*>> ranges;
        for (size_t i = 0; i < s->arms.size(); ++i) {
            if (s->arms[i]->is_else) elseIdx = static_cast<int>(i);
            for (auto& p : s->arms[i]->pats) {
                if (p.is_range) {
                    ranges.push_back({&p, bodyBB[i]});
                } else {
                    cases.push_back({ConstantInt::get(llvm::cast<IntegerType>(sty),
                                                      static_cast<uint64_t>(p.vlo), true),
                                     bodyBB[i]});
                }
            }
        }

        BasicBlock* fallback = elseIdx >= 0 ? bodyBB[elseIdx] : endBB;
        BasicBlock* defBB = BasicBlock::Create(ctx, "switch.default", curFn);
        if (cases.empty()) {
            b.CreateBr(defBB);
        } else {
            auto* sw = b.CreateSwitch(sv, defBB, static_cast<unsigned>(cases.size()));
            for (auto& entry : cases) sw->addCase(entry.first, entry.second);
        }

        // 默认路径：按源码顺序测试区间分支，最后落到 else 或 switch.end
        b.SetInsertPoint(defBB);
        const bool unsig = s->sty && !s->sty->is_signed();
        for (size_t k = 0; k < ranges.size(); ++k) {
            BasicBlock* next = (k + 1 < ranges.size())
                                   ? BasicBlock::Create(ctx, "switch.next", curFn)
                                   : fallback;
            Value* lo = ConstantInt::get(llvm::cast<IntegerType>(sty),
                                         static_cast<uint64_t>(ranges[k].first->vlo), true);
            Value* hi = ConstantInt::get(llvm::cast<IntegerType>(sty),
                                         static_cast<uint64_t>(ranges[k].first->vhi), true);
            Value* ge = unsig ? b.CreateICmpUGE(sv, lo) : b.CreateICmpSGE(sv, lo);
            Value* lt = unsig ? b.CreateICmpULT(sv, hi) : b.CreateICmpSLT(sv, hi);
            b.CreateCondBr(b.CreateAnd(ge, lt), ranges[k].second, next);
            if (k + 1 < ranges.size()) b.SetInsertPoint(next);
        }
        if (ranges.empty()) b.CreateBr(fallback);

        for (size_t i = 0; i < s->arms.size(); ++i) {
            b.SetInsertPoint(bodyBB[i]);
            block(s->arms[i]->body.get());
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(endBB);
        }
        b.SetInsertPoint(endBB);
    }

    // ------------------------------------------------------------ 接口表
    std::vector<const MethodSig*> islotSigs;
    std::map<std::string, int> islotIndex;

    static std::string sigKey(const MethodSig& m) {
        std::string k = m.name + "(";
        for (auto& p : m.params) k += type_code(p) + ",";
        k += ")";
        return k;
    }

    void buildIfaceSlots() {
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

    const MethodSig* ifaceDefault(const TypePtr& t, const MethodSig& slot, int depth = 0) {
        if (!t || depth > 256) return nullptr;
        for (auto& m : t->methods) {
            if (same_sig(m, slot) && !m.is_abstract) return &m;
        }
        for (auto& i : t->interfaces) {
            if (auto r = ifaceDefault(i, slot, depth + 1)) return r;
        }
        return nullptr;
    }

    // 类/基类接口链上的默认实现
    const MethodSig* findInterfaceImpl(const TypePtr& cls, const MethodSig& slot) {
        if (const MethodSig* m = findImpl(cls, slot)) return m;
        for (TypePtr cur = cls; cur && cur->kind == TypeKind::Class; cur = cur->base) {
            for (auto& i : cur->interfaces) {
                if (const MethodSig* d = ifaceDefault(i, slot)) return d;
            }
        }
        return nullptr;
    }

    std::map<std::string, Function*> thunkCache;

    // 接口方法调用约定：ret(ptr obj, ptr desc, params...)
    Function* ifaceThunk(const TypePtr& cls, const MethodSig& slot, const MethodSig& impl,
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

    GlobalVariable* emitItable(const TypePtr& cls) {
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

    // ------------------------------------------------------------ 虚表
    static bool same_sig(const MethodSig& a, const MethodSig& b) {
        if (a.name != b.name || a.params.size() != b.params.size()) return false;
        for (size_t i = 0; i < a.params.size(); ++i) {
            if (!same_type(a.params[i], b.params[i])) return false;
        }
        return true;
    }

    std::map<const LType*, std::vector<const MethodSig*>> vtabMemo;
    std::map<const Type*, std::vector<const MethodSig*>> vtabMemo2;

    const std::vector<const MethodSig*>& vtableOf(const TypePtr& cls) {
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

    const MethodSig* findImpl(const TypePtr& cls, const MethodSig& sig) {
        TypePtr cur = cls;
        while (cur && cur->kind == TypeKind::Class) {
            for (auto& m : cur->methods) {
                if (same_sig(m, sig)) return m.is_abstract ? nullptr : &m;
            }
            cur = cur->base;
        }
        return nullptr;
    }

    int slotOf(const TypePtr& t, const MethodSig* sig) {
        if (!t || t->kind != TypeKind::Class || !sig) return -1;
        const auto& v = vtableOf(t);
        for (size_t i = 0; i < v.size(); ++i) {
            if (same_sig(*v[i], *sig)) return static_cast<int>(i);
        }
        return -1;
    }

    FunctionCallee methodFn(const MethodSig* sig) {
        std::vector<LType*> ptys{ptrTy()};
        for (auto& t : sig->params) ptys.push_back(lt(t));
        return mod->getOrInsertFunction(
            method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params),
            FunctionType::get(lt(sig->ret), ptys, false));
    }

    GlobalVariable* emitVtable(const TypePtr& cls) {
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

    // ------------------------------------------------------------ class 代码生成
    void beginBody(Function* fn, const TypePtr& ret = nullptr) {
        curFn = fn;
        curRetTy = ret;
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

    void finishBody(Function* fn, const std::string& what) {
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
            throw MuError("LLVM 后端：`" + what + "` 生成的 IR 无效：" + os.str());
        }
        curFn = nullptr;
        allocaBB = nullptr;
        bodyBB = nullptr;
        slots.clear();
        refScopes.clear();
        loopScopeDepth.clear();
        stmtTemps.clear();
        curClassTy = nullptr;
        curThis = nullptr;
        thisSlot = nullptr;
    }

    void bindParams(Function* fn, size_t first,
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

    void emitMethod(ClassDecl* c, MethodDecl* m) {
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
        block(m->body.get());
        finishBody(fn, c->name + "." + m->name);
    }

    void emitCtor(ClassDecl* c, CtorDecl* ctor) {
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
        // 入口先设置本类的 desc；调用 super(...) 之后会恢复
        Value* dp = b.CreateGEP(i8(), curThis, ConstantInt::get(i64(), 8));
        b.CreateStore(descOf(c->ty), dp);
        bindParams(fn, 1, ctor->param_locals);

        // 省略 super(...) 且基类有无参构造器：按 C++ 语义隐式调用
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
                Value* dp2 = b.CreateGEP(i8(), curThis, ConstantInt::get(i64(), 8));
                b.CreateStore(descOf(c->ty), dp2);
            }
        }
        block(ctor->body.get());
        finishBody(fn, c->name + " 构造器");
    }

    void emitReleaseValue(Value* addr, const TypePtr& ty, bool unaligned = false) {
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

    void emitRetainValue(Value* addr, const TypePtr& ty, bool unaligned = false) {
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

    void emitDtor(ClassDecl* c) {
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(dtor_cname(c->ty->unit, c->name),
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(c->is_public ? GlobalValue::ExternalLinkage
                                    : GlobalValue::InternalLinkage);
        curClassTy = c->ty;
        beginBody(fn);
        curThis = fn->getArg(0);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(curThis, thisSlot);
        if (c->dtor && c->dtor->body) block(c->dtor->body.get());

        // 释放本类声明的引用字段（逆序）；weak 字段只注销登记
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
        finishBody(fn, c->name + " 析构器");
    }

    void emitDestroy(ClassDecl* c) {
        std::string nm = "mu_" + c->ty->unit + "_destroy_" + c->name;
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(nm, FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(GlobalValue::InternalLinkage);
        beginBody(fn);
        Value* p = fn->getArg(0);
        // 先把指向本对象的所有 weak 引用置 null
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
        finishBody(fn, c->name + " 销毁函数");
    }

    void emitClassDesc(ClassDecl* c) {
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

    void emitStructMethod(StructDecl* sd, MethodDecl* m) {
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
        block(m->body.get());
        finishBody(fn, sd->name + "." + m->name);
    }

    void emitStructCtor(StructDecl* sd, CtorDecl* ctor) {
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
        block(ctor->body.get());
        finishBody(fn, sd->name + " 构造器");
    }

    void emitStructDtor(StructDecl* sd) {
        if (!sd->dtor) return;
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(sd->dtor->cname,
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(sd->is_public ? GlobalValue::ExternalLinkage
                                    : GlobalValue::InternalLinkage);
        curClassTy = sd->ty;
        beginBody(fn);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(fn->getArg(0), thisSlot);
        block(sd->dtor->body.get());
        // 用户析构体之后，逐字段释放引用（逆序）
        for (size_t k = sd->ty->fields.size(); k > 0; --k) {
            size_t idx = k - 1;
            if (sd->ty->fields[idx].weak) continue;
            emitReleaseValue(fieldAddr(b.CreateLoad(ptrTy(), thisSlot), sd->ty,
                                       static_cast<int>(idx)),
                             sd->ty->fields[idx].type, sd->ty->is_packed);
        }
        finishBody(fn, sd->name + " 析构器");
    }

    void emitInterfaceMethod(InterfaceDecl* itf, MethodDecl* m) {
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
        block(m->body.get());
        finishBody(fn, itf->name + "." + m->name);
    }

    // ------------------------------------------------------------ 函数
    void emitFunction(FuncDecl* f) {
        if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm] emit " << f->name << "\n";
        std::vector<LType*> ptys;
        for (auto& t : f->pty) ptys.push_back(lt(t));
        auto* ft = FunctionType::get(lt(f->rty), ptys, false);
        Function* fn = cast<Function>(mod->getOrInsertFunction(f->cname, ft).getCallee());
        fn->setLinkage((f->is_extern || f->is_public || f->name == "main")
                           ? GlobalValue::ExternalLinkage
                           : GlobalValue::InternalLinkage);
        if (f->is_extern) return;

        curClassTy = nullptr;
        curThis = nullptr;
        thisSlot = nullptr;
        refScopes.clear();
        loopScopeDepth.clear();
        stmtTemps.clear();
        curFn = fn;
        curRetTy = f->rty;
        allocaBB = BasicBlock::Create(ctx, "allocas", fn);
        bodyBB = BasicBlock::Create(ctx, "entry", fn);
        ab.SetInsertPoint(allocaBB);

        // 参数
        size_t i = 0;
        for (auto& a : fn->args()) {
            LocalVar* pl = i < f->param_locals.size() ? f->param_locals[i].get() : nullptr;
            if (pl) {
                auto* slot = entryAlloca(lt(pl->rty), sane(pl->name), alignOf(pl->rty));
                slots[pl] = slot;
                b.SetInsertPoint(bodyBB);
                b.CreateStore(&a, slot);
                a.setName(pl->name);
            }
            ++i;
        }
        b.SetInsertPoint(bodyBB);
        block(f->body.get());
        if (!hasTerm(b.GetInsertBlock())) {
            if (is_void(f->rty)) {
                b.CreateRetVoid();
            } else if (f->cname == "main") {
                b.CreateRet(ConstantInt::get(i32(), 0));
            } else {
                b.CreateUnreachable();
            }
        }
        ab.SetInsertPoint(allocaBB);
        ab.CreateBr(bodyBB);

        std::string err;
        raw_string_ostream os(err);
        if (verifyFunction(*fn, &os)) {
            throw MuError("LLVM 后端：函数 `" + f->name + "` 生成的 IR 无效：" + os.str());
        }
        curFn = nullptr;
        allocaBB = nullptr;
        bodyBB = nullptr;
        slots.clear();
    }

    void emitGlobals() {
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto* g = dynamic_cast<GlobalVar*>(d.get());
                if (!g || g->is_extern) continue;
                LType* ty = lt(g->rty);
                Constant* init = g->init ? constValue(g->rty, g->init.get())
                                         : zeroConst(ty, g->rty);
                auto linkage = g->is_public ? GlobalValue::ExternalLinkage
                                            : GlobalValue::InternalLinkage;
                auto* gv = new GlobalVariable(*mod, ty, g->is_const, linkage, init, g->cname);
                if (g->is_const) gv->setConstant(true);
                uint64_t ga = alignOf(g->rty);
                if (ga > 1) gv->setAlignment(llvm::Align(ga));
            }
        }
    }

    void run() {
        // 目标
        std::string tripleStr = "x86_64-w64-windows-gnu";
        std::string err;
        llvm::Triple triple(tripleStr);
        const Target* target = TargetRegistry::lookupTarget(triple, err);
        if (!target) throw MuError("LLVM 后端：找不到目标 " + tripleStr + "：" + err);
        TargetOptions opt;
        tm.reset(target->createTargetMachine(triple, "generic", "", opt,
                                             std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_),
                                             std::nullopt, CodeGenOptLevel::Default));
        mod = std::make_unique<Module>("mulan", ctx);
        mod->setTargetTriple(triple);
        if (tm) mod->setDataLayout(tm->createDataLayout());
        mod->addModuleFlag(Module::Warning, "mulan.version",
                           llvm::MDString::get(ctx, MULANC_VERSION));

        sliceTy = ptrTy();  // 动态数组句柄（头指针）
        descTy = LStruct::create(ctx, "mu_desc");
        descTy->setBody({i64(), ptrTy(), ptrTy(), ptrTy(), ptrTy()}, false);
        ifaceTy = LStruct::create(ctx, "mu_iface");
        ifaceTy->setBody({ptrTy(), ptrTy()}, false);
        weakNodeTy = LStruct::create(ctx, "mu_weak_node");
        weakNodeTy->setBody({ptrTy(), ptrTy(), ptrTy()}, false);
        new GlobalVariable(*mod, ptrTy(), /*isConstant=*/false, GlobalValue::InternalLinkage,
                           Constant::getNullValue(ptrTy()), "mu_weak_head");
        new GlobalVariable(*mod, i32(), /*isConstant=*/false, GlobalValue::InternalLinkage,
                           ConstantInt::get(i32(), 0), "mu_weak_lock");

        // 先建结构体类型（字段可能引用彼此；指针是 opaque ptr，不会递归）
        for (auto& u : prog.units) {
            for (auto& d : u->decls) {
                if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                    if (sd->ty) structOf(sd->ty);
                }
            }
        }
        // 对象布局与描述符
        buildIfaceSlots();
        // 先占位所有 desc（派生类可能引用基类 desc），再填初始化器
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;
                objStruct(c->ty);
                new GlobalVariable(*mod, descTy, /*isConstant=*/true, GlobalValue::InternalLinkage,
                                   Constant::getNullValue(descTy),
                                   class_desc_name(c->ty->unit, c->name));
            }
        }
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;
                emitClassDesc(c);
            }
        }

        emitGlobals();

        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                    if (f->is_extern) {
                        std::vector<LType*> ptys;
                        for (auto& t : f->pty) ptys.push_back(lt(t));
                        mod->getOrInsertFunction(
                            f->cname, FunctionType::get(lt(f->rty), ptys, false));
                    }
                }
            }
        }
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                if (auto f = dynamic_cast<FuncDecl*>(d.get())) emitFunction(f);
            }
        }
        // class 成员函数、构造器、析构器、销毁函数；接口默认方法
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (c && c->ty) {
                    for (auto& m : c->methods) emitMethod(c, m.get());
                    for (auto& ctor : c->ctors) emitCtor(c, ctor.get());
                    emitDtor(c);
                    emitDestroy(c);
                }
                auto itf = dynamic_cast<InterfaceDecl*>(d.get());
                if (itf && itf->ty) {
                    for (auto& m : itf->methods) emitInterfaceMethod(itf, m.get());
                }
                auto sd = dynamic_cast<StructDecl*>(d.get());
                if (sd && sd->ty) {
                    for (auto& m : sd->methods) emitStructMethod(sd, m.get());
                    for (auto& ctor : sd->ctors) emitStructCtor(sd, ctor.get());
                    emitStructDtor(sd);
                }
            }
        }

        std::string errs;
        raw_string_ostream os(errs);
        if (verifyModule(*mod, &os)) {
            throw MuError("LLVM 后端：生成的模块无效：" + os.str());
        }

        // 标准 -O2 流水线：让 Str 的短方法/运算符与 RC 辅助函数内联。
        {
            LoopAnalysisManager LAM;
            FunctionAnalysisManager FAM;
            CGSCCAnalysisManager CGAM;
            ModuleAnalysisManager MAM;
            PassBuilder PB(tm.get());
            PB.registerModuleAnalyses(MAM);
            PB.registerCGSCCAnalyses(CGAM);
            PB.registerFunctionAnalyses(FAM);
            PB.registerLoopAnalyses(LAM);
            PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
            ModulePassManager MPM =
                PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
            MPM.run(*mod, MAM);
        }
    }
};

}  // namespace

std::string llvm_emit_text(Program& prog) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    Emitter em{prog};
    em.run();
    std::string out;
    raw_string_ostream os(out);
    em.mod->print(os, nullptr);
    os.flush();
    return out;
}

void llvm_emit_object(Program& prog, const std::string& obj_path) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    Emitter em{prog};
    em.run();
    std::error_code ec;
    raw_fd_ostream dest(obj_path, ec, llvm::sys::fs::OF_None);
    if (ec) throw MuError("无法写入目标文件 " + obj_path + "：" + ec.message());
    llvm::legacy::PassManager pm;
    if (em.tm->addPassesToEmitFile(pm, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        throw MuError("LLVM 后端：目标不支持生成目标文件");
    }
    pm.run(*em.mod);
    dest.flush();
}













