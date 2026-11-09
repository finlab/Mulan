// 语法分析
//
// 递归下降；优先级层次与 Python 版完全一致（高 → 低）：
//   postfix → unary → * / % → + - → << >> → & → ^ → | → 比较 → == != → not → and → or
// 位运算高于比较；`not` 低于比较、只高于 and/or。赋值是语句，不是表达式。
#include "parser.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common.hpp"
#include "lexer.hpp"

namespace {

// 与 mtypes.PRIM_NAMES | {"void", "string"} 一致（parser.py 的 PRIMS）
const std::unordered_set<std::string>& prims() {
    static const std::unordered_set<std::string> s = {
        "byte",  "char",   "short", "ushort", "int",   "uint",  "long",
        "ulong", "usize", "isize", "float",  "double", "bool",  "void",  "string",
    };
    return s;
}

// ast.ASSIGN_OPS
const std::unordered_set<std::string>& assign_ops() {
    static const std::unordered_set<std::string> s = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    };
    return s;
}

// 一条声明中的多个声明符共享同一个类型语法；深度复制以各持一份所有权的 TypeSpec。
TypeSpecPtr clone_spec(const TypeSpec& s) {
    auto out = std::make_unique<TypeSpec>();
    out->kind = s.kind;
    out->name = s.name;
    out->elem = s.elem ? clone_spec(*s.elem) : nullptr;
    out->size = s.size;
    out->readonly = s.readonly;
    out->weak = s.weak;
    out->line = s.line;
    out->col = s.col;
    return out;
}

TypeSpecPtr make_named_spec(const std::string& name, int line, int col) {
    auto spec = std::make_unique<TypeSpec>();
    spec->kind = TypeSpec::K::Named;
    spec->name = name;
    spec->line = line;
    spec->col = col;
    return spec;
}

// ---------------------------------------------------------------- 语法分析器

class Parser {
public:
    Parser(const std::string& src, const std::string& filename)
        : filename_(filename), toks_(tokenize(src, filename)) {}

    std::unique_ptr<Unit> parse_unit();

private:
    // ------------------------------------------------------ 基础

    const Token& peek(std::size_t k = 0) const {
        std::size_t j = pos_ + k;
        return j < toks_.size() ? toks_[j] : toks_.back();
    }

    bool at_eof() const { return peek().kind == TokKind::Eof; }

    Token advance() {
        Token t = peek();
        if (t.kind != TokKind::Eof) ++pos_;
        return t;
    }

    ParserError err(const std::string& msg) const { return err_at(msg, peek()); }

    ParserError err_at(const std::string& msg, const Token& t) const {
        return ParserError(msg, t.line, t.col, filename_);
    }

    template <class... Args>
    bool at_op(Args&&... ops) const {
        const Token& t = peek();
        if (t.kind != TokKind::Op) return false;
        return ((t.text == ops) || ...);
    }

    template <class... Args>
    bool at_kw(Args&&... kws) const {
        const Token& t = peek();
        if (t.kind != TokKind::Kw) return false;
        return ((t.text == kws) || ...);
    }

    Token expect_op(const std::string& op) {
        if (!at_op(op)) throw err("期望 `" + op + "`，实际是 " + peek().describe());
        return advance();
    }

    Token expect_kw(const std::string& kw) {
        if (!at_kw(kw)) throw err("期望 `" + kw + "`，实际是 " + peek().describe());
        return advance();
    }

    Token expect_ident(const std::string& what = "标识符") {
        if (peek().kind != TokKind::Ident) {
            throw err("期望" + what + "，实际是 " + peek().describe());
        }
        return advance();
    }

    void semicolon() { expect_op(";"); }

    // ------------------------------------------------------ unit

    std::unique_ptr<Decl> parse_extern(const Token& tk);
    std::unique_ptr<Decl> parse_struct();
    std::unique_ptr<Decl> parse_enum();
    std::unique_ptr<Decl> parse_class();
    std::unique_ptr<Decl> parse_interface();
    std::unique_ptr<FuncDecl> parse_operator_func(TypeSpecPtr ret, const Token& tk);
    std::unique_ptr<MethodDecl> parse_method(TypeSpecPtr ret, const Token& name_tok);
    std::unique_ptr<CtorDecl> parse_ctor(const Token& name_tok);
    std::unique_ptr<DtorDecl> parse_dtor();
    std::vector<std::string> parse_base_list();
    std::string parse_qualified_name();
    std::vector<std::unique_ptr<GlobalVar>> parse_global();
    std::unique_ptr<FuncDecl> parse_func(std::string name, TypeSpecPtr ret, const Token& tk);
    std::vector<std::unique_ptr<Param>> parse_params();

    // ------------------------------------------------------ 类型

    bool is_type_start(const Token& t) const;
    TypeSpecPtr parse_type();
    TypeSpecPtr type_suffixes(TypeSpecPtr spec, bool readonly, const Token& t);

    // ------------------------------------------------------ 语句

    std::unique_ptr<Block> parse_block();
    std::vector<StmtPtr> parse_stmt();
    bool looks_like_local_decl();
    std::vector<StmtPtr> parse_local(bool is_const, bool is_auto);
    InitPtr parse_init();
    std::unique_ptr<Stmt> parse_if();
    std::unique_ptr<Stmt> parse_while();
    std::unique_ptr<Stmt> parse_for();

    // ------------------------------------------------------ 表达式

    ExprPtr parse_expr() { return parse_or(); }
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_not();
    ExprPtr parse_eq();
    ExprPtr parse_rel();
    ExprPtr parse_bitor();
    ExprPtr parse_bitxor();
    ExprPtr parse_bitand();
    ExprPtr parse_shift();
    ExprPtr parse_add();
    ExprPtr parse_mul();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_postfix_tail(ExprPtr e);
    ExprPtr parse_call(ExprPtr callee);
    ExprPtr parse_primary();

    ExprPtr make_binary(const std::string& op, ExprPtr left, ExprPtr right, const Token& tok);

    std::string filename_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    int group_counter_ = 0;  // 声明语句分组计数（auto 多声明符一致性检查用）
    // 已声明的本地类型名（struct / enum），用于消解 `T x` 与表达式的歧义
    std::unordered_set<std::string> type_names_;
    // `use` 引入的 unit 名，支持 `math.Point` 这种限定类型
    std::unordered_set<std::string> module_names_;
};

// ---------------------------------------------------------------- unit

std::unique_ptr<Unit> Parser::parse_unit() {
    const Token t = peek();
    if (!at_kw("unit")) throw err("文件必须以 `unit 名字;` 开头");
    advance();
    std::string name = expect_ident("unit 名").text;
    semicolon();

    auto unit = std::make_unique<Unit>();
    unit->name = std::move(name);
    unit->filename = filename_;
    unit->line = t.line;
    unit->col = t.col;

    while (!at_eof()) {
        const Token tk = peek();
        if (at_kw("use")) {
            advance();
            std::string uname = expect_ident("unit 名").text;
            semicolon();
            auto use = std::make_unique<UseDecl>();
            use->unit_name = std::move(uname);
            use->line = tk.line;
            use->col = tk.col;
            module_names_.insert(use->unit_name);
            unit->uses.push_back(std::move(use));
            continue;
        }

        if (at_kw("extern")) {
            advance();
            unit->decls.push_back(parse_extern(tk));
            continue;
        }

        if (at_kw("struct")) {
            unit->decls.push_back(parse_struct());
            continue;
        }

        if (at_kw("enum")) {
            unit->decls.push_back(parse_enum());
            continue;
        }

        if (at_kw("class")) {
            unit->decls.push_back(parse_class());
            continue;
        }

        if (at_kw("interface")) {
            unit->decls.push_back(parse_interface());
            continue;
        }

        if (at_kw("static", "const")) {
            for (auto& g : parse_global()) unit->decls.push_back(std::move(g));
            continue;
        }

        if (at_kw("auto")) throw err("unit 级变量不能使用 `auto`，必须显式写类型");

        // 函数定义或（非法的）unit 级变量
        TypeSpecPtr spec = parse_type();
        if (at_kw("operator")) {
            unit->decls.push_back(parse_operator_func(std::move(spec), tk));
            continue;
        }
        const Token name_tok = expect_ident("声明名");
        if (at_op("(")) {
            unit->decls.push_back(parse_func(name_tok.text, std::move(spec), name_tok));
        } else {
            throw err_at("unit 级变量必须带 `static` 或 `extern`（`const` 常量除外）", name_tok);
        }
    }

    return unit;
}

std::unique_ptr<Decl> Parser::parse_extern(const Token& tk) {
    TypeSpecPtr spec = parse_type();
    const Token name_tok = expect_ident("外部符号名");
    if (at_op("(")) {
        auto fn = std::make_unique<FuncDecl>();
        fn->name = name_tok.text;
        fn->ret = std::move(spec);
        fn->params = parse_params();
        fn->is_extern = true;
        fn->line = tk.line;
        fn->col = tk.col;
        semicolon();
        return fn;
    }
    auto gv = std::make_unique<GlobalVar>();
    gv->name = name_tok.text;
    gv->spec = std::move(spec);
    gv->is_static = false;
    gv->is_extern = true;
    gv->is_const = false;
    gv->line = tk.line;
    gv->col = tk.col;
    semicolon();
    return gv;
}

std::unique_ptr<Decl> Parser::parse_struct() {
    const Token tk = expect_kw("struct");
    std::string name = expect_ident("结构体名").text;
    type_names_.insert(name);
    expect_op("{");

    auto decl = std::make_unique<StructDecl>();
    decl->name = name;
    decl->line = tk.line;
    decl->col = tk.col;

    while (!at_op("}")) {
        if (at_op("~")) {
            if (decl->dtor) throw err("struct `" + name + "` 只能有一个析构器");
            decl->dtor = parse_dtor();
            continue;
        }
        if (peek().kind == TokKind::Ident && peek().text == name && peek(1).is_op("(")) {
            const Token ctor_name = advance();
            decl->ctors.push_back(parse_ctor(ctor_name));
            continue;
        }
        if (at_kw("weak")) {
            throw err("`weak` 只能用于 class 字段");
        }
        TypeSpecPtr spec = parse_type();
        const Token name_tok = expect_ident("字段名或方法名");
        if (at_op("(")) {
            decl->methods.push_back(parse_method(std::move(spec), name_tok));
            continue;
        }
        decl->fields.emplace_back(name_tok.text, clone_spec(*spec));
        for (;;) {
            if (!at_op(",")) break;
            advance();
            std::string fname = expect_ident("字段名").text;
            decl->fields.emplace_back(std::move(fname), clone_spec(*spec));
        }
        semicolon();
    }
    expect_op("}");
    if (at_op(";")) advance();
    return decl;
}

std::unique_ptr<Decl> Parser::parse_enum() {
    const Token tk = expect_kw("enum");
    std::string name = expect_ident("枚举名").text;
    type_names_.insert(name);
    expect_op("{");

    auto decl = std::make_unique<EnumDecl>();
    decl->name = name;
    decl->line = tk.line;
    decl->col = tk.col;

    while (!at_op("}")) {
        std::string member = expect_ident("枚举成员").text;
        std::optional<long long> val;
        if (at_op("=")) {
            advance();
            long long sign = 1;
            if (at_op("-")) {
                advance();
                sign = -1;
            }
            const Token t = peek();
            if (t.kind != TokKind::Int) throw err("枚举值必须是整数字面量");
            advance();
            val = sign * t.ival;
        }
        decl->members.emplace_back(std::move(member), val);
        if (at_op(",")) {
            advance();
        } else {
            break;
        }
    }
    expect_op("}");
    if (at_op(";")) advance();
    return decl;
}

std::string Parser::parse_qualified_name() {
    Token t = expect_ident("类型名");
    std::string n = t.text;
    while (at_op(".")) {
        advance();
        n += ".";
        n += expect_ident("名称").text;
    }
    return n;
}

std::vector<std::string> Parser::parse_base_list() {
    std::vector<std::string> bases;
    if (!at_op(":")) return bases;
    advance();
    for (;;) {
        bases.push_back(parse_qualified_name());
        if (at_op(",")) {
            advance();
            continue;
        }
        break;
    }
    return bases;
}

std::unique_ptr<MethodDecl> Parser::parse_method(TypeSpecPtr ret, const Token& name_tok) {
    auto m = std::make_unique<MethodDecl>();
    m->name = name_tok.text;
    m->ret = std::move(ret);
    m->line = name_tok.line;
    m->col = name_tok.col;
    m->params = parse_params();
    if (at_kw("override")) {
        advance();
        m->is_override = true;
    }
    if (at_op(";")) {
        advance();
    } else {
        m->body = parse_block();
    }
    return m;
}

std::unique_ptr<CtorDecl> Parser::parse_ctor(const Token& name_tok) {
    auto c = std::make_unique<CtorDecl>();
    c->line = name_tok.line;
    c->col = name_tok.col;
    c->params = parse_params();
    c->body = parse_block();
    return c;
}

std::unique_ptr<DtorDecl> Parser::parse_dtor() {
    const Token tk = expect_op("~");
    expect_ident("类名");
    expect_op("(");
    expect_op(")");
    auto d = std::make_unique<DtorDecl>();
    d->line = tk.line;
    d->col = tk.col;
    d->body = parse_block();
    return d;
}

std::unique_ptr<Decl> Parser::parse_class() {
    const Token tk = expect_kw("class");
    const Token nm = expect_ident("类名");
    type_names_.insert(nm.text);

    auto cd = std::make_unique<ClassDecl>();
    cd->name = nm.text;
    cd->line = tk.line;
    cd->col = tk.col;
    cd->bases = parse_base_list();

    expect_op("{");
    while (!at_op("}")) {
        if (at_op("~")) {
            if (cd->dtor) throw err("class `" + cd->name + "` 只能有一个析构器");
            cd->dtor = parse_dtor();
            continue;
        }
        bool prefix_override = false;
        if (at_kw("override")) {
            advance();
            prefix_override = true;
        }
        const Token t = peek();
        if (t.kind == TokKind::Ident && t.text == cd->name && peek(1).is_op("(")) {
            const Token ctor_name = advance();
            cd->ctors.push_back(parse_ctor(ctor_name));
            continue;
        }
        bool prefix_weak = false;
        if (at_kw("weak")) {
            advance();
            prefix_weak = true;
        }
        TypeSpecPtr spec = parse_type();
        if (prefix_weak) spec->weak = true;
        const Token name_tok = expect_ident("成员名");
        if (at_op("(")) {
            if (prefix_weak) throw err_at("`weak` 只能用于字段，不能用于方法", name_tok);
            auto m = parse_method(std::move(spec), name_tok);
            if (prefix_override) m->is_override = true;
            cd->methods.push_back(std::move(m));
            continue;
        }
        if (prefix_override) throw err_at("`override` 只能用于方法", name_tok);
        cd->fields.emplace_back(name_tok.text, clone_spec(*spec));
        while (at_op(",")) {
            advance();
            cd->fields.emplace_back(expect_ident("字段名").text, clone_spec(*spec));
        }
        semicolon();
    }
    expect_op("}");
    if (at_op(";")) advance();
    return cd;
}

std::unique_ptr<Decl> Parser::parse_interface() {
    const Token tk = expect_kw("interface");
    const Token nm = expect_ident("接口名");
    type_names_.insert(nm.text);

    auto id = std::make_unique<InterfaceDecl>();
    id->name = nm.text;
    id->line = tk.line;
    id->col = tk.col;
    id->bases = parse_base_list();

    expect_op("{");
    while (!at_op("}")) {
        TypeSpecPtr spec = parse_type();
        const Token name_tok = expect_ident("方法名");
        id->methods.push_back(parse_method(std::move(spec), name_tok));
    }
    expect_op("}");
    if (at_op(";")) advance();
    return id;
}

std::unique_ptr<FuncDecl> Parser::parse_operator_func(TypeSpecPtr ret, const Token& tk) {
    expect_kw("operator");
    std::string sym;
    if (at_op("[")) {
        advance();
        expect_op("]");
        sym = "[]";
        if (at_op("=")) {
            advance();
            sym = "[]=";
        }
    } else if (peek().kind == TokKind::Op) {
        sym = advance().text;
    } else {
        throw err("`operator` 后缺少运算符");
    }

    auto f = std::make_unique<FuncDecl>();
    f->name = "operator" + sym;
    f->ret = std::move(ret);
    f->line = tk.line;
    f->col = tk.col;
    f->is_operator = true;
    f->params = parse_params();
    f->body = parse_block();
    return f;
}

// ---------------------------------------------------------------- 类型

bool Parser::is_type_start(const Token& t) const {
    if (t.kind == TokKind::Kw) {
        if (prims().count(t.text) != 0) return true;
        if (t.text == "readonly") return true;
        return false;
    }
    if (t.kind == TokKind::Ident) {
        if (type_names_.count(t.text) != 0) return true;
        // 限定类型 `math.Point`
        if (module_names_.count(t.text) != 0 && peek(1).is_op(".")) return true;
    }
    return false;
}

TypeSpecPtr Parser::parse_type() {
    bool readonly = false;
    if (at_kw("readonly")) {
        advance();
        readonly = true;
    }
    const Token t = peek();
    if (t.kind == TokKind::Kw && prims().count(t.text) != 0) {
        advance();
        return type_suffixes(make_named_spec(t.text, t.line, t.col), readonly, t);
    }
    if (t.kind != TokKind::Ident) {
        throw err("期望类型名，实际是 " + t.describe());
    }
    advance();
    std::string name = t.text;
    if (at_op(".")) {
        advance();
        std::string sub = expect_ident("类型名").text;
        name += ".";
        name += sub;
    }
    return type_suffixes(make_named_spec(name, t.line, t.col), readonly, t);
}

TypeSpecPtr Parser::type_suffixes(TypeSpecPtr spec, bool readonly, const Token& t) {
    for (;;) {
        if (at_op("*")) {
            const Token tok = advance();
            auto ptr = std::make_unique<TypeSpec>();
            ptr->kind = TypeSpec::K::Ptr;
            ptr->elem = std::move(spec);
            ptr->line = tok.line;
            ptr->col = tok.col;
            spec = std::move(ptr);
            continue;
        }
        if (at_op("[")) {
            const Token tok = advance();
            if (at_op("]")) {
                advance();
                auto slice = std::make_unique<TypeSpec>();
                slice->kind = TypeSpec::K::Slice;
                slice->elem = std::move(spec);
                slice->line = tok.line;
                slice->col = tok.col;
                spec = std::move(slice);
                continue;
            }
            const Token n = peek();
            if (n.kind != TokKind::Int) throw err("数组长度暂只支持整数字面量");
            advance();
            if (n.ival <= 0) throw err_at("数组长度必须大于 0", n);
            expect_op("]");
            auto arr = std::make_unique<TypeSpec>();
            arr->kind = TypeSpec::K::Array;
            arr->elem = std::move(spec);
            arr->size = static_cast<std::size_t>(n.ival);
            arr->line = tok.line;
            arr->col = tok.col;
            spec = std::move(arr);
            continue;
        }
        break;
    }

    if (readonly) {
        if (spec->kind == TypeSpec::K::Ptr || spec->kind == TypeSpec::K::Slice) {
            spec->readonly = true;
        } else if (spec->kind == TypeSpec::K::Named) {
            throw err_at("`readonly` 只能用于指针或切片类型", t);
        } else {
            throw err_at("`readonly` 只能用于最外层的指针或切片", t);
        }
    }
    return spec;
}

// ---------------------------------------------------------------- 函数

std::vector<std::unique_ptr<Param>> Parser::parse_params() {
    expect_op("(");
    std::vector<std::unique_ptr<Param>> params;
    if (!at_op(")")) {
        for (;;) {
            const Token ptok = peek();
            TypeSpecPtr spec = parse_type();
            std::string pname = expect_ident("参数名").text;
            auto p = std::make_unique<Param>();
            p->name = std::move(pname);
            p->spec = std::move(spec);
            p->line = ptok.line;
            p->col = ptok.col;
            params.push_back(std::move(p));
            if (!at_op(",")) break;
            advance();
        }
    }
    expect_op(")");
    return params;
}

std::unique_ptr<FuncDecl> Parser::parse_func(std::string name, TypeSpecPtr ret, const Token& tk) {
    auto fn = std::make_unique<FuncDecl>();
    fn->name = std::move(name);
    fn->ret = std::move(ret);
    fn->params = parse_params();
    fn->body = parse_block();
    fn->is_extern = false;
    fn->line = tk.line;
    fn->col = tk.col;
    return fn;
}

std::vector<std::unique_ptr<GlobalVar>> Parser::parse_global() {
    bool is_const = false;
    bool is_static = false;
    if (at_kw("const")) {
        advance();
        is_const = true;
    }
    if (at_kw("static")) {
        advance();
        is_static = true;
    }
    TypeSpecPtr spec = parse_type();

    std::vector<std::unique_ptr<GlobalVar>> out;
    for (;;) {
        const Token name_tok = expect_ident("变量名");
        InitPtr init;
        if (at_op("=")) {
            advance();
            init = parse_init();
        }
        auto gv = std::make_unique<GlobalVar>();
        gv->name = name_tok.text;
        gv->spec = clone_spec(*spec);
        gv->init = std::move(init);
        gv->is_static = is_static;
        gv->is_extern = false;
        gv->is_const = is_const;
        gv->line = name_tok.line;
        gv->col = name_tok.col;
        out.push_back(std::move(gv));
        if (!at_op(",")) break;
        advance();
    }
    semicolon();
    return out;
}

// ---------------------------------------------------------------- 语句

std::unique_ptr<Block> Parser::parse_block() {
    const Token tk = expect_op("{");
    auto block = std::make_unique<Block>();
    block->line = tk.line;
    block->col = tk.col;
    while (!at_op("}")) {
        if (at_eof()) throw err_at("代码块没有闭合", tk);
        for (auto& s : parse_stmt()) block->stmts.push_back(std::move(s));
    }
    expect_op("}");
    return block;
}

std::vector<StmtPtr> Parser::parse_stmt() {
    const Token t = peek();
    if (t.kind == TokKind::Op && t.text == "{") {
        std::vector<StmtPtr> out;
        out.push_back(parse_block());
        return out;
    }
    if (t.kind == TokKind::Op && t.text == ";") throw err("不支持空语句 `;`");
    if (at_kw("if")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_if());
        return out;
    }
    if (at_kw("while")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_while());
        return out;
    }
    if (at_kw("for")) {
        std::vector<StmtPtr> out;
        out.push_back(parse_for());
        return out;
    }
    if (at_kw("break")) {
        advance();
        semicolon();
        auto s = std::make_unique<BreakStmt>();
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (at_kw("continue")) {
        advance();
        semicolon();
        auto s = std::make_unique<ContinueStmt>();
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (at_kw("return")) {
        advance();
        ExprPtr value;
        if (!at_op(";")) value = parse_expr();
        semicolon();
        auto s = std::make_unique<ReturnStmt>();
        s->value = std::move(value);
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    if (at_kw("del")) {
        const Token dt = advance();
        auto value = parse_expr();
        semicolon();
        auto d = std::make_unique<DelStmt>();
        d->value = std::move(value);
        d->line = dt.line;
        d->col = dt.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(d));
        return out;
    }
    if (at_kw("static")) {
        throw err("第一阶段不支持 `static` 局部变量");
    }
    if (at_kw("const", "auto")) {
        return parse_local(at_kw("const"), at_kw("auto"));
    }
    // 以类型关键字开头的语句一定是声明：直接按声明解析，让类型错误原样暴露
    if (t.kind == TokKind::Kw && is_type_start(t)) {
        return parse_local(false, false);
    }
    if (looks_like_local_decl()) {
        return parse_local(false, false);
    }

    // `Foo bar;` 但 Foo 不是已知类型：给出更明确的诊断（表达式语句不可能以两个标识符开头）
    if (t.kind == TokKind::Ident && peek(1).kind == TokKind::Ident) {
        throw err_at("`" + t.text + "` 不是已知类型（类型必须先声明后使用）", t);
    }

    ExprPtr expr = parse_expr();
    if (peek().kind == TokKind::Op && assign_ops().count(peek().text) != 0) {
        std::string op = advance().text;
        ExprPtr value = parse_expr();
        semicolon();
        auto s = std::make_unique<AssignStmt>();
        s->target = std::move(expr);
        s->op = std::move(op);
        s->value = std::move(value);
        s->line = t.line;
        s->col = t.col;
        std::vector<StmtPtr> out;
        out.push_back(std::move(s));
        return out;
    }
    semicolon();
    auto s = std::make_unique<ExprStmt>();
    s->expr = std::move(expr);
    s->line = t.line;
    s->col = t.col;
    std::vector<StmtPtr> out;
    out.push_back(std::move(s));
    return out;
}

bool Parser::looks_like_local_decl() {
    const Token t = peek();
    if (!is_type_start(t)) return false;
    const std::size_t save = pos_;
    bool ok = false;
    try {
        parse_type();
        ok = peek().kind == TokKind::Ident;
    } catch (const MuError&) {
        ok = false;
    }
    pos_ = save;
    return ok;
}

std::vector<StmtPtr> Parser::parse_local(bool is_const, bool is_auto) {
    if (at_kw("const")) {
        advance();
        is_const = true;
    }
    if (at_kw("auto")) {
        advance();
        is_auto = true;
    }

    TypeSpecPtr spec;
    if (!is_auto) spec = parse_type();
    const int gid = ++group_counter_;

    std::vector<StmtPtr> out;
    for (;;) {
        const Token name_tok = expect_ident("变量名");
        InitPtr init;
        if (at_op("=")) {
            advance();
            init = parse_init();
        }
        if (is_auto && !init) throw err_at("`auto` 声明必须有初始化器", name_tok);
        auto lv = std::make_unique<LocalVar>();
        lv->name = name_tok.text;
        lv->spec = spec ? clone_spec(*spec) : nullptr;
        lv->init = std::move(init);
        lv->is_const = is_const;
        lv->is_auto = is_auto;
        lv->group = gid;
        lv->line = name_tok.line;
        lv->col = name_tok.col;
        out.push_back(std::move(lv));
        if (!at_op(",")) break;
        advance();
    }
    semicolon();
    return out;
}

InitPtr Parser::parse_init() {
    const Token t = peek();
    if (at_op("{")) {
        advance();
        auto agg = std::make_unique<InitAggregate>();
        agg->line = t.line;
        agg->col = t.col;
        while (!at_op("}")) {
            if (peek().kind == TokKind::Ident && peek(1).is_op("=")) {
                std::string fname = advance().text;
                advance();  // '='
                agg->items.emplace_back(std::move(fname), parse_init());
            } else {
                agg->items.emplace_back(std::string(), parse_init());
            }
            if (at_op(",")) {
                advance();
            } else {
                break;
            }
        }
        expect_op("}");
        return agg;
    }
    auto ie = std::make_unique<InitExpr>();
    ie->expr = parse_expr();
    ie->line = t.line;
    ie->col = t.col;
    return ie;
}

std::unique_ptr<Stmt> Parser::parse_if() {
    const Token tk = expect_kw("if");
    ExprPtr cond = parse_expr();
    std::unique_ptr<Block> then = parse_block();
    StmtPtr els;
    if (at_kw("elif")) {
        advance();
        // 复用 if 的解析：把刚消费的 `elif` 改写成 `if`
        --pos_;
        Token if_tok;
        if_tok.kind = TokKind::Kw;
        if_tok.text = "if";
        if_tok.line = tk.line;
        if_tok.col = tk.col;
        toks_[pos_] = std::move(if_tok);
        els = parse_if();
    } else if (at_kw("else")) {
        advance();
        if (at_kw("if")) {
            els = parse_if();
        } else {
            els = parse_block();
        }
    }
    auto node = std::make_unique<IfStmt>();
    node->cond = std::move(cond);
    node->then = std::move(then);
    node->els = std::move(els);
    node->line = tk.line;
    node->col = tk.col;
    return node;
}

std::unique_ptr<Stmt> Parser::parse_while() {
    const Token tk = expect_kw("while");
    ExprPtr cond = parse_expr();
    std::unique_ptr<Block> body = parse_block();
    auto node = std::make_unique<WhileStmt>();
    node->cond = std::move(cond);
    node->body = std::move(body);
    node->line = tk.line;
    node->col = tk.col;
    return node;
}

std::unique_ptr<Stmt> Parser::parse_for() {
    const Token tk = expect_kw("for");
    bool paren = false;
    if (at_op("(")) {
        advance();
        paren = true;
    }
    bool by_ref = false;
    if (at_kw("ref")) {
        advance();
        by_ref = true;
    }
    if (peek().kind != TokKind::Ident) throw err("for-in 需要一个循环变量名");
    std::string var = advance().text;
    expect_kw("in");
    ExprPtr seq = parse_expr();
    if (at_op("..")) {
        advance();
        ExprPtr hi = parse_expr();
        auto range = std::make_unique<RangeExpr>();
        range->lo = std::move(seq);
        range->hi = std::move(hi);
        range->line = tk.line;
        range->col = tk.col;
        seq = std::move(range);
    }
    if (paren) expect_op(")");
    std::unique_ptr<Block> body = parse_block();
    auto node = std::make_unique<ForInStmt>();
    node->by_ref = by_ref;
    node->var = std::move(var);
    node->seq = std::move(seq);
    node->body = std::move(body);
    node->line = tk.line;
    node->col = tk.col;
    return node;
}

// ---------------------------------------------------------------- 表达式

ExprPtr Parser::make_binary(const std::string& op, ExprPtr left, ExprPtr right,
                            const Token& tok) {
    auto node = std::make_unique<Binary>();
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    node->line = tok.line;
    node->col = tok.col;
    return node;
}

ExprPtr Parser::parse_or() {
    ExprPtr left = parse_and();
    while (at_kw("or")) {
        const Token tok = advance();
        ExprPtr right = parse_and();
        left = make_binary("or", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_and() {
    ExprPtr left = parse_not();
    while (at_kw("and")) {
        const Token tok = advance();
        ExprPtr right = parse_not();
        left = make_binary("and", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_not() {
    if (at_kw("not")) {
        const Token tok = advance();
        ExprPtr operand = parse_not();
        auto node = std::make_unique<NotExpr>();
        node->operand = std::move(operand);
        node->line = tok.line;
        node->col = tok.col;
        return node;
    }
    return parse_eq();
}

ExprPtr Parser::parse_eq() {
    ExprPtr left = parse_rel();
    while (at_op("==", "!=")) {
        const Token tok = advance();
        ExprPtr right = parse_rel();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_rel() {
    ExprPtr left = parse_bitor();
    while (at_op("<", "<=", ">", ">=")) {
        const Token tok = advance();
        ExprPtr right = parse_bitor();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitor() {
    ExprPtr left = parse_bitxor();
    while (at_op("|")) {
        const Token tok = advance();
        ExprPtr right = parse_bitxor();
        left = make_binary("|", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitxor() {
    ExprPtr left = parse_bitand();
    while (at_op("^")) {
        const Token tok = advance();
        ExprPtr right = parse_bitand();
        left = make_binary("^", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitand() {
    ExprPtr left = parse_shift();
    while (at_op("&")) {
        const Token tok = advance();
        ExprPtr right = parse_shift();
        left = make_binary("&", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_shift() {
    ExprPtr left = parse_add();
    while (at_op("<<", ">>")) {
        const Token tok = advance();
        ExprPtr right = parse_add();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_add() {
    ExprPtr left = parse_mul();
    while (at_op("+", "-")) {
        const Token tok = advance();
        ExprPtr right = parse_mul();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_mul() {
    ExprPtr left = parse_unary();
    while (at_op("*", "/", "%")) {
        const Token tok = advance();
        ExprPtr right = parse_unary();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_unary() {
    const Token t = peek();
    if (at_kw("new")) {
        advance();
        auto ne = std::make_unique<NewExpr>();
        ne->type = parse_type();
        ne->line = t.line;
        ne->col = t.col;
        if (at_op("(")) {
            advance();
            while (!at_op(")")) {
                if (peek().kind == TokKind::Ident && peek(1).is_op("=")) {
                    std::string nm = advance().text;
                    advance();
                    ne->named.emplace_back(std::move(nm), parse_expr());
                } else {
                    ne->args.push_back(parse_expr());
                }
                if (at_op(",")) {
                    advance();
                } else {
                    break;
                }
            }
            expect_op(")");
            if (!ne->args.empty() && !ne->named.empty())
                throw err_at("位置参数与命名参数不能混用", t);
        }
        return parse_postfix_tail(std::move(ne));
    }
    if (t.kind == TokKind::Op && at_op("-", "+", "&", "*", "!")) {
        advance();
        ExprPtr operand = parse_unary();
        auto node = std::make_unique<Unary>();
        node->op = t.text;
        node->operand = std::move(operand);
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    if (at_kw("sizeof", "alignof")) {
        std::string which = advance().text;
        expect_op("(");
        TypeSpecPtr spec = parse_type();
        expect_op(")");
        auto node = std::make_unique<SizeofExpr>();
        node->spec = std::move(spec);
        node->which = std::move(which);
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    if (at_kw("len")) {
        advance();
        expect_op("(");
        ExprPtr seq = parse_expr();
        expect_op(")");
        auto node = std::make_unique<Len>();
        node->seq = std::move(seq);
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    return parse_postfix_tail(parse_primary());
}

ExprPtr Parser::parse_postfix_tail(ExprPtr e) {
    for (;;) {
        const Token t = peek();
        if (t.is_op("(")) {
            e = parse_call(std::move(e));
            continue;
        }
        if (t.is_op("[")) {
            advance();
            ExprPtr idx = parse_expr();
            expect_op("]");
            auto node = std::make_unique<Index>();
            node->base = std::move(e);
            node->index = std::move(idx);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        if (t.is_op(".")) {
            advance();
            std::string nm = expect_ident("成员名").text;
            auto node = std::make_unique<Member>();
            node->obj = std::move(e);
            node->name = std::move(nm);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        if (t.is_kw("as")) {
            advance();
            TypeSpecPtr spec = parse_type();
            auto node = std::make_unique<Cast>();
            node->expr = std::move(e);
            node->spec = std::move(spec);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        break;
    }
    return e;
}

ExprPtr Parser::parse_call(ExprPtr callee) {
    const Token t = expect_op("(");
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    while (!at_op(")")) {
        if (peek().kind == TokKind::Ident && peek(1).is_op("=")) {
            std::string nm = advance().text;
            advance();
            named.emplace_back(std::move(nm), parse_expr());
        } else {
            args.push_back(parse_expr());
        }
        if (at_op(",")) {
            advance();
        } else {
            break;
        }
    }
    expect_op(")");
    if (!args.empty() && !named.empty()) throw err_at("位置参数与命名参数不能混用", t);
    auto node = std::make_unique<Call>();
    node->callee = std::move(callee);
    node->args = std::move(args);
    node->named = std::move(named);
    node->line = t.line;
    node->col = t.col;
    return node;
}

ExprPtr Parser::parse_primary() {
    const Token t = peek();
    if (t.kind == TokKind::Kw && (t.text == "this" || t.text == "super")) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Int) {
        advance();
        auto node = std::make_unique<IntLit>();
        node->value = t.ival;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Float) {
        advance();
        auto node = std::make_unique<FloatLit>();
        node->value = t.fval;
        node->is_float32 = t.f32;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Char) {
        advance();
        auto node = std::make_unique<CharLit>();
        node->value = static_cast<int>(t.ival);
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Str) {
        advance();
        auto node = std::make_unique<StringLit>();
        node->value = t.bytes;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && (t.text == "true" || t.text == "false")) {
        advance();
        auto node = std::make_unique<BoolLit>();
        node->value = t.text == "true";
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && t.text == "null") {
        advance();
        auto node = std::make_unique<NullLit>();
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && (t.text == "alloc" || t.text == "realloc" || t.text == "free")) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Ident) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Op && t.text == "(") {
        advance();
        ExprPtr e = parse_expr();
        expect_op(")");
        return e;
    }
    throw err("意外的 " + t.describe());
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

std::unique_ptr<Unit> parse_source(const std::string& src, const std::string& filename) {
    Parser parser(src, filename);
    return parser.parse_unit();
}

std::unique_ptr<Unit> parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw MuError("无法读写文件 " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_source(ss.str(), path);
}
