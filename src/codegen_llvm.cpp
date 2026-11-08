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

    LStruct* strTy = nullptr;
    LStruct* sliceTy = nullptr;
    LStruct* descTy = nullptr;
    LStruct* ifaceTy = nullptr;  // {ptr obj, ptr desc}
    std::map<std::string, LStruct*> named;
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
    std::vector<std::vector<const LocalVar*>> refScopes;  // 需要释放的引用局部变量
    std::vector<size_t> loopScopeDepth;

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
        if (n == "byte" || n == "ubyte" || n == "char") return i8();
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
        std::vector<LType*> fs;
        for (auto& f : t->fields) fs.push_back(lt(f.type));
        st->setBody(fs, false);
        return st;
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

    // 对象布局：{ i64 rc, ptr desc, fields... }（fields 已在 sema 扁平化，基类字段在前）
    LStruct* objStruct(const TypePtr& cls) {
        std::string key = "o:" + cls->unit + ":" + cls->name;
        auto it = named.find(key);
        if (it != named.end()) return it->second;
        auto* st = LStruct::create(ctx, "mu_obj_" + sane(cls->unit) + "_" + sane(cls->name));
        named[key] = st;
        std::vector<LType*> fs{i64(), ptrTy()};
        for (auto& f : cls->fields) fs.push_back(lt(f.type));
        st->setBody(fs, false);
        return st;
    }

    static constexpr unsigned kObjRc = 0;
    static constexpr unsigned kObjDesc = 1;
    static constexpr unsigned kObjField0 = 2;

    uint64_t objectSize(const TypePtr& cls) {
        return mod->getDataLayout().getTypeAllocSize(objStruct(cls));
    }

    uint64_t objectAlign(const TypePtr& cls) {
        return mod->getDataLayout().getABITypeAlign(objStruct(cls)).value();
    }

    Value* fieldAddr(Value* objPtr, const TypePtr& cls, int field_index) {
        return b.CreateStructGEP(objStruct(cls), objPtr,
                                 static_cast<unsigned>(kObjField0 + field_index));
    }

    Value* descOf(const TypePtr& cls) {
        return mod->getOrInsertGlobal(class_desc_name(cls->unit, cls->name), descTy);
    }

    LType* lt(const TypePtr& t) {
        if (!t) return i32();
        switch (t->kind) {
            case TypeKind::Void: return LType::getVoidTy(ctx);
            case TypeKind::Prim: return primTy(t->name);
            case TypeKind::String: return strTy;
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
    uint64_t alignOf(const TypePtr& t) {
        if (!t) return 1;
        switch (t->kind) {
            case TypeKind::Void: return 1;
            case TypeKind::Prim: {
                const std::string& n = t->name;
                if (n == "bool" || n == "byte" || n == "ubyte" || n == "char") return 1;
                if (n == "short" || n == "ushort") return 2;
                if (n == "int" || n == "uint" || n == "float") return 4;
                return 8;
            }
            case TypeKind::Ptr:
            case TypeKind::String:
            case TypeKind::Slice: return 8;
            case TypeKind::Enum: return 4;
            case TypeKind::Array: return alignOf(t->elem);
            case TypeKind::Struct: {
                uint64_t a = 1;
                for (auto& f : t->fields) a = std::max(a, alignOf(f.type));
                return a;
            }
            default: return 8;
        }
    }

    uint64_t sizeOf(const TypePtr& t) {
        if (!t) return 0;
        switch (t->kind) {
            case TypeKind::Void: return 0;
            case TypeKind::Prim: {
                const std::string& n = t->name;
                if (n == "bool" || n == "byte" || n == "ubyte" || n == "char") return 1;
                if (n == "short" || n == "ushort") return 2;
                if (n == "int" || n == "uint" || n == "float") return 4;
                return 8;
            }
            case TypeKind::Ptr:
            case TypeKind::String:
            case TypeKind::Slice: return 8 * 2;
            case TypeKind::Enum: return 4;
            case TypeKind::Array: return sizeOf(t->elem) * t->array_size;
            case TypeKind::Struct: {
                uint64_t off = 0, a = 1;
                for (auto& f : t->fields) {
                    uint64_t fa = alignOf(f.type);
                    a = std::max(a, fa);
                    off = (off + fa - 1) / fa * fa;
                    off += sizeOf(f.type);
                }
                return (off + a - 1) / a * a;
            }
            default: return 8;
        }
    }

    bool zeroable(const TypePtr& t) {
        return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Array ||
                     t->kind == TypeKind::Slice || t->kind == TypeKind::String);
    }

    // ------------------------------------------------------------ 函数/全局
    FunctionCallee libFn(const char* name, LType* ret, std::vector<LType*> params) {
        return mod->getOrInsertFunction(name, FunctionType::get(ret, params, false));
    }

    Constant* zeroConst(LType* ty, const TypePtr& mt) {
        if (mt && zeroable(mt)) return ConstantAggregateZero::get(ty);
        return Constant::getNullValue(ty);
    }

    Value* stringConst(const std::string& bytes) {
        auto it = strCache.find(bytes);
        if (it != strCache.end()) return it->second;
        Constant* arr = ConstantDataArray::getString(ctx, bytes, true);
        auto* gv = new GlobalVariable(*mod, arr->getType(), true, GlobalValue::PrivateLinkage,
                                      arr, ".str");
        Constant* c = ConstantStruct::get(strTy, {gv, ConstantInt::get(i64(), bytes.size())});
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
    static bool needsCleanup(const TypePtr& t) {
        if (!t) return false;
        if (t->kind == TypeKind::Class || t->kind == TypeKind::Interface) return true;
        if (t->kind == TypeKind::Array) return needsCleanup(t->elem);
        return false;
    }

    void releaseLocal(const LocalVar* lv) {
        if (!lv || !needsCleanup(lv->rty) || lv->by_ref) return;
        auto it = slots.find(lv);
        if (it == slots.end()) return;
        if (isRefKind(lv->rty)) {
            Value* v = b.CreateLoad(lt(lv->rty), it->second);
            emitReleaseRef(v, lv->rty);
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

    void releaseAllScopes() {
        for (size_t i = refScopes.size(); i > 0; --i) {
            auto& scope = refScopes[i - 1];
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) releaseLocal(*it);
        }
    }

    // 表达式是否已经持有一份可转移的强引用（new / 返回 class 引用的调用）
    bool ownsReference(Expr* e) {
        if (!e || !isRefKind(e->ty)) return false;
        if (dynamic_cast<NewExpr*>(e)) return true;
        if (dynamic_cast<Call*>(e)) return true;
        if (dynamic_cast<Cast*>(e)) return ownsReference(static_cast<Cast*>(e)->expr.get());
        return false;
    }

    // 返回 class 引用时把借用变成一份新的强引用
    Value* prepareReturn(Expr* e) {
        Value* v = coerce(e, curRetTy);
        if (isRefKind(curRetTy) && !ownsReference(e)) emitRetainRef(v, curRetTy);
        return v;
    }

    // ------------------------------------------------------------ 分配
    AllocaInst* entryAlloca(LType* ty, const std::string& name) {
        IRBuilder<>::InsertPointGuard g(ab);
        if (allocaBB) ab.SetInsertPoint(allocaBB);
        return ab.CreateAlloca(ty, nullptr, name);
    }

    AllocaInst* slotFor(const LocalVar* lv) {
        auto it = slots.find(lv);
        if (it != slots.end()) return it->second;
        LType* ty = lt(lv->rty);
        if (lv->by_ref) ty = ptrTy();
        auto* a = entryAlloca(ty, sane(lv->name));
        slots[lv] = a;
        return a;
    }

    // ------------------------------------------------------------ 常量
    Constant* constValue(const TypePtr& ty, Init* init) {
        LType* ct = lt(ty);
        if (!init) return zeroConst(ct, ty);
        if (is_zero_literal(init)) return zeroConst(ct, ty);
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
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
                return ConstantStruct::get(cast<LStruct>(ct), vals);
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
                auto* carr = ConstantArray::get(arrTy, vals);
                auto* gv = new GlobalVariable(*mod, arrTy, true, GlobalValue::PrivateLinkage, carr,
                                              ".slice");
                return ConstantStruct::get(sliceTy, {gv, ConstantInt::get(i64(), vals.size())});
            }
            fail("LLVM 后端：不支持的全局聚合初始化", ia->line, ia->col);
        }
        auto ie = dynamic_cast<InitExpr*>(init);
        if (!ie) fail("LLVM 后端：未知初始化器");
        Expr* e = ie->expr.get();
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (ty->kind == TypeKind::String) {
                fail("LLVM 后端：string 不能用整数初始化");
            }
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
            if (ty->kind == TypeKind::String) return cast<Constant>(stringConst(sl->value));
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

        if (from->kind == TypeKind::String && to->kind == TypeKind::Ptr)
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
            if (n->ident == "this") return thisSlot;
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
            return b.CreateStructGEP(st, base, static_cast<unsigned>(m->field_index));
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            TypePtr bt = ix->base->ty;
            Value* idx = convertTo(eval(ix->index.get()), ix->index->ty, Type::make_prim("usize"),
                                   ix->line, ix->col);
            if (bt->kind == TypeKind::Array) {
                Value* base = addrOf(ix->base.get());
                auto* st = cast<LStruct>(lt(bt));
                return gep(st, base, {ConstantInt::get(i64(), 0),
                                              ConstantInt::get(i32(), 0), idx});
            }
            if (bt->kind == TypeKind::Slice) {
                Value* sv = eval(ix->base.get());
                return gep(lt(bt->elem), b.CreateExtractValue(sv, {0}), idx);
            }
            if (bt->kind == TypeKind::Ptr) {
                return gep(lt(bt->elem), eval(ix->base.get()), idx);
            }
            if (bt->kind == TypeKind::String) {
                Value* sv = eval(ix->base.get());
                return gep(i8(), b.CreateExtractValue(sv, {0}), idx);
            }
            fail("LLVM 后端：不可下标访问", ix->line, ix->col);
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op == "*") return eval(u->operand.get());
        }
        // 其它表达式：先求值到临时量
        Value* v = eval(e);
        auto* tmp = entryAlloca(v->getType(), "tmp");
        b.CreateStore(v, tmp);
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
            e->ty->kind == TypeKind::Array && same_type(e->ty->elem, to->elem)) {
            Value* base = addrOf(e);
            auto* st = cast<LStruct>(lt(e->ty));
            Value* inner = b.CreateGEP(st, base, {ConstantInt::get(i64(), 0),
                                                  ConstantInt::get(i32(), 0)});
            Value* v = UndefValue::get(sliceTy);
            v = b.CreateInsertValue(v, inner, {0});
            v = b.CreateInsertValue(v, ConstantInt::get(i64(), e->ty->array_size), {1});
            return v;
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
        if ((op == "==" || op == "!=") && lt_->kind == TypeKind::String &&
            rt_->kind == TypeKind::String) {
            Value* l = eval(e->left.get());
            Value* r = eval(e->right.get());
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

        // 指针算术
        if ((op == "+" || op == "-") && lt_->kind == TypeKind::Ptr && is_integer(rt_)) {
            Value* p = eval(e->left.get());
            Value* n = convertTo(eval(e->right.get()), rt_, Type::make_prim("isize"));
            return gep(lt(lt_->elem), p, n);
        }
        if (op == "+" && is_integer(lt_) && rt_->kind == TypeKind::Ptr) {
            Value* n = convertTo(eval(e->left.get()), lt_, Type::make_prim("isize"));
            Value* p = eval(e->right.get());
            return gep(lt(rt_->elem), p, n);
        }
        if (op == "-" && lt_->kind == TypeKind::Ptr && rt_->kind == TypeKind::Ptr) {
            Value* a = b.CreatePtrToInt(eval(e->left.get()), i64());
            Value* c = b.CreatePtrToInt(eval(e->right.get()), i64());
            Value* d = b.CreateSub(a, c);
            uint64_t sz = sizeOf(lt_->elem);
            return b.CreateSDiv(d, ConstantInt::get(i64(), sz ? sz : 1));
        }

        Value* l = eval(e->left.get());
        Value* r = eval(e->right.get());

        if (op == "==" || op == "!=") {
            bool lref = isRefKind(lt_) || lt_->kind == TypeKind::Null;
            bool rref = isRefKind(rt_) || rt_->kind == TypeKind::Null;
            if (lref && rref && (isRefKind(lt_) || isRefKind(rt_))) {
                Value* l = eval(e->left.get());
                Value* r = eval(e->right.get());
                if (lt_->kind == TypeKind::Null) l = Constant::getNullValue(ptrTy());
                else if (isRefKind(lt_)) l = refObj(l, lt_);
                if (rt_->kind == TypeKind::Null) r = Constant::getNullValue(ptrTy());
                else if (isRefKind(rt_)) r = refObj(r, rt_);
                return compareOp(op, l, r, false, false);
            }
        }
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (lt_->kind == TypeKind::Ptr || rt_->kind == TypeKind::Ptr) {
                Value* lv = eval(e->left.get());
                Value* rv = eval(e->right.get());
                if (lt_->kind == TypeKind::Null) lv = Constant::getNullValue(ptrTy());
                if (rt_->kind == TypeKind::Null) rv = Constant::getNullValue(ptrTy());
                return compareOp(op, lv, rv, false, false);
            }
            if (same_type(lt_, rt_) && lt_->kind == TypeKind::Prim && lt_->is_bool()) {
                return compareOp(op, l, r, false, false);
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

    // 调用自由函数/运算符/方法（静态派发）
    Value* emitUserCall(FuncDecl* f, const std::vector<Expr*>& args) {
        std::vector<LType*> ptys;
        for (auto& t : f->pty) ptys.push_back(lt(t));
        FunctionCallee callee =
            mod->getOrInsertFunction(f->cname, FunctionType::get(lt(f->rty), ptys, false));
        std::vector<Value*> vals;
        for (size_t i = 0; i < args.size() && i < f->pty.size(); ++i)
            vals.push_back(coerce(args[i], f->pty[i]));
        return b.CreateCall(callee, vals);
    }

    Value* evalNew(NewExpr* ne) {
        TypePtr cls = ne->class_ty;
        uint64_t sz = objectSize(cls);
        Value* p = b.CreateCall(libFn("malloc", ptrTy(), {i64()}),
                                {ConstantInt::get(i64(), sz)});
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
            std::vector<Value*> vals{p};
            for (size_t i = 0; i < ne->args.size() && i < ne->ctor_pty.size(); ++i)
                vals.push_back(coerce(ne->args[i].get(), ne->ctor_pty[i]));
            b.CreateCall(callee, vals);
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
                v = b.CreateInsertValue(v, av, {static_cast<unsigned>(fi)});
            }
            return v;
        }
        if (c->callee_kind == "method") {
            const MethodSig* sig = c->method;
            if (!sig || !sig->owner) fail("LLVM 后端：方法调用缺少签名", c->line, c->col);
            Value* recv = nullptr;
            TypePtr recvTy = nullptr;
            if (auto m = dynamic_cast<Member*>(c->callee.get())) {
                recv = eval(m->obj.get());
                recvTy = m->obj->ty;
            } else {
                recvTy = curClassTy;
                recv = b.CreateLoad(lt(recvTy), thisSlot);
            }
            std::vector<LType*> ptys{ptrTy()};
            for (auto& t : sig->params) ptys.push_back(lt(t));
            auto* fnTy = FunctionType::get(lt(sig->ret), ptys, false);
            std::vector<Value*> vals{recv};
            for (size_t i = 0; i < c->args.size() && i < sig->params.size(); ++i)
                vals.push_back(coerce(c->args[i].get(), sig->params[i]));

            if (recvTy && recvTy->kind == TypeKind::Interface) {
                auto it = islotIndex.find(sigKey(*sig));
                if (it == islotIndex.end()) {
                    fail("LLVM 后端：接口方法未进入 itable", c->line, c->col);
                }
                int idx = it->second;
                Value* obj = b.CreateExtractValue(recv, {0});
                Value* dsc = b.CreateExtractValue(recv, {1});
                Value* itp = b.CreateGEP(i8(), dsc, ConstantInt::get(i64(), 24));
                Value* itab = b.CreateLoad(ptrTy(), itp);
                Value* fnp = b.CreateGEP(ptrTy(), itab, ConstantInt::get(i64(), idx));
                Value* fn = b.CreateLoad(ptrTy(), fnp);
                std::vector<LType*> ip{ptrTy(), ptrTy()};
                for (auto& t : sig->params) ip.push_back(lt(t));
                auto* ifnTy = FunctionType::get(lt(sig->ret), ip, false);
                std::vector<Value*> iargs{obj, dsc};
                for (size_t i = 1; i < vals.size(); ++i) iargs.push_back(vals[i]);
                return b.CreateCall(ifnTy, fn, iargs);
            }

            int idx = slotOf(recvTy, sig);
            if (idx >= 0) {
                Value* dp = b.CreateGEP(i8(), recv, ConstantInt::get(i64(), 8));
                Value* desc = b.CreateLoad(ptrTy(), dp);
                Value* vtp = b.CreateGEP(i8(), desc, ConstantInt::get(i64(), 16));
                Value* vt = b.CreateLoad(ptrTy(), vtp);
                Value* fnp = b.CreateGEP(ptrTy(), vt, ConstantInt::get(i64(), idx));
                Value* fn = b.CreateLoad(ptrTy(), fnp);
                return b.CreateCall(fnTy, fn, vals);
            }
            // 未进虚表：静态调用声明处实现
            FunctionCallee callee = methodFn(sig);
            return b.CreateCall(callee, vals);
        }
        if (c->callee_kind == "super") {
            if (c->method) {
                const MethodSig* sig = c->method;
                std::vector<LType*> ptys{ptrTy()};
                for (auto& t : sig->params) ptys.push_back(lt(t));
                FunctionCallee callee = mod->getOrInsertFunction(
                    method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params),
                    FunctionType::get(lt(sig->ret), ptys, false));
                std::vector<Value*> vals{b.CreateLoad(ptrTy(), thisSlot)};
                for (size_t i = 0; i < c->args.size() && i < sig->params.size(); ++i)
                    vals.push_back(coerce(c->args[i].get(), sig->params[i]));
                return b.CreateCall(callee, vals);
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
                std::vector<Value*> vals{b.CreateLoad(ptrTy(), thisSlot)};
                for (size_t i = 0; i < c->args.size(); ++i)
                    vals.push_back(coerce(c->args[i].get(), (pty ? *pty : empty)[i]));
                b.CreateCall(callee, vals);
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
        std::vector<Value*> args;
        for (size_t i = 0; i < c->args.size() && i < f->pty.size(); ++i)
            args.push_back(coerce(c->args[i].get(), f->pty[i]));
        return b.CreateCall(callee, args);
    }

    // 块是否已有终结指令（getTerminator 在空块上会断言）
    static bool hasTerm(BasicBlock* bb) {
        return bb && !bb->empty() && bb->back().isTerminator();
    }

    Value* eval(Expr* e) {
        if (auto v = dynamic_cast<IntLit*>(e)) {
            if (isRefKind(e->ty) || (e->ty && e->ty->kind == TypeKind::Null))
                return Constant::getNullValue(ptrTy());
            LType* ty = lt(e->ty);
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
            return b.CreateLoad(lt(e->ty), addrOf(e));
        }
        if (dynamic_cast<Member*>(e)) {
            return b.CreateLoad(lt(e->ty), addrOf(e));
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            if (ix->get_func) return emitUserCall(ix->get_func, {ix->base.get(), ix->index.get()});
            return b.CreateLoad(lt(e->ty), addrOf(e));
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op_func) return emitUserCall(u->op_func, {u->operand.get()});
            if (u->op == "&") return addrOf(u->operand.get());
            if (u->op == "*") return b.CreateLoad(lt(e->ty), eval(u->operand.get()));
            Value* v = eval(u->operand.get());
            if (u->op == "-") {
                if (e->ty->kind == TypeKind::Prim && e->ty->is_float())
                    return b.CreateFNeg(v);
                return b.CreateSub(ConstantInt::get(lt(e->ty), 0), v);
            }
            if (u->op == "+") return v;
            if (u->op == "!")
                return b.CreateXor(v, ConstantInt::get(lt(e->ty), static_cast<uint64_t>(-1), true));
        }
        if (auto v = dynamic_cast<NotExpr*>(e)) {
            Value* x = eval(v->operand.get());
            return b.CreateXor(x, ConstantInt::get(i1(), 1));
        }
        if (auto v = dynamic_cast<Binary*>(e)) return evalBinary(v);
        if (auto v = dynamic_cast<Cast*>(e))
            return convertTo(eval(v->expr.get()), v->expr->ty, e->ty, v->line, v->col);
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
            if (st->kind == TypeKind::Array) return ConstantInt::get(i64(), st->array_size);
            Value* s = eval(v->seq.get());
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
                    }
                }
                auto it = prog.infos[curUnit].structs.find(spec->name);
                if (it != prog.infos[curUnit].structs.end()) return it->second->ty;
                fail("LLVM 后端：sizeof/alignof 未知类型 " + spec->name);
            }
            case TypeSpec::K::Ptr:
                return Type::make_ptr(specType(spec->elem.get()), spec->readonly);
            case TypeSpec::K::Slice:
                return Type::make_slice(specType(spec->elem.get()), spec->readonly);
            case TypeSpec::K::Array:
                return Type::make_array(specType(spec->elem.get()), spec->size);
        }
        fail("LLVM 后端：未知类型 spec");
    }
    void curSpecUnitFix() {}

    // ------------------------------------------------------------ 语句
    void storeValue(Value* v, Value* addr, const TypePtr& ty) {
        if (!v) return;
        if (ty->kind == TypeKind::Void) return;
        b.CreateStore(v, addr);
    }

    void zeroMem(Value* addr, const TypePtr& ty) {
        uint64_t n = sizeOf(ty);
        if (n == 0) return;
        b.CreateMemSet(addr, ConstantInt::get(i8(), 0), ConstantInt::get(i64(), n),
                       MaybeAlign(alignOf(ty)));
    }

    void copyMem(Value* dst, Value* src, uint64_t n) {
        if (n == 0) return;
        b.CreateMemCpy(dst, MaybeAlign(8), src, MaybeAlign(8), ConstantInt::get(i64(), n));
    }

    void initInto(Value* addr, const TypePtr& ty, Init* init) {
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
            if (ty->kind == TypeKind::Array && sizeOf(ty->elem) == 1 &&
                dynamic_cast<StringLit*>(e)) {
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       char-array from string\n";
                Value* s = stringConst(static_cast<StringLit*>(e)->value);
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       A\n";
                Value* sp = b.CreateExtractValue(s, {0});
                Value* sl = b.CreateExtractValue(s, {1});
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       B\n";
                zeroMem(addr, ty);
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       C\n";
                b.CreateMemCpy(addr, MaybeAlign(alignOf(ty)), sp, MaybeAlign(1), sl);
                if (std::getenv("MULANC_LLVM_TRACE")) std::cerr << "[llvm]       D\n";
                return;
            }
            Value* v = coerce(e, ty);
            if (isRefKind(ty) && !ownsReference(e)) emitRetainRef(v, ty);
            storeValue(v, addr, ty);
            return;
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            // 与 C 一致：聚合初始化未覆盖的部分清零
            zeroMem(addr, ty);
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
                    Value* fp = b.CreateStructGEP(st, addr, static_cast<unsigned>(idx));
                    initInto(fp, ty->fields[idx].type, sub);
                }
                return;
            }
            if (ty->kind == TypeKind::Array) {
                auto* st = cast<LStruct>(lt(ty));
                Value* inner = gep(st, addr, {ConstantInt::get(i64(), 0),
                                                       ConstantInt::get(i32(), 0)});
                for (size_t i = 0; i < ia->items.size() && i < ty->array_size; ++i) {
                    Value* p = gep(lt(ty->elem), inner, ConstantInt::get(i64(), i));
                    initInto(p, ty->elem, ia->items[i].second.get());
                }
                return;
            }
            if (ty->kind == TypeKind::Slice) {
                std::vector<Init*> elems;
                for (auto& it : ia->items) elems.push_back(it.second.get());
                auto* arrTy = LArray::get(lt(ty->elem), elems.size());
                auto* tmp = entryAlloca(arrTy, "slice.tmp");
                for (size_t i = 0; i < elems.size(); ++i) {
                    Value* p = gep(arrTy, tmp, {ConstantInt::get(i64(), 0),
                                                        ConstantInt::get(i64(), i)});
                    initInto(p, ty->elem, elems[i]);
                }
                Value* v = UndefValue::get(sliceTy);
                v = b.CreateInsertValue(v, tmp, {0});
                v = b.CreateInsertValue(v, ConstantInt::get(i64(), elems.size()), {1});
                b.CreateStore(v, addr);
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
            } else if (lv->rty && lv->rty->kind == TypeKind::Array && needsCleanup(lv->rty)) {
                zeroMem(slot, lv->rty);
            }
            if (needsCleanup(lv->rty) && !lv->by_ref) refScopes.back().push_back(lv);
            return;
        }
        if (auto es = dynamic_cast<ExprStmt*>(s)) {
            eval(es->expr.get());
            return;
        }
        if (auto as = dynamic_cast<AssignStmt*>(s)) {
            if (as->set_func) {
                auto* ix = dynamic_cast<Index*>(as->target.get());
                emitUserCall(as->set_func,
                             {ix->base.get(), ix->index.get(), as->value.get()});
                return;
            }
            Value* addr = addrOf(as->target.get());
            if (as->op == "=") {
                Value* nv = coerce(as->value.get(), as->target->ty);
                bool weakTarget = false;
                if (auto m = dynamic_cast<Member*>(as->target.get())) {
                    TypePtr ot = m->obj->ty;
                    if (ot && ot->kind == TypeKind::Class && m->field_index >= 0 &&
                        m->field_index < static_cast<int>(ot->fields.size())) {
                        weakTarget = ot->fields[m->field_index].weak;
                    }
                }
                if (isRefKind(as->target->ty) && !weakTarget) {
                    Value* old = b.CreateLoad(lt(as->target->ty), addr);
                    if (!ownsReference(as->value.get())) emitRetainRef(nv, as->target->ty);
                    emitReleaseRef(old, as->target->ty);
                    b.CreateStore(nv, addr);
                    return;
                }
                b.CreateStore(nv, addr);
                return;
            }
            Value* v = eval(as->value.get());
            std::string op = as->op.substr(0, as->op.size() - 1);
            Value* old = b.CreateLoad(lt(as->target->ty), addr);
            TypePtr rt = as->target->ty;
            if (rt->kind == TypeKind::Ptr) {
                Value* n = convertTo(v, as->value->ty, Type::make_prim("isize"));
                Value* np = gep(lt(rt->elem), old, op == "+" ? n : b.CreateNeg(n));
                b.CreateStore(np, addr);
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
            b.CreateStore(res, addr);
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
            Value* cond = eval(ws->cond.get());
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
        if (auto d = dynamic_cast<DelStmt*>(s)) {
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
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().second);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (dynamic_cast<ContinueStmt*>(s)) {
            if (loops.empty()) fail("LLVM 后端：continue 不在循环中");
            if (!loopScopeDepth.empty()) releaseScopesDeeperThan(loopScopeDepth.back());
            b.CreateBr(loops.back().first);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            if (!rs->value) {
                releaseAllScopes();
                b.CreateRetVoid();
            } else {
                Value* rv = prepareReturn(rs->value.get());
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
        if (simple) {
            seqAddr = addrOf(s->seq.get());
        } else {
            Value* v = eval(s->seq.get());
            seqAddr = entryAlloca(lt(sty), "seq.tmp");
            b.CreateStore(v, seqAddr);
        }

        // 长度与元素地址
        Value* len = nullptr;
        Value* elemPtr = nullptr;
        if (sty->kind == TypeKind::Array) {
            len = ConstantInt::get(i64(), sty->array_size);
            elemPtr = gep(cast<LStruct>(lt(sty)), seqAddr,
                                  {ConstantInt::get(i64(), 0), ConstantInt::get(i32(), 0),
                                   ConstantInt::get(i64(), 0)});
        } else {
            Value* sv = b.CreateLoad(lt(sty), seqAddr);
            Value* data = b.CreateExtractValue(sv, {0});
            len = b.CreateExtractValue(sv, {1});
            TypePtr et = sty->kind == TypeKind::Slice ? sty->elem : Type::make_prim("char");
            elemPtr = data;  // 元素类型 GEP 时按 et
            if (sty->kind == TypeKind::Slice) {
                // 指针就是元素指针
            }
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
            b.CreateStore(b.CreateLoad(lt(et), ep), varSlot);
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
        curClassTy = nullptr;
        curThis = nullptr;
        thisSlot = nullptr;
    }

    void bindParams(Function* fn, size_t first,
                    const std::vector<std::unique_ptr<LocalVar>>& pls) {
        size_t i = first;
        for (auto& pl : pls) {
            if (i >= fn->arg_size()) break;
            auto* slot = entryAlloca(lt(pl->rty), sane(pl->name));
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
        fn->setLinkage(GlobalValue::ExternalLinkage);
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
        fn->setLinkage(GlobalValue::ExternalLinkage);
        curClassTy = c->ty;
        beginBody(fn);
        curThis = fn->getArg(0);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(curThis, thisSlot);
        // 入口先设置本类的 desc；调用 super(...) 之后会恢复
        Value* dp = b.CreateGEP(i8(), curThis, ConstantInt::get(i64(), 8));
        b.CreateStore(descOf(c->ty), dp);
        bindParams(fn, 1, ctor->param_locals);
        block(ctor->body.get());
        finishBody(fn, c->name + " 构造器");
    }

    void emitReleaseValue(Value* addr, const TypePtr& ty) {
        if (!ty) return;
        if (isRefKind(ty)) {
            emitReleaseRef(b.CreateLoad(lt(ty), addr), ty);
            return;
        }
        if (ty->kind == TypeKind::Array && isRefKind(ty->elem)) {
            auto* st = cast<LStruct>(lt(ty));
            for (size_t i = 0; i < ty->array_size; ++i) {
                Value* ep = b.CreateGEP(st, addr, {ConstantInt::get(i64(), 0),
                                                   ConstantInt::get(i32(), 0),
                                                   ConstantInt::get(i64(), i)});
                emitReleaseRef(b.CreateLoad(lt(ty->elem), ep), ty->elem);
            }
        }
    }

    void emitDtor(ClassDecl* c) {
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(dtor_cname(c->ty->unit, c->name),
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false))
                .getCallee());
        fn->setLinkage(GlobalValue::ExternalLinkage);
        curClassTy = c->ty;
        beginBody(fn);
        curThis = fn->getArg(0);
        thisSlot = entryAlloca(ptrTy(), "this");
        b.CreateStore(curThis, thisSlot);
        if (c->dtor && c->dtor->body) block(c->dtor->body.get());

        // 释放本类声明的引用字段（逆序）
        size_t total = c->ty->fields.size();
        size_t own = c->fields.size();
        size_t baseCount = total >= own ? total - own : 0;
        for (size_t k = total; k > baseCount; --k) {
            size_t idx = k - 1;
            if (c->ty->fields[idx].weak) continue;
            Value* fa = fieldAddr(curThis, c->ty, static_cast<int>(idx));
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
        fn->setLinkage(GlobalValue::ExternalLinkage);
        beginBody(fn);
        Value* p = fn->getArg(0);
        FunctionCallee dtor = mod->getOrInsertFunction(
            dtor_cname(c->ty->unit, c->name),
            FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
        b.CreateCall(dtor, {p});
        b.CreateCall(libFn("free", LType::getVoidTy(ctx), {ptrTy()}), {p});
        finishBody(fn, c->name + " 销毁函数");
    }

    void emitClassDesc(ClassDecl* c) {
        std::string destroyName = "mu_" + c->ty->unit + "_destroy_" + c->name;
        FunctionCallee dfn =
            mod->getOrInsertFunction(destroyName,
                                     FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false));
        GlobalVariable* vt = emitVtable(c->ty);
        GlobalVariable* it = emitItable(c->ty);
        std::vector<Constant*> vals{
            ConstantInt::get(i64(), objectSize(c->ty)),
            cast<Constant>(dfn.getCallee()),
            vt ? cast<Constant>(vt) : Constant::getNullValue(ptrTy()),
            it ? cast<Constant>(it) : Constant::getNullValue(ptrTy()),
        };
        new GlobalVariable(*mod, descTy, /*isConstant=*/true, GlobalValue::InternalLinkage,
                           ConstantStruct::get(descTy, vals),
                           class_desc_name(c->ty->unit, c->name));
    }

    void emitInterfaceMethod(InterfaceDecl* itf, MethodDecl* m) {
        if (!m->body) return;
        std::vector<LType*> ptys{ifaceTy};
        for (auto& t : m->pty) ptys.push_back(lt(t));
        Function* fn = cast<Function>(
            mod->getOrInsertFunction(m->cname, FunctionType::get(lt(m->rty), ptys, false))
                .getCallee());
        fn->setLinkage(GlobalValue::ExternalLinkage);
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
        fn->setLinkage(GlobalValue::ExternalLinkage);
        if (f->is_extern) return;

        curClassTy = nullptr;
        curThis = nullptr;
        thisSlot = nullptr;
        refScopes.clear();
        loopScopeDepth.clear();
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
                auto* slot = entryAlloca(lt(pl->rty), sane(pl->name));
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
                auto linkage = (g->is_static || g->is_const) ? GlobalValue::InternalLinkage
                                                             : GlobalValue::ExternalLinkage;
                auto* gv = new GlobalVariable(*mod, ty, g->is_const, linkage, init, g->cname);
                if (g->is_const) gv->setConstant(true);
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
                                             std::nullopt, CodeGenOptLevel::None));
        mod = std::make_unique<Module>("mulan", ctx);
        mod->setTargetTriple(triple);
        if (tm) mod->setDataLayout(tm->createDataLayout());
        mod->addModuleFlag(Module::Warning, "mulan.version",
                           llvm::MDString::get(ctx, MULANC_VERSION));

        strTy = LStruct::create(ctx, "mu_string");
        strTy->setBody({ptrTy(), i64()}, false);
        sliceTy = LStruct::create(ctx, "mu_slice");
        sliceTy->setBody({ptrTy(), i64()}, false);
        descTy = LStruct::create(ctx, "mu_desc");
        descTy->setBody({i64(), ptrTy(), ptrTy(), ptrTy()}, false);
        ifaceTy = LStruct::create(ctx, "mu_iface");
        ifaceTy->setBody({ptrTy(), ptrTy()}, false);

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
        for (auto& u : prog.units) {
            curUnit = u->name;
            for (auto& d : u->decls) {
                auto c = dynamic_cast<ClassDecl*>(d.get());
                if (!c || !c->ty) continue;
                objStruct(c->ty);
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
            }
        }

        std::string errs;
        raw_string_ostream os(errs);
        if (verifyModule(*mod, &os)) {
            throw MuError("LLVM 后端：生成的模块无效：" + os.str());
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








