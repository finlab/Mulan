// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// LLVM 后端内部实现：Emitter 类声明与跨文件辅助函数声明（仅供 codegen_*.cpp 使用）
//
// 成员函数按功能分散在 codegen_support / codegen_runtime / codegen_expr / codegen_expr2 /
// codegen_stmt / codegen_class / codegen_func 中；对外接口见 codegen_llvm.hpp。
#pragma once

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

// ------------------------------------------------------------ 跨文件辅助

// 把任意字符串变成合法的符号片段（定义在 codegen_support.cpp）
std::string sane(std::string s);

// 初始化表达式是否是字面量 0（定义在 codegen_support.cpp）
bool is_zero_literal(Init* init);

bool is_void(const TypePtr& t);

// ------------------------------------------------------------ Emitter

struct Emitter {
    explicit Emitter(Program& p, bool dbg = false);

    Program& prog;
    bool wantDebug = false;
    LLVMContext ctx;
    std::unique_ptr<Module> mod;
    std::unique_ptr<TargetMachine> tm;
    IRBuilder<> b{ctx};
    IRBuilder<> ab{ctx};

    TypePtr strMeta;  // std.string.string 的语义类型（惰性查找）

    // 调试信息（DWARF）：编译单元 / 文件 / 当前函数的 subprogram
    std::unique_ptr<DIBuilder> dib;
    DICompileUnit* diCu = nullptr;
    DIFile* diFile = nullptr;
    DISubprogram* diSub = nullptr;    LType* sliceTy = nullptr;  // 动态数组句柄
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


    // class 对象布局（模型）：{ i64 rc, ptr desc } 固定 16 字节，字段从 16 开始按 type_align 排列。
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
    // ------------------------------------------------------------ 基础工具
    LType* ptrTy();
    LType* i1();
    LType* i8();
    LType* i32();
    LType* i64();

    [[noreturn]] void fail(const std::string& m, int line = 0, int col = 0);

    LType* primTy(const std::string& n);

    LStruct* structOf(const TypePtr& t);

    // 模型字段索引 -> LLVM 字段索引（align(N) 会在字段间插入显式 [k x i8] 填充）
    unsigned llvmFieldIndex(const TypePtr& t, int model_idx);

    LStruct* arrayOf(const TypePtr& t);

    static uint64_t alignUp(uint64_t v, uint64_t a);


    uint64_t classObjAlign(const TypePtr& cls);

    uint64_t classFieldOffset(const TypePtr& cls, int idx);

    uint64_t classObjSize(const TypePtr& cls);

    // 对象布局：{ i64 rc, ptr desc, [填充], fields... }（fields 已在 sema 扁平化，基类字段在前）
    // 与 structOf 同样插入显式 [k x i8] 填充，保证 LLVM 偏移与语义层 type_align/type_size 一致。
    LStruct* objStruct(const TypePtr& cls);


    uint64_t objectSize(const TypePtr& cls);

    uint64_t objectAlign(const TypePtr& cls);

    // align(N) > 16 的 class 对象要显式对齐分配（malloc 只保证 16 字节）；释放时必须配对
    bool classNeedsAlignedAlloc(const TypePtr& cls);

    // 字段地址：union 的所有成员落在偏移 0（LLVM 类型是「载体 + 填充」，字段 0 即基址）
    Value* fieldSlot(LStruct* st, const TypePtr& ty, Value* base, int idx);

    Value* fieldAddr(Value* objPtr, const TypePtr& cls, int field_index);

    Value* descOf(const TypePtr& cls);

    // std.string.string：编译器识别的规范字符串结构体（字段 p/n/owner）
    TypePtr strType();

    bool isStrType(const TypePtr& t);

    LType* lt(const TypePtr& t);

    // ------------------------------------------------------------ 布局
    // 唯一实现在 types.cpp（type_align/type_size），这里只做转接，避免两份布局漂移。
    uint64_t alignOf(const TypePtr& t);
    uint64_t sizeOf(const TypePtr& t);

    bool zeroable(const TypePtr& t);

    // ------------------------------------------------------------ 函数/全局
    FunctionCallee libFn(const char* name, LType* ret, std::vector<LType*> params);

    Constant* zeroConst(LType* ty, const TypePtr& mt);

    Value* stringConst(const std::string& bytes);

    // ------------------------------------------------------------ 引用计数运行时
    Function* ensureRetain();

    Function* ensureRelease();

    void emitRetain(Value* p);

    void emitRelease(Value* p);

    // ------------------------------------------------------------ 动态数组（T[] / T[,]）
    // rank 1：{ i64 rc; i64 len; i64 cap; data... }
    // rank r≥2：{ i64 rc; i64 d0; ... d(r-1); data... }

    static uint64_t sliceDataOffset(int rank);

    Value* sliceDataPtr(Value* h, int rank);

    Value* sliceDimField(Value* h, int axis);

    // 元素总数（rank1 用 len，rank≥2 用各维乘积）
    Value* sliceTotalCount(Value* h, int rank);

    // len(a, k)：null 句柄为 0；k 为编译期常量
    Value* emitSliceDim(Value* h, int axis);

    // len(a, k)：k 为运行期表达式（带越界检查）
    Value* emitSliceDimDynamic(Value* h, Value* axis, int rank);

    // 元素总数（null 句柄为 0）
    Value* emitSliceTotal(Value* h, int rank);

    // 分配动态数组并清零；dims 为各维尺寸
    Value* emitSliceAlloc(const std::vector<Value*>& dims, const TypePtr& elem);

    Value* emitDynArrayAlloc(Value* n, const TypePtr& elem);

    void emitSliceRetain(Value* h);

    // 元素释放函数：逐一 release 元素（如有引用），然后 free 头部
    Function* ensureSliceFree(const TypePtr& elem, int rank);

    void emitSliceRelease(Value* h, const TypePtr& sliceTy);

    // 定长数组 -> 动态数组（按嵌套维度复制；元素含引用时逐元素 retain）
    Value* emitDynArrayFromArray(Value* arrAddr, const TypePtr& arrTy,
                                 const TypePtr& dstTy);

    // ------------------------------------------------------------ weak 注册表
    GlobalVariable* weakHeadGlobal();
    GlobalVariable* weakLockGlobal();

    void emitWeakLock(IRBuilder<>& hb);

    void emitWeakUnlock(IRBuilder<>& hb);

    Function* ensureWeakReg();

    Function* ensureWeakUnreg();

    Function* ensureWeakNotify();

    // 写 weak 字段：先注销旧登记，存储新值，非空则登记到目标对象
    void emitWeakStore(Value* addr, Value* val);

    bool isRefKind(const TypePtr& t);

    // 引用值（class 句柄或 interface 值）里的对象指针
    Value* refObj(Value* v, const TypePtr& ty);

    void emitRetainRef(Value* v, const TypePtr& ty);

    void emitReleaseRef(Value* v, const TypePtr& ty);

    // ------------------------------------------------------------ 局部引用清理
    // 值的生命周期里是否需要 RC 清理（含递归的 struct/array 字段）
    static bool needsCleanup(const TypePtr& t);

    void releaseLocal(const LocalVar* lv);

    void pushRefScope();

    void popRefScope();

    // 跳出当前块/循环前释放内层作用域的引用
    void releaseScopesDeeperThan(size_t depth);

    // 语句内物化的 owned 动态数组临时值：语句结束时释放
    void trackOwnedSliceTemp(Expr* e, Value* v, const TypePtr& ty);

    // owned 的 Str 右值：物化到语句临时并在语句结束释放 owner
    Value* evalStrValue(Expr* e);

    void releaseAllScopes();

    // 释放语句执行期间物化的 owned 右值（从 mark 之后注册的），保持逆序
    void destroyTempsFrom(size_t mark);

    // 表达式是否已经持有一份可转移的强引用（new / 返回 class 引用的调用）
    bool ownsReference(Expr* e);

    // 表达式结果是否已持有一份可转移的强引用（不限于 class 句柄，也用于引用数组）
    bool valueOwned(Expr* e);

    // 表达式结果在目标类型下是否已持有一份可转移的所有权。
    // 定长数组 -> 动态数组的转换会在 coerce 里新建数组，因此也算 owned。
    bool convOwns(Expr* e, const TypePtr& dst);

    // 返回 class 引用时把借用变成一份新的强引用
    Value* prepareReturn(Expr* e);

    // ------------------------------------------------------------ 分配
    // model_align：语义层对齐（align(N) 可能大于 LLVM 类型的自然对齐）
    AllocaInst* entryAlloca(LType* ty, const std::string& name, uint64_t model_align = 0);

    // 结构体成员的字节偏移（与 types.cpp 的布局规则一致；union 全部 0）
    uint64_t memberOffset(const TypePtr& t, size_t idx);

    // Mulan 类型 -> DWARF 类型。结构体展开成员（一层层往下，深度上限 2）；
    // 指针只对「标量 / 枚举」递归元素类型，其余显示为 void*（避免自引用无界递归）。
    DIType* diType(const TypePtr& t, int depth = 0);

    AllocaInst* slotFor(const LocalVar* lv);

    // ------------------------------------------------------------ 常量
    // 类型级 / unit 级编译期常量：按缓存的求值结果发字面常量（位宽与符号按声明类型）
    Constant* constOfValue(const TypePtr& ty, bool is_float, long long iv, double fv);

    Constant* constValue(const TypePtr& ty, Init* init);

    // ------------------------------------------------------------ 表达式
    Value* convertTo(Value* v, const TypePtr& from, const TypePtr& to, int line = 0,
                     int col = 0);

    bool isLvalue(Expr* e);

    // 该表达式的地址是否可能未满足其类型的自然对齐（packed 结构链上的字段/数组元素）。
    // 保守判定：取不到时按自然对齐处理（规范里 `&p.field` 的解引用本身就是 UB）。
    bool unalignedExpr(Expr* e);

    // 带校验的 GEP：把 LLVM 的断言错误变成可读的诊断。
    // 注意：第 0 个下标是"指针步进"，从第 1 个下标起才按聚合类型逐层索引。
    Value* gep(LType* ty, Value* ptr, std::initializer_list<Value*> idxs, int line = 0,
               int col = 0);

    Value* gep(LType* ty, Value* ptr, Value* idx, int line = 0, int col = 0);

    Value* addrOf(Expr* e);

    Value* compareOp(const std::string& op, Value* l, Value* r, bool is_signed, bool is_float);

    Value* coerce(Expr* e, const TypePtr& to);

    Value* evalBinary(Binary* e);

    // 条件表达式：只求值被选中的分支，结果物化到 entry 块的结果槽后按 owned 值返回。
    Value* evalTernary(Ternary* e);

    // 把分支值写入结果槽：借用源逐字段 retain，owned 源直接转移所有权（与 initInto 一致）
    void emitTernaryArm(Value* addr, Expr* src, const TypePtr& ty);


    ArgPack prepareArgs(const std::vector<Expr*>& args, const std::vector<TypePtr>& ptys);

    // C 默认实参提升：比 int 小的整型 → int，float → double，其余原样（C varargs ABI 要求）
    Value* promoteVararg(Value* v, const TypePtr& t);

    void releaseTemps(ArgPack& pack);

    // 方法调用（recv 约定：class = 句柄值；struct = 对象地址）
    Value* emitMethodCallVals(const MethodSig* sig, Value* recv, const TypePtr& recvTy,
                              const std::vector<Value*>& args, Node* at);

    // 用户 operator[]（自由函数）：容器按 class 句柄 / struct 地址 / struct 值传参
    Value* emitOperatorIndexCall(FuncDecl* f, Value* seqAddr, const TypePtr& sty, Value* idxVal,
                                 bool unaligned);

    // 调用自由函数/运算符/方法（静态派发）
    Value* emitUserCall(FuncDecl* f, const std::vector<Expr*>& args);

    Value* evalNew(NewExpr* ne);

    Value* evalCall(Call* c);

    // 块是否已有终结指令（getTerminator 在空块上会断言）
    static bool hasTerm(BasicBlock* bb);

    // 引用类型向下/向上转换（运行期检查 desc 链；失败得 null）
    Value* emitDowncast(Value* src, const TypePtr& from, const TypePtr& to);

    Value* eval(Expr* e);

    // sizeof/alignof 的 TypeSpec 解析（只依赖已解析的类型模型）
    TypePtr specType(TypeSpec* spec);
    void curSpecUnitFix();

    // ------------------------------------------------------------ 语句
    // 访问对齐：packed 结构链上的字段/元素只保证 1 字节对齐，必须把 load/store
    // 的 align 显式降到 1，否则 LLVM 会按脚点类型对齐生成错误的非对齐访问。
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

    // switch：值分支走 LLVM switch（跳表），区间分支走比较链，else 兜底。
    void emitSwitch(SwitchStmt* s);

    // ------------------------------------------------------------ 接口表

    static std::string sigKey(const MethodSig& m);

    void buildIfaceSlots();

    const MethodSig* ifaceDefault(const TypePtr& t, const MethodSig& slot, int depth = 0);

    // 类/基类接口链上的默认实现
    const MethodSig* findInterfaceImpl(const TypePtr& cls, const MethodSig& slot);


    // 接口方法调用约定：ret(ptr obj, ptr desc, params...)
    Function* ifaceThunk(const TypePtr& cls, const MethodSig& slot, const MethodSig& impl,
                         int idx);

    GlobalVariable* emitItable(const TypePtr& cls);

    // ------------------------------------------------------------ 虚表
    static bool same_sig(const MethodSig& a, const MethodSig& b);


    const std::vector<const MethodSig*>& vtableOf(const TypePtr& cls);

    const MethodSig* findImpl(const TypePtr& cls, const MethodSig& sig);

    int slotOf(const TypePtr& t, const MethodSig* sig);

    FunctionCallee methodFn(const MethodSig* sig);

    GlobalVariable* emitVtable(const TypePtr& cls);

    // ------------------------------------------------------------ class 代码生成
    void beginBody(Function* fn, const TypePtr& ret = nullptr);

    void finishBody(Function* fn, const std::string& what);

    void bindParams(Function* fn, size_t first,
                    const std::vector<std::unique_ptr<LocalVar>>& pls);

    // 参数是拥有型局部变量（SPEC §4）：进入时 retain 一份，函数退出时随作用域 release。
    // 调用方仍按值传参并在需要时释放自己的临时值，因此净引用计数不变。
    // （后续可在不改变语义的前提下做借用/逃逸分析，把 retain/release 优化掉。）
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

    // ------------------------------------------------------------ 函数
    // IRBuilder 跨函数复用：每次进入新函数前清掉上一个函数残留的 !dbg 位置，
    // 否则新函数的指令会指向旧函数的 DISubprogram（LLVM verifier 会拒绝）。
    void resetDebugScope();

    void emitFunction(FuncDecl* f);

    void emitGlobals();

    void initDebug();

    void run();
};

}  // namespace muc_detail
