// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// LLVM backend internals: Emitter class declaration and cross-file helper declarations (for codegen_*.cpp only)
//
// Member functions are split by functionality across codegen_support / codegen_runtime / codegen_expr / codegen_expr2 /
// codegen_stmt / codegen_class / codegen_func; the public interface is in codegen_llvm.hpp.
#pragma once

#include "codegen_llvm.hpp"

#include "common.hpp"
#include "mangle.hpp"
#include "may_throw.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
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
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
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
using llvm::DICompileUnit;
using llvm::DIBuilder;
using llvm::DebugLoc;
using llvm::DIFile;
using llvm::DIType;
using llvm::ConstantAsMetadata;
using llvm::DILocation;
using llvm::DINode;
using llvm::DISourceLanguageName;
using llvm::DISubprogram;
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


#ifndef MUC_VERSION
#define MUC_VERSION "0.0.0"
#endif

namespace muc_detail {

// ------------------------------------------------------------ cross-file helpers

// Turn an arbitrary string into a legal symbol fragment (defined in codegen_support.cpp)
std::string sane(std::string s);

// Whether the initializer expression is the literal 0 (defined in codegen_support.cpp)
bool is_zero_literal(Init* init);

bool is_void(const TypePtr& t);

// ------------------------------------------------------------ Emitter

struct Emitter {
    explicit Emitter(Program& p, bool dbg = false, bool bounds = true);

    Program& prog;
    bool wantDebug = false;
    bool boundsCheck = true;
    MayThrowInfo mayThrow;
    LLVMContext ctx;
    std::unique_ptr<Module> mod;
    std::unique_ptr<TargetMachine> tm;
    IRBuilder<> b{ctx};
    IRBuilder<> ab{ctx};

    TypePtr strMeta;  // semantic type of std.string.string (looked up lazily)

    // Debug info (DWARF): compile unit / file / subprogram of the current function
    std::unique_ptr<DIBuilder> dib;
    DICompileUnit* diCu = nullptr;
    DIFile* diFile = nullptr;
    DISubprogram* diSub = nullptr;    LType* sliceTy = nullptr;  // dynamic array handle
    std::map<std::string, Function*> sliceFreeCache;  // element type -> free function
    LStruct* descTy = nullptr;
    LStruct* ifaceTy = nullptr;  // {ptr obj, ptr desc}
    std::map<std::string, LStruct*> named;
    std::map<std::string, std::vector<unsigned>> fieldMap;  // struct: model field index -> LLVM index
    std::map<std::string, Constant*> strCache;

    BasicBlock* allocaBB = nullptr;
    BasicBlock* bodyBB = nullptr;
    Function* curFn = nullptr;
    std::string curUnit;
    std::map<const LocalVar*, AllocaInst*> slots;

    // `para` capture support: while a section body is being emitted, a captured local resolves to
    // `load(frame->field)` (the suspended caller's storage) instead of an alloca of the section.
    std::map<const LocalVar*, int> captureIndex;
    Value* frameArg = nullptr;
    LType* frameTy = nullptr;
    struct PendingPara {
        Block* body = nullptr;
        std::vector<LocalVar*> captures;  // index i -> frame field i
        Function* bodyFn = nullptr;             // void(ptr): the section body
        Function* wrapFn = nullptr;             // void(ptr): calls the body, aborts on an escaped throw
        LType* frameTy = nullptr;
    };
    std::vector<PendingPara> pendingParas;
    // `para for` (SPEC 3.17.3): one task per item. The item body is a hidden function `void(void*)`
    // that receives a per-item context `{ frame*, item }` (the item is the element address for
    // `for ref`); the context is heap-allocated by the producer and freed by the task wrapper.
    struct PendingParaFor {
        ParaForStmt* p = nullptr;
        Function* bodyFn = nullptr;
        Function* wrapFn = nullptr;
        LType* frameTy = nullptr;
        LType* ctxTy = nullptr;
        TypePtr elemTy;
        bool byRef = false;
    };
    std::vector<PendingParaFor> pendingParaFors;
    // While a `para for` source is being lowered in the producer: the ordinary for-in lowering runs
    // the loop control, and its body is replaced by "start one task for the current item".
    struct ParaForSpawn {
        Function* wrapFn = nullptr;
        Value* frame = nullptr;   // void*: the capture frame (points at the producer's slots)
        Value* groupSlot = nullptr;  // alloca holding the region's task-list handle
        TypePtr elemTy;
        bool byRef = false;
    };
    ParaForSpawn* paraSpawn = nullptr;
    // Every `para for` region whose producer is being emitted, innermost last. An unwind out of the
    // producer (a throwing `next()`/`current()`/`operator[]`/source expression) must join the tasks
    // that already started before the unwinding scope releases the slots they read.
    std::vector<std::function<void()>> paraUnwindCleanup;
    int paraSeq = 0;
    std::vector<std::pair<BasicBlock*, BasicBlock*>> loops;  // (continue, break)
    TypePtr curClassTy;   // class the current method/constructor/destructor belongs to
    Value* curThis = nullptr;  // current this pointer (the first parameter at method entry)
    AllocaInst* thisSlot = nullptr;
    Function* rcRetainFn = nullptr;
    Function* rcReleaseFn = nullptr;
    Function* weakRegFn = nullptr;
    Function* weakUnregFn = nullptr;
    Function* weakNotifyFn = nullptr;
    LStruct* weakNodeTy = nullptr;
    std::vector<std::vector<const LocalVar*>> refScopes;  // reference local variables that need releasing
    std::vector<size_t> loopScopeDepth;
    // owned struct/array temporaries materialized by taking their address inside a statement, released together at the end of the statement
    std::vector<std::pair<Value*, TypePtr>> stmtTemps;

    // ------------------------------------------------------------ exceptions (B2: TLS pending slot + call-site checks)
    struct EHContext {
        BasicBlock* dispatch = nullptr;  // catch dispatch block of the innermost try
        size_t scopeDepth = 0;           // refScopes depth at try entry
        size_t tempDepth = 0;            // stmtTemps size at try entry
        size_t finallyDepth = 0;         // finallyStack size after pushing this try's finally
    };
    struct FinallyCtx {
        Block* body = nullptr;
        int loopDepth = 0;               // loops.size() at try entry (break/continue filtering)
    };
    std::vector<EHContext> ehStack;
    std::vector<FinallyCtx> finallyStack;  // outer -> inner
    std::vector<size_t> abortBases;        // cleanup contexts that must not let an exception escape
    std::vector<std::unique_ptr<LocalVar>> ehLocals;  // synthetic catch owners
    GlobalVariable* ehSlot = nullptr;
    bool curIsEntry = false;   // main: an uncaught exception aborts instead of returning a dummy
    bool inNothrow = false;    // destructors: any pending exception aborts

    void ensureEHSlot();
    Value* loadPendingFlag();
    Value* loadPendingObj();
    void storePending(Value* obj);
    void storePendingState(Value* obj, Value* flag);
    void clearPending();
    void emitCheckPending();
    void emitCheckPendingWith(const std::function<void()>& onThrow);
    void emitReleaseTempsFrom(size_t mark);
    void emitUnwindCurrent(const std::function<void()>& onThrow = {});
    void emitReturnDummy();
    void emitThrow(Expr* e);
    void emitTry(TryStmt* t);
    void emitFinallyBody(Block* body);
    void emitFinallyBodies(size_t count);
    void emitFinallyForLoopExit();
    void emitAbortIfPending();
    Value* emitDescMatches(Value* desc, Value* target);


    // class object layout (model): { i64 rc, ptr desc } fixed at 16 bytes, fields start at 16 and are laid out per type_align.
    static constexpr uint64_t kObjHeader = 16;
    static constexpr unsigned kObjRc = 0;
    static constexpr unsigned kObjDesc = 1;
    static constexpr uint64_t kArrRc = 0;
    TypePtr curRetTy;
    std::vector<const MethodSig*> islotSigs;
    std::map<std::string, int> islotIndex;
    std::map<std::string, Function*> thunkCache;
    std::map<const LType*, std::vector<const MethodSig*>> vtabMemo;
    std::map<const Type*, std::vector<const MethodSig*>> vtabMemo2;

    struct ArgPack {
        std::vector<Value*> vals;
        std::vector<std::pair<Value*, TypePtr>> owned;
    };
    // ------------------------------------------------------------ basic utilities
    LType* ptrTy();
    LType* i1();
    LType* i8();
    LType* i32();
    LType* i64();

    [[noreturn]] void fail(const std::string& m, int line = 0, int col = 0);

    LType* primTy(const std::string& n);

    LStruct* structOf(const TypePtr& t);

    // model field index -> LLVM field index (align(N) inserts explicit [k x i8] padding between fields)
    unsigned llvmFieldIndex(const TypePtr& t, int model_idx);

    LStruct* arrayOf(const TypePtr& t);

    static uint64_t alignUp(uint64_t v, uint64_t a);


    uint64_t classObjAlign(const TypePtr& cls);

    uint64_t classFieldOffset(const TypePtr& cls, int idx);

    uint64_t classObjSize(const TypePtr& cls);

    // object layout: { i64 rc, ptr desc, [padding], fields... } (fields are already flattened in sema, base class fields first)
    // Like structOf, explicit [k x i8] padding is inserted so LLVM offsets match the semantic layer's type_align/type_size.
    LStruct* objStruct(const TypePtr& cls);


    uint64_t objectSize(const TypePtr& cls);

    uint64_t objectAlign(const TypePtr& cls);

    // class objects with align(N) > 16 need an explicitly aligned allocation (malloc only guarantees 16 bytes); the release must be paired
    bool classNeedsAlignedAlloc(const TypePtr& cls);

    // field address: all members of a union land at offset 0 (the LLVM type is "carrier + padding", field 0 is the base address)
    Value* fieldSlot(LStruct* st, const TypePtr& ty, Value* base, int idx);

    Value* fieldAddr(Value* objPtr, const TypePtr& cls, int field_index);

    Value* descOf(const TypePtr& cls);

    LType* lt(const TypePtr& t);

    // ------------------------------------------------------------ layout
    // The only implementation is in types.cpp (type_align/type_size); this only forwards to avoid two layouts drifting apart.
    uint64_t alignOf(const TypePtr& t);
    uint64_t sizeOf(const TypePtr& t);

    bool zeroable(const TypePtr& t);

    // ------------------------------------------------------------ functions/globals
    FunctionCallee libFn(const char* name, LType* ret, std::vector<LType*> params);

    Constant* zeroConst(LType* ty, const TypePtr& mt);

    Value* stringConst(const std::string& bytes);

    // ------------------------------------------------------------ reference counting runtime
    Function* ensureRetain();

    Function* ensureRelease();

    void emitRetain(Value* p);

    void emitRelease(Value* p);

    // ------------------------------------------------------------ dynamic arrays (T[] / T[,])
    // rank 1: { i64 rc; i64 len; i64 cap; data... }
    // rank r>=2: { i64 rc; i64 d0; ... d(r-1); data... }

    static uint64_t sliceDataOffset(int rank);

    Value* sliceDataPtr(Value* h, int rank);

    Value* sliceDimField(Value* h, int axis);

    // Bounds check for a managed sequence subscript: per-axis checks, then the linear index.
    // Emits a call to std.exception.bounds_fail on the cold path.
    Value* emitSliceIndexChecked(Value* h, const std::vector<Value*>& idxs, const TypePtr& bt);
    void emitBoundsFail(Value* index, Value* length, Value* axis);

    // total element count (rank 1 uses len, rank >= 2 uses the product of the dimensions)
    Value* sliceTotalCount(Value* h, int rank);

    // len(a, k): a null handle yields 0; k is a compile-time constant
    Value* emitSliceDim(Value* h, int axis);

    // len(a, k): k is a runtime expression (with an out-of-bounds check)
    Value* emitSliceDimDynamic(Value* h, Value* axis, int rank);

    // total element count (a null handle yields 0)
    Value* emitSliceTotal(Value* h, int rank);

    // Allocate a dynamic array and zero it; dims holds the size of each dimension
    Value* emitSliceAlloc(const std::vector<Value*>& dims, const TypePtr& elem);

    Value* emitDynArrayAlloc(Value* n, const TypePtr& elem);

    void emitSliceRetain(Value* h);

    // element release function: release the elements one by one (when they hold references), then free the header
    Function* ensureSliceFree(const TypePtr& elem, int rank);

    void emitSliceRelease(Value* h, const TypePtr& sliceTy);

    // fixed-size array -> dynamic array (copied by nested dimension; when elements hold references, retain each element)
    Value* emitDynArrayFromArray(Value* arrAddr, const TypePtr& arrTy,
                                 const TypePtr& dstTy);

    // ------------------------------------------------------------ weak registry
    GlobalVariable* weakHeadGlobal();
    GlobalVariable* weakLockGlobal();

    void emitWeakLock(IRBuilder<>& hb);

    void emitWeakUnlock(IRBuilder<>& hb);

    Function* ensureWeakReg();

    Function* ensureWeakUnreg();

    Function* ensureWeakNotify();

    // Write a weak field: unregister the old registration first, store the new value, and register it with the target object if it is non-null
    void emitWeakStore(Value* addr, Value* val);

    bool isRefKind(const TypePtr& t);

    // the object pointer inside a reference value (a class handle or an interface value)
    Value* refObj(Value* v, const TypePtr& ty);

    void emitRetainRef(Value* v, const TypePtr& ty);

    void emitReleaseRef(Value* v, const TypePtr& ty);

    // ------------------------------------------------------------ local reference cleanup
    // whether the value's lifetime needs RC cleanup (including recursive struct/array fields)
    static bool needsCleanup(const TypePtr& t);

    void releaseLocal(const LocalVar* lv);

    void pushRefScope();

    void popRefScope();

    // release the references of inner scopes before leaving the current block/loop
    void releaseScopesDeeperThan(size_t depth);

    // owned dynamic array temporaries materialized inside a statement: released at the end of the statement
    void trackOwnedSliceTemp(Expr* e, Value* v, const TypePtr& ty);

    void releaseAllScopes();

    // Release the owned rvalues materialized during statement execution (those registered after mark), keeping reverse order
    void destroyTempsFrom(size_t mark);

    // whether the expression already holds a transferable strong reference (new / a call returning a class reference)
    bool ownsReference(Expr* e);

    // whether the expression result already holds a transferable strong reference (not limited to class handles; also used for reference arrays)
    bool valueOwned(Expr* e);

    // whether the expression result already holds a transferable ownership under the target type.
    // A fixed-size array -> dynamic array conversion allocates a new array in coerce, so it counts as owned too.
    bool convOwns(Expr* e, const TypePtr& dst);

    // Turn a borrow into a new strong reference when returning a class reference
    Value* prepareReturn(Expr* e);

    // ------------------------------------------------------------ allocation
    // model_align: semantic layer alignment (align(N) may exceed the natural alignment of the LLVM type)
    AllocaInst* entryAlloca(LType* ty, const std::string& name, uint64_t model_align = 0);

    // byte offset of a struct member (consistent with the layout rules in types.cpp; 0 for all union members)
    uint64_t memberOffset(const TypePtr& t, size_t idx);

    // Mulan type -> DWARF type. Structs expand their members (going down layer by layer, depth limit 2);
    // pointers only recurse into the element type for "scalar / enum", everything else is shown as void* (to avoid unbounded recursion for self-references).
    DIType* diType(const TypePtr& t, int depth = 0);

    Value* slotFor(const LocalVar* lv);

    // ------------------------------------------------------------ constants
    // type-level / unit-level compile-time constants: emit literal constants from the cached evaluation result (bit width and signedness follow the declared type)
    Constant* constOfValue(const TypePtr& ty, bool is_float, long long iv, double fv);

    Constant* constValue(const TypePtr& ty, Init* init);

    // ------------------------------------------------------------ expressions
    Value* convertTo(Value* v, const TypePtr& from, const TypePtr& to, int line = 0,
                     int col = 0);

    bool isLvalue(Expr* e);

    // whether the address of this expression may fail to satisfy its type's natural alignment (a field/array element on a packed struct chain).
    // Conservative decision: when it cannot be determined, treat it as naturally aligned (in the spec, dereferencing `&p.field` is itself UB).
    bool unalignedExpr(Expr* e);

    // Checked GEP: turn LLVM's assertion errors into readable diagnostics.
    // Note: index 0 is the "pointer stride"; indexing by aggregate type starts at index 1.
    Value* gep(LType* ty, Value* ptr, std::initializer_list<Value*> idxs, int line = 0,
               int col = 0);

    Value* gep(LType* ty, Value* ptr, Value* idx, int line = 0, int col = 0);

    Value* addrOf(Expr* e);

    Value* compareOp(const std::string& op, Value* l, Value* r, bool is_signed, bool is_float);

    Value* coerce(Expr* e, const TypePtr& to);

    Value* evalBinary(Binary* e);

    // Conditional expression: only the selected branch is evaluated; the result is materialized into the entry block's result slot and returned as an owned value.
    Value* evalTernary(Ternary* e);

    // Write the branch value into the result slot: a borrowed source is retained field by field, an owned source transfers ownership directly (consistent with initInto)
    void emitTernaryArm(Value* addr, Expr* src, const TypePtr& ty);


    ArgPack prepareArgs(const std::vector<Expr*>& args, const std::vector<TypePtr>& ptys);

    // C default argument promotion: integers smaller than int -> int, float -> double, everything else unchanged (required by the C varargs ABI)
    Value* promoteVararg(Value* v, const TypePtr& t);

    void releaseTemps(ArgPack& pack);

    // Method call (recv convention: class = handle value; struct = object address)
    Value* emitMethodCallVals(const MethodSig* sig, Value* recv, const TypePtr& recvTy,
                              const std::vector<Value*>& args, Node* at);

    // User-defined operator[] (free function): the container is passed as a class handle / struct address / struct value
    Value* emitOperatorIndexCall(FuncDecl* f, Value* seqAddr, const TypePtr& sty, Value* idxVal,
                                 bool unaligned);

    // Call a free function / operator / method (static dispatch)
    Value* emitUserCall(FuncDecl* f, const std::vector<Expr*>& args);

    // Member operator on a struct (3.11): the receiver is passed as a borrowed pointer (`this`), so
    // `s[i]` / `s == t` cost no copy and no RC.
    Value* emitStructMethodCall(const MethodSig* sig, Value* recvAddr, const std::vector<Expr*>& args);
    Value* emitMemberOp(const MethodSig* sig, Expr* recv, const std::vector<Expr*>& args);
    Value* emitMemberIndex(const MethodSig* sig, Value* recvAddr, const std::vector<Value*>& idxVals);
    Value* emitMemberIndexSet(const MethodSig* sig, Value* recvAddr, Value* idxVal, Expr* value);

    Value* evalNew(NewExpr* ne);

    Value* evalCall(Call* c);

    // whether the block already has a terminator instruction (getTerminator asserts on an empty block)
    static bool hasTerm(BasicBlock* bb);

    // Downcast/upcast of reference types (checks the desc chain at runtime; yields null on failure)
    Value* emitDowncast(Value* src, const TypePtr& from, const TypePtr& to);

    Value* eval(Expr* e);

    // TypeSpec resolution for sizeof/alignof (depends only on the resolved type model)


    // ------------------------------------------------------------ statements
    // Access alignment: fields/elements on a packed struct chain only guarantee 1-byte alignment, so the align of load/store
    // must be explicitly lowered to 1, otherwise LLVM generates incorrect unaligned accesses according to the pointer type's alignment.
    void storeAt(Value* v, Value* addr, bool unaligned = false);

    Value* loadAt(LType* ty, Value* addr, bool unaligned = false);

    void storeValue(Value* v, Value* addr, const TypePtr& ty, bool unaligned = false);

    void zeroMem(Value* addr, const TypePtr& ty, bool unaligned = false);

    void copyMem(Value* dst, Value* src, uint64_t n);

    void initInto(Value* addr, const TypePtr& ty, Init* init, bool unaligned = false);

    void block(Block* blk);

    void stmt(Stmt* s);

    void stmtImpl(Stmt* s);


    void emitFor(ForInStmt* s);

    // switch: value branches use an LLVM switch (jump table), range branches use a comparison chain, and else is the fallback.
    void emitSwitch(SwitchStmt* s);

    // ------------------------------------------------------------ interface table

    static std::string sigKey(const MethodSig& m);

    void buildIfaceSlots();

    const MethodSig* ifaceDefault(const TypePtr& t, const MethodSig& slot, int depth = 0);

    // default implementation on the class/base class interface chain
    const MethodSig* findInterfaceImpl(const TypePtr& cls, const MethodSig& slot);


    // Interface method calling convention: ret(ptr obj, ptr desc, params...)
    Function* ifaceThunk(const TypePtr& cls, const MethodSig& slot, const MethodSig& impl,
                         int idx);

    GlobalVariable* emitItable(const TypePtr& cls);

    // ------------------------------------------------------------ virtual table
    static bool same_sig(const MethodSig& a, const MethodSig& b);


    const std::vector<const MethodSig*>& vtableOf(const TypePtr& cls);

    const MethodSig* findImpl(const TypePtr& cls, const MethodSig& sig);

    int slotOf(const TypePtr& t, const MethodSig* sig);

    FunctionCallee methodFn(const MethodSig* sig);

    GlobalVariable* emitVtable(const TypePtr& cls);

    // ------------------------------------------------------------ class code generation
    void beginBody(Function* fn, const TypePtr& ret = nullptr);

    void finishBody(Function* fn, const std::string& what);

    void bindParams(Function* fn, size_t first,
                    const std::vector<std::unique_ptr<LocalVar>>& pls);

    // Parameters are owning local variables (SPEC §4): one reference is retained on entry and released with the scope when the function exits.
    // The caller still passes parameters by value and releases its own temporaries when needed, so the net reference count does not change.
    // (Borrow/escape analysis can later be done without changing the semantics to optimize away the retain/release pairs.)
    void ownParams(const std::vector<std::unique_ptr<LocalVar>>& pls);

    void emitMethod(ClassDecl* c, MethodDecl* m);

    void emitCtor(ClassDecl* c, CtorDecl* ctor);

    void emitReleaseValue(Value* addr, const TypePtr& ty, bool unaligned = false);

    void emitRetainValue(Value* addr, const TypePtr& ty, bool unaligned = false);

    void emitDtor(ClassDecl* c);

    void emitDestroy(ClassDecl* c);

    void emitClassDesc(ClassDecl* c);

    void emitStructMethod(StructDecl* sd, MethodDecl* m);

    void emitStructCtor(StructDecl* sd, CtorDecl* ctor);

    void emitStructDtor(StructDecl* sd);

    void emitInterfaceMethod(InterfaceDecl* itf, MethodDecl* m);

    // ------------------------------------------------------------ functions
    // The IRBuilder is reused across functions: before entering a new function, clear the stale !dbg location left over from the previous function,
    // otherwise the new function's instructions would point at the previous function's DISubprogram (which the LLVM verifier rejects).
    void resetDebugScope();

    void emitFunction(FuncDecl* f);

    // `para` (SPEC 3.17.2): sections run on OS threads, capture the caller's locals through a frame
    // of pointers, and join before the statement completes.
    void emitPara(ParaStmt* p);
    void emitParaFor(ParaForStmt* p);
    void emitParaSection(PendingPara p);
    void emitParaForBody(PendingParaFor q);
    // One item of a `para for` source: allocate the context, retain the item copy (it is owned by
    // the task body), start the task and record its handle in the region's task list.
    void emitParaForSpawnItem(Value* slot);
    // { frame*, item } — the per-item context type shared by the producer and the task body.
    LStruct* paraCtxTy(const TypePtr& elemTy, bool byRef);
    FunctionCallee runtimeFn(const char* unit, const char* name, LType* ret, std::vector<LType*> params);

    void emitGlobals();

    void initDebug();

    void run();
};

}  // namespace muc_detail
