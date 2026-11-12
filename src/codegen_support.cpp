// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// This file contains the basic utilities, layout, functions/globals, reference counting runtime, allocation and constants (Emitter member function definitions).

#include "codegen_impl.hpp"

namespace muc_detail {

Emitter::Emitter(Program& p, bool dbg) : prog(p), wantDebug(dbg) {}

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

bool is_void(const TypePtr& t) { return t && t->kind == TypeKind::Void; }

    // ------------------------------------------------------------ basic utilities
    LType* Emitter::ptrTy() { return PointerType::get(ctx, 0); }
    LType* Emitter::i1() { return LType::getInt1Ty(ctx); }
    LType* Emitter::i8() { return LType::getInt8Ty(ctx); }
    LType* Emitter::i32() { return LType::getInt32Ty(ctx); }
    LType* Emitter::i64() { return LType::getInt64Ty(ctx); }

    [[noreturn]] void Emitter::fail(const std::string& m, int line, int col) {
        throw MuError(m, line, col, prog.units.empty() ? "" : prog.units.front()->filename);
    }

    LType* Emitter::primTy(const std::string& n) {
        if (n == "bool") return i1();
        if (n == "byte" || n == "char") return i8();
        if (n == "short" || n == "ushort") return LType::getInt16Ty(ctx);
        if (n == "int" || n == "uint") return i32();
        if (n == "long" || n == "ulong" || n == "usize" || n == "isize") return i64();
        if (n == "float") return LType::getFloatTy(ctx);
        if (n == "double") return LType::getDoubleTy(ctx);
        fail("LLVM backend: unknown primitive type " + n);
    }

    LStruct* Emitter::structOf(const TypePtr& t) {
        std::string key = "s:" + t->unit + ":" + t->name;
        auto it = named.find(key);
        if (it != named.end()) return it->second;
        auto* st = LStruct::create(ctx, "mu_" + sane(t->unit) + "_" + sane(t->name));
        named[key] = st;
        const auto& dl = mod->getDataLayout();
        std::vector<LType*> fs;
        std::vector<unsigned> map;  // model field index -> LLVM field index
        uint64_t llvmCur = 0;       // current offset from LLVM's point of view
        uint64_t modelCur = 0;      // current offset in the semantic layer (types.cpp)

        if (t->is_union) {
            // LLVM has no union type: represent it with an "aligned carrier + trailing i8 padding" struct,
            // so that the ABI size / align match the semantic layer; all members are read from offset 0 (fieldAddr special-cases this).
            uint64_t sz = sizeOf(t);
            uint64_t al = alignOf(t);
            if (sz == 0) fail("LLVM backend: union `" + t->str() + "` has no members");
            if (t->is_packed) {
                fs.push_back(LArray::get(i8(), sz));
                st->setBody(fs, true);  // packed: alignment 1
            } else {
                LType* carrier = i8();
                uint64_t csz = 1;
                if (al >= 8 && sz >= 8) {
                    carrier = i64();
                    csz = 8;
                } else if (al >= 4 && sz >= 4) {
                    carrier = i32();
                    csz = 4;
                } else if (al >= 2 && sz >= 2) {
                    carrier = LType::getInt16Ty(ctx);
                    csz = 2;
                }
                fs.push_back(carrier);
                if (sz > csz) fs.push_back(LArray::get(i8(), sz - csz));
                st->setBody(fs, false);
            }
            map.push_back(0);
            fieldMap[key] = std::move(map);
            return st;
        }

        for (auto& f : t->fields) {
            LType* ft = lt(f.type);
            if (!ft->isSized()) {
                fail("LLVM backend: struct `" + t->str() + "` field `" + f.name + "` is not a fixed-size type");
            }
            uint64_t fsz = dl.getTypeAllocSize(ft);
            uint64_t fa = alignOf(f.type);
            uint64_t la = dl.getABITypeAlign(ft).value();
            uint64_t modelOff = t->is_packed ? modelCur : (modelCur + fa - 1) / fa * fa;
            uint64_t llvmOff = t->is_packed ? llvmCur : (llvmCur + la - 1) / la * la;
            if (llvmOff < modelOff) {
                // Explicit padding: make the LLVM offsets match the semantic layer (align(N) fields rely on it for alignment in nested structs)
                fs.push_back(LArray::get(i8(), modelOff - llvmOff));
                llvmCur = modelOff;
                llvmOff = modelOff;
            } else if (llvmOff > modelOff) {
                fail("LLVM backend: struct `" + t->str() + "` field `" + f.name +
                     "` has a layout mismatch with sema");
            }
            map.push_back(static_cast<unsigned>(fs.size()));
            fs.push_back(ft);
            llvmCur = llvmOff + fsz;
            modelCur = modelOff + sizeOf(f.type);
        }
        // Tail padding: the LLVM struct size must equal the semantic layer's sizeof (array element strides depend on this invariant)
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
            fail("LLVM backend: size of struct `" + t->str() + "` does not match sema (" +
                 std::to_string(llvmEnd) + " / " + std::to_string(modelSize) + ")");
        }
        st->setBody(fs, t->is_packed);
        fieldMap[key] = std::move(map);
        return st;
    }

    // model field index -> LLVM field index (align(N) inserts explicit [k x i8] padding between fields)
    unsigned Emitter::llvmFieldIndex(const TypePtr& t, int model_idx) {
        const char* prefix = t->kind == TypeKind::Class ? "o:" : "s:";
        auto it = fieldMap.find(prefix + t->unit + ":" + t->name);
        if (it == fieldMap.end() || model_idx < 0 ||
            static_cast<size_t>(model_idx) >= it->second.size()) {
            return static_cast<unsigned>(model_idx);
        }
        return it->second[static_cast<size_t>(model_idx)];
    }

    LStruct* Emitter::arrayOf(const TypePtr& t) {
        std::string key = "a:" + t->str();
        auto it = named.find(key);
        if (it != named.end()) return it->second;
        auto* st = LStruct::create(ctx, "mu_arr_" + sane(t->str()));
        named[key] = st;
        st->setBody({LArray::get(lt(t->elem), t->array_size)}, false);
        return st;
    }

    uint64_t Emitter::alignUp(uint64_t v, uint64_t a) {
        return a <= 1 ? v : (v + a - 1) / a * a;
    }


    uint64_t Emitter::classObjAlign(const TypePtr& cls) {
        uint64_t a = kObjHeader;  // heap allocation guarantees at least 16 bytes
        for (auto& f : cls->fields) a = std::max(a, alignOf(f.type));
        return a;
    }

    uint64_t Emitter::classFieldOffset(const TypePtr& cls, int idx) {
        uint64_t off = kObjHeader;
        for (int i = 0; i < idx; ++i) {
            off = alignUp(off, alignOf(cls->fields[i].type));
            off += sizeOf(cls->fields[i].type);
        }
        return alignUp(off, alignOf(cls->fields[idx].type));
    }

    uint64_t Emitter::classObjSize(const TypePtr& cls) {
        uint64_t off = kObjHeader;
        for (auto& f : cls->fields) {
            off = alignUp(off, alignOf(f.type));
            off += sizeOf(f.type);
        }
        return alignUp(off, classObjAlign(cls));
    }

    // object layout: { i64 rc, ptr desc, [padding], fields... } (fields are already flattened in sema, base class fields first)
    // Like structOf, explicit [k x i8] padding is inserted so LLVM offsets match the semantic layer's type_align/type_size.
    LStruct* Emitter::objStruct(const TypePtr& cls) {
        std::string key = "o:" + cls->unit + ":" + cls->name;
        auto it = named.find(key);
        if (it != named.end()) return it->second;
        auto* st = LStruct::create(ctx, "mu_obj_" + sane(cls->unit) + "_" + sane(cls->name));
        named[key] = st;
        const auto& dl = mod->getDataLayout();
        std::vector<LType*> fs{i64(), ptrTy()};
        std::vector<unsigned> map;  // model field index -> LLVM field index
        uint64_t llvmCur = kObjHeader;
        uint64_t modelCur = kObjHeader;
        for (auto& f : cls->fields) {
            LType* ft = lt(f.type);
            if (!ft->isSized()) {
                fail("LLVM backend: class `" + cls->str() + "` field `" + f.name + "` is not a fixed-size type");
            }
            uint64_t fsz = dl.getTypeAllocSize(ft);
            uint64_t fa = alignOf(f.type);
            uint64_t la = dl.getABITypeAlign(ft).value();
            uint64_t modelOff = alignUp(modelCur, fa);
            uint64_t llvmOff = alignUp(llvmCur, la);
            if (llvmOff < modelOff) {
                // Explicit padding: make the field offsets satisfy align(N) (LLVM itself only lays out by the natural alignment of the type)
                fs.push_back(LArray::get(i8(), modelOff - llvmOff));
                llvmCur = modelOff;
                llvmOff = modelOff;
            } else if (llvmOff > modelOff) {
                fail("LLVM backend: class `" + cls->str() + "` field `" + f.name +
                     "` has a layout mismatch with sema");
            }
            map.push_back(static_cast<unsigned>(fs.size()));
            fs.push_back(ft);
            llvmCur = llvmOff + fsz;
            modelCur = modelOff + sizeOf(f.type);
        }
        // Pad the tail up to the object alignment (do not rely on the LLVM struct's natural alignment: it is only 8 when all fields are 8-byte types)
        uint64_t modelSize = alignUp(modelCur, classObjAlign(cls));
        if (llvmCur < modelSize) {
            fs.push_back(LArray::get(i8(), modelSize - llvmCur));
        } else if (llvmCur > modelSize) {
            fail("LLVM backend: size of class `" + cls->str() + "` does not match sema (" +
                 std::to_string(llvmCur) + " / " + std::to_string(modelSize) + ")");
        }
        st->setBody(fs, false);
        if (dl.getTypeAllocSize(st) != modelSize) {
            fail("LLVM backend: LLVM size of class `" + cls->str() + "` does not match sema (" +
                 std::to_string(dl.getTypeAllocSize(st)) + " / " + std::to_string(modelSize) +
                 "）");
        }
        fieldMap[key] = std::move(map);
        return st;
    }


    uint64_t Emitter::objectSize(const TypePtr& cls) { return classObjSize(cls); }

    uint64_t Emitter::objectAlign(const TypePtr& cls) { return classObjAlign(cls); }

    // class objects with align(N) > 16 need an explicitly aligned allocation (malloc only guarantees 16 bytes); the release must be paired
    bool Emitter::classNeedsAlignedAlloc(const TypePtr& cls) { return classObjAlign(cls) > 16; }

    // field address: all members of a union land at offset 0 (the LLVM type is "carrier + padding", field 0 is the base address)
    Value* Emitter::fieldSlot(LStruct* st, const TypePtr& ty, Value* base, int idx) {
        if (ty->kind == TypeKind::Struct && ty->is_union) {
            (void)idx;
            return b.CreateStructGEP(st, base, 0);
        }
        return b.CreateStructGEP(st, base, llvmFieldIndex(ty, idx));
    }

    Value* Emitter::fieldAddr(Value* objPtr, const TypePtr& cls, int field_index) {
        if (cls->kind == TypeKind::Struct) {
            // First obtain the LLVM type (this may trigger structOf to fill the field mapping table), then look up the mapping
            LStruct* st = cast<LStruct>(lt(cls));
            return fieldSlot(st, cls, objPtr, field_index);
        }
        // First build the object type (filling fieldMap), then index by the mapping (there is explicit padding before align(N) fields)
        LStruct* st = objStruct(cls);
        unsigned idx = llvmFieldIndex(cls, field_index);
        return b.CreateStructGEP(st, objPtr, idx);
    }

    Value* Emitter::descOf(const TypePtr& cls) {
        return mod->getOrInsertGlobal(class_desc_name(cls->unit, cls->name), descTy);
    }

    LType* Emitter::lt(const TypePtr& t) {
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

    // ------------------------------------------------------------ layout
    // The only implementation is in types.cpp (type_align/type_size); this only forwards to avoid two layouts drifting apart.
    uint64_t Emitter::alignOf(const TypePtr& t) { return type_align(t); }
    uint64_t Emitter::sizeOf(const TypePtr& t) { return type_size(t); }

    bool Emitter::zeroable(const TypePtr& t) {
        return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Array ||
                     t->kind == TypeKind::Slice);
    }

    // ------------------------------------------------------------ functions/globals
    FunctionCallee Emitter::libFn(const char* name, LType* ret, std::vector<LType*> params) {
        return mod->getOrInsertFunction(name, FunctionType::get(ret, params, false));
    }

    Constant* Emitter::zeroConst(LType* ty, const TypePtr& mt) {
        if (mt && mt->kind == TypeKind::Slice) return Constant::getNullValue(ty);
        if (mt && zeroable(mt)) return ConstantAggregateZero::get(ty);
        return Constant::getNullValue(ty);
    }

    // A string literal (3.3): a static, NUL-terminated UTF-8 buffer. Its value is a `const char*`.
    Value* Emitter::stringConst(const std::string& bytes) {
        auto it = strCache.find(bytes);
        if (it != strCache.end()) return it->second;
        Constant* arr = ConstantDataArray::getString(ctx, bytes, true);
        auto* gv = new GlobalVariable(*mod, arr->getType(), true, GlobalValue::PrivateLinkage,
                                      arr, ".str");
        strCache[bytes] = gv;
        return gv;
    }

    // ------------------------------------------------------------ reference counting runtime
    Function* Emitter::ensureRetain() {
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

    Function* Emitter::ensureRelease() {
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
        // the destroy function calls the destructor chain and frees, so do not free here (that would double free)
        hb.CreateCall(FunctionType::get(LType::getVoidTy(ctx), {ptrTy()}, false), dfn, {p});
        hb.CreateBr(done);
        hb.SetInsertPoint(done);
        hb.CreateRetVoid();
        rcReleaseFn = fn;
        return fn;
    }

    void Emitter::emitRetain(Value* p) {
        if (!p) return;
        b.CreateCall(ensureRetain(), {p});
    }

    void Emitter::emitRelease(Value* p) {
        if (!p) return;
        b.CreateCall(ensureRelease(), {p});
    }

    // ------------------------------------------------------------ allocation
    // model_align: semantic layer alignment (align(N) may exceed the natural alignment of the LLVM type)
    AllocaInst* Emitter::entryAlloca(LType* ty, const std::string& name, uint64_t model_align) {
        IRBuilder<>::InsertPointGuard g(ab);
        if (allocaBB) ab.SetInsertPoint(allocaBB);
        auto* a = ab.CreateAlloca(ty, nullptr, name);
        if (model_align > 1) a->setAlignment(llvm::Align(model_align));
        return a;
    }

    // byte offset of a struct member (consistent with the layout rules in types.cpp; 0 for all union members)
    uint64_t Emitter::memberOffset(const TypePtr& t, size_t idx) {
        if (t->is_union) return 0;
        uint64_t off = 0;
        for (size_t i = 0; i <= idx && i < t->fields.size(); ++i) {
            const TypePtr& ft = t->fields[i].type;
            const uint64_t fa = alignOf(ft);
            if (!t->is_packed) off = (off + fa - 1) / fa * fa;
            if (i == idx) return off;
            off += sizeOf(ft);
        }
        return 0;
    }

    // Mulan type -> DWARF type. Structs expand their members (going down layer by layer, depth limit 2);
    // pointers only recurse into the element type for "scalar / enum", everything else is shown as void* (to avoid unbounded recursion for self-references).
    DIType* Emitter::diType(const TypePtr& t, int depth) {
        if (!dib || !t) return nullptr;
        const uint64_t bits = static_cast<uint64_t>(sizeOf(t) * 8);
        if (depth > 2) return dib->createBasicType(t->str(), bits, llvm::dwarf::DW_ATE_unsigned);

        if (t->kind == TypeKind::Prim) {
            unsigned enc = llvm::dwarf::DW_ATE_signed;
            if (t->name == "bool") {
                enc = llvm::dwarf::DW_ATE_boolean;
            } else if (t->name == "float" || t->name == "double") {
                enc = llvm::dwarf::DW_ATE_float;
            } else if (t->name == "byte" || t->name == "char" || t->name == "ushort" ||
                       t->name == "uint" || t->name == "ulong" || t->name == "usize") {
                enc = llvm::dwarf::DW_ATE_unsigned;
            }
            return dib->createBasicType(t->str(), bits, enc);
        }
        if (t->kind == TypeKind::Enum) {
            return dib->createBasicType(t->str(), bits ? bits : 32, llvm::dwarf::DW_ATE_signed);
        }
        if (t->kind == TypeKind::Ptr) {
            DIType* pointee = nullptr;
            if (t->elem && (t->elem->kind == TypeKind::Prim || t->elem->kind == TypeKind::Enum)) {
                pointee = diType(t->elem, depth + 1);
            } else {
                pointee = dib->createBasicType("void", 0, llvm::dwarf::DW_ATE_unsigned);
            }
            return dib->createPointerType(pointee, bits);
        }
        if (t->kind == TypeKind::Struct) {
            std::vector<llvm::Metadata*> members;
            members.reserve(t->fields.size());
            for (size_t i = 0; i < t->fields.size(); ++i) {
                const auto& f = t->fields[i];
                members.push_back(dib->createMemberType(
                    diCu, f.name, diFile, 0,
                    ConstantAsMetadata::get(ConstantInt::get(
                        i64(), static_cast<uint64_t>(sizeOf(f.type) * 8))),
                    static_cast<uint32_t>(alignOf(f.type) * 8),
                    ConstantAsMetadata::get(ConstantInt::get(
                        i64(), static_cast<uint64_t>(memberOffset(t, i) * 8))),
                    DINode::FlagZero, diType(f.type, depth + 1)));
            }
            auto* sizeMeta =
                ConstantAsMetadata::get(ConstantInt::get(i64(), bits));
            return dib->createStructType(diCu, t->str(), diFile, 0, sizeMeta,
                                         static_cast<uint32_t>(alignOf(t) * 8),
                                         DINode::FlagZero, nullptr,
                                         dib->getOrCreateArray(members));
        }
        return dib->createBasicType(t->str(), bits, llvm::dwarf::DW_ATE_unsigned);
    }

    AllocaInst* Emitter::slotFor(const LocalVar* lv) {
        auto it = slots.find(lv);
        if (it != slots.end()) return it->second;
        LType* ty = lt(lv->rty);
        if (lv->by_ref) ty = ptrTy();
        auto* a = entryAlloca(ty, sane(lv->name), alignOf(lv->rty));
        slots[lv] = a;
        if (dib && diSub) {
            const unsigned line = lv->line > 0 ? static_cast<unsigned>(lv->line) : 1u;
            const unsigned col = lv->col > 0 ? static_cast<unsigned>(lv->col) : 1u;
            auto* dv = dib->createAutoVariable(diSub, lv->name, diFile, line, diType(lv->rty));
            dib->insertDeclare(a, dv, dib->createExpression(),
                               DILocation::get(ctx, line, col, diSub), allocaBB);
        }
        return a;
    }

    // ------------------------------------------------------------ constants
    // type-level / unit-level compile-time constants: emit literal constants from the cached evaluation result (bit width and signedness follow the declared type)
    Constant* Emitter::constOfValue(const TypePtr& ty, bool is_float, long long iv, double fv) {
        LType* ct = lt(ty);
        if (is_float) return ConstantFP::get(ct, fv);
        if (ty && ty->is_bool()) return ConstantInt::get(i1(), iv != 0 ? 1 : 0);
        auto* it = dyn_cast<IntegerType>(ct);
        if (!it) fail("LLVM backend: constant value for non-integer type " + (ty ? ty->str() : "?"));
        return ConstantInt::get(it, static_cast<uint64_t>(iv), /*isSigned=*/true);
    }

    Constant* Emitter::constValue(const TypePtr& ty, Init* init) {
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
                // the LLVM struct may have explicit padding fields (packed / align(N)), expand according to the mapping table
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
            fail("LLVM backend: unsupported global aggregate initializer", ia->line, ia->col);
        }
        auto ie = dynamic_cast<InitExpr*>(init);
        if (!ie) fail("LLVM backend: unknown initializer");
        Expr* e = ie->expr.get();
        if (auto il = dynamic_cast<IntLit*>(e)) {
            if (ty->kind == TypeKind::Slice && il->value == 0) {
                return Constant::getNullValue(ct);
            }
            // integer literals adapt to the float type according to context (SPEC §3.6)
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
        if (auto u = dynamic_cast<Unary*>(e)) {
            // constant initializers support unary +/-: forms like `const int EOF = -1;` are common
            if (u->op == "-" || u->op == "+") {
                const double sign = (u->op == "-") ? -1.0 : 1.0;
                if (auto il = dynamic_cast<IntLit*>(u->operand.get())) {
                    const long long v = (u->op == "-") ? -il->value : il->value;
                    if (ty->kind == TypeKind::Prim && ty->is_float())
                        return ConstantFP::get(ct, static_cast<double>(v));
                    auto* it = IntegerType::get(ctx, static_cast<unsigned>(sizeOf(ty) * 8));
                    return ConstantInt::get(it, static_cast<uint64_t>(v), /*isSigned=*/true);
                }
                if (auto fl = dynamic_cast<FloatLit*>(u->operand.get()))
                    return ConstantFP::get(ct, sign * fl->value);
            }
        }
        if (auto sl = dynamic_cast<StringLit*>(e)) {
            if (ty->kind == TypeKind::Ptr) return cast<Constant>(stringConst(sl->value));
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
        fail("LLVM backend: global initializer must be a constant", e->line, e->col);
    }

}  // namespace muc_detail
