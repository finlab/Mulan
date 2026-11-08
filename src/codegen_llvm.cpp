// LLVM 后端：用 LLVM C++ API 直接构造 IR，产出 COFF 目标文件（由 gcc 链接）。
//
// 策略：所有局部变量用 alloca 承载（配合 -O 时可被 mem2reg 优化），
// 类型 / 命名 / lowering 与 C 后端保持一致，便于两后端对拍。
#include "codegen_llvm.hpp"

#include "common.hpp"

#if defined(MULANC_HAVE_LLVM)

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

bool llvm_backend_available() { return true; }
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
    std::map<std::string, LStruct*> named;
    std::map<std::string, Constant*> strCache;

    BasicBlock* allocaBB = nullptr;
    BasicBlock* bodyBB = nullptr;
    Function* curFn = nullptr;
    std::string curUnit;
    std::map<const LocalVar*, AllocaInst*> slots;
    std::vector<std::pair<BasicBlock*, BasicBlock*>> loops;  // (continue, break)

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
            Value* base = addrOf(m->obj.get());
            TypePtr ot = m->obj->ty;
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
        if (dynamic_cast<NullLit*>(e)) return Constant::getNullValue(ptrTy());
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
        if (dynamic_cast<Index*>(e)) {
            return b.CreateLoad(lt(e->ty), addrOf(e));
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
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
        for (auto& x : blk->stmts) stmt(x.get());
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
            if (lv->init) initInto(slot, lv->rty, lv->init.get());
            return;
        }
        if (auto es = dynamic_cast<ExprStmt*>(s)) {
            eval(es->expr.get());
            return;
        }
        if (auto as = dynamic_cast<AssignStmt*>(s)) {
            Value* addr = addrOf(as->target.get());
            if (as->op == "=") {
                b.CreateStore(coerce(as->value.get(), as->target->ty), addr);
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
            block(ws->body.get());
            loops.pop_back();
            if (!hasTerm(b.GetInsertBlock())) b.CreateBr(condBB);
            b.SetInsertPoint(endBB);
            return;
        }
        if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            emitFor(fs);
            return;
        }
        if (dynamic_cast<BreakStmt*>(s)) {
            if (loops.empty()) fail("LLVM 后端：break 不在循环中");
            b.CreateBr(loops.back().second);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (dynamic_cast<ContinueStmt*>(s)) {
            if (loops.empty()) fail("LLVM 后端：continue 不在循环中");
            b.CreateBr(loops.back().first);
            BasicBlock* dead = BasicBlock::Create(ctx, "dead", curFn);
            b.SetInsertPoint(dead);
            return;
        }
        if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            if (!rs->value) {
                b.CreateRetVoid();
            } else {
                b.CreateRet(coerce(rs->value.get(), curRetTy));
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
            block(s->body.get());
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
        block(s->body.get());
        loops.pop_back();
        if (!hasTerm(b.GetInsertBlock())) b.CreateBr(incBB);
        b.SetInsertPoint(incBB);
        Value* ci3 = b.CreateLoad(i64(), idx);
        b.CreateStore(b.CreateAdd(ci3, ConstantInt::get(i64(), 1)), idx);
        b.CreateBr(condBB);
        b.SetInsertPoint(endBB);
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

        // 先建结构体类型（字段可能引用彼此；指针是 opaque ptr，不会递归）
        for (auto& u : prog.units) {
            for (auto& d : u->decls) {
                if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                    if (sd->ty) structOf(sd->ty);
                }
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

#else  // !MULANC_HAVE_LLVM

bool llvm_backend_available() { return false; }

std::string llvm_emit_text(Program&) { return "; LLVM backend not built\n"; }

void llvm_emit_object(Program&, const std::string&) {
    throw MuError("本编译器构建时未启用 LLVM 后端（重新配置 CMake 并安装 LLVM）");
}

#endif







