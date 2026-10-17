/**
 * @file rust_parse.cpp
 * @brief RustView 的节点池、文法动作与解析入口（由 `rust_subset.gram` 的 `%epilogue` 内嵌）。
 *
 * 这个文件**不单独编译**：`ParseEngine` 生成 `parser.cpp` 时把它整段贴进产物末尾，
 * 于是「词法 + 语法 + 建树 + `Engine::runText` 的驱动」是同一份生成结果。
 * `src/` 目录里只有分析器、可视化器和命令行，没有任何手写的解析代码。
 *
 * 数据流：
 *   源码文本 --(生成的词法/语法)--> 产生式动作 rv_*
 *            --(本文件的节点池)--> Node 树 --(parseText 交出所有权)--> Analyzer
 */
#include "parser.hpp"
#include "rust_syntax.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace rustview {
namespace {

/**
 * @brief 词法单元占位节点：`name` 存原文，`number` 存数值，`line` 存行号。
 */
std::vector<NodePtr> g_pool;  ///< 节点池：下标就是文法里的语义值
int g_lastLine = 1;           ///< 最近一个词法单元的行号（给空产生式兜底）

/**
 * @brief 把一个节点放进池子。
 * @param node 新节点（所有权移交给池子，允许为空指针）。
 * @return 池子下标；`node` 为空时返回 -1。
 */
int push(NodePtr node) {
    if (!node) return -1;
    g_pool.push_back(std::move(node));
    return static_cast<int>(g_pool.size()) - 1;
}

/**
 * @brief 取出池子里的节点（所有权转移），并把槽位置空。
 * @param handle 池子下标，越界或已取走时安全返回空指针。
 * @return 节点所有权；拿不到就返回 `nullptr`。
 *
 * 「取走」而不是「共享」是刻意的：每个语义值在文法里只被父节点消费一次，
 * 取走的同时置空可以顺带防止「同一个子节点被挂到两棵树上」这种低级错误。
 */
NodePtr take(int handle) {
    if (handle < 0 || handle >= static_cast<int>(g_pool.size())) return nullptr;
    return std::move(g_pool[static_cast<std::size_t>(handle)]);
}

/** @brief 只看不取（读名字 / 行号用），越界返回空指针。 */
Node *peek(int handle) {
    if (handle < 0 || handle >= static_cast<int>(g_pool.size())) return nullptr;
    return g_pool[static_cast<std::size_t>(handle)].get();
}

/** @brief 造一个只有种类和行号的节点并入池。 */
int make(NodeKind kind, int line) {
    auto node = std::make_unique<Node>();
    node->kind = kind;
    node->line = line > 0 ? line : g_lastLine;
    return push(std::move(node));
}

/** @brief 读句柄的行号，越界回落到最近一个词法单元。 */
int lineOf(int handle) {
    Node *node = peek(handle);
    return node ? node->line : g_lastLine;
}

/** @brief 把列表节点的元素整体取走（列表本身被丢弃）。 */
std::vector<NodePtr> takeList(int handle) {
    std::vector<NodePtr> out;
    NodePtr list = take(handle);
    if (list && list->kind == NodeKind::List) out = std::move(list->items);
    return out;
}

/** @brief 去掉字符串字面量两端引号，并还原最常见的转义。 */
std::string unquote(const std::string &raw) {
    if (raw.size() < 2) return raw;
    const char first = raw.front();
    if ((first != '"' && first != '\'') || raw.back() != first) return raw;
    std::string out;
    out.reserve(raw.size() - 2);
    for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 2 < raw.size()) {
            const char next = raw[++i];
            switch (next) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back('\0'); break;
            default: out.push_back(next); break;
            }
            continue;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

/* ------------------------------------------------------------------ *
 * 词法单元 / 建树动作：全部是「池子操作」，没有业务逻辑
 * ------------------------------------------------------------------ */

std::string rv_text(int handle) {
    Node *node = peek(handle);
    return node ? node->name : std::string();
}

int rv_line(int handle) { return lineOf(handle); }

int rv_token_value(std::string_view text, double number, int line) {
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Tok;
    node->name.assign(text);
    node->number = number;
    node->line = line > 0 ? line : g_lastLine;
    g_lastLine = node->line;
    return push(std::move(node));
}

int rv_list() { return make(NodeKind::List, g_lastLine); }

void rv_list_push(int list, int item) {
    NodePtr element = take(item);
    Node *listNode = peek(list);
    if (listNode && element) listNode->items.push_back(std::move(element));
}

int rv_none() { return -1; }

int rv_program(int items) {
    const int handle = make(NodeKind::Program, g_lastLine);
    peek(handle)->items = takeList(items);
    return handle;
}

int rv_struct(int name, int fields) {
    const int handle = make(NodeKind::StructDecl, lineOf(name));
    Node *node = peek(handle);
    node->name = rv_text(name);
    node->fields = takeList(fields);
    return handle;
}

void rv_struct_field(int list, int name, int type) {
    const int handle = make(NodeKind::StructField, lineOf(name));
    Node *node = peek(handle);
    node->name = rv_text(name);
    node->type = take(type);
    Node *listNode = peek(list);
    if (listNode) listNode->fields.push_back(take(handle));
}

int rv_fn(int name, int lifetimes, int params, int ret, int body) {
    const int handle = make(NodeKind::FnDecl, lineOf(name));
    Node *node = peek(handle);
    node->name = rv_text(name);
    node->lifetimes = takeList(lifetimes);  // 没有泛型参数表时 take(-1) 得到空表
    node->params = takeList(params);
    node->ret = take(ret);
    node->body = take(body);
    return handle;
}

void rv_param(int list, int name, int type) {
    const int handle = make(NodeKind::Param, lineOf(name));
    Node *node = peek(handle);
    node->name = rv_text(name);
    node->type = take(type);
    if (node->type) node->isMut = node->type->isRef && node->type->isMut;
    Node *listNode = peek(list);
    if (listNode) listNode->items.push_back(take(handle));
}

int rv_type(int name, bool isRef, bool isMut) {
    const int handle = make(NodeKind::Type, lineOf(name));
    Node *node = peek(handle);
    node->name = rv_text(name);
    node->isRef = isRef;
    node->isMut = isMut;
    return handle;
}

int rv_lifetime_type(int lifetime, int name, bool isRef, bool isMut) {
    const int handle = rv_type(name, isRef, isMut);
    Node *node = peek(handle);
    // LIFETIME 这个 Token 的原文本身就带撇号（`'a`），这里直接存，画图和报错时不必再补。
    if (node) node->lifetime = rv_text(lifetime);
    return handle;
}

int rv_lifetime_decl(int lifetime, int constraint) {
    const int handle = make(NodeKind::LifetimeDecl, lineOf(lifetime));
    Node *node = peek(handle);
    node->name = rv_text(lifetime);
    node->base = take(constraint);  // 有约束 `'a: 'b` 时，存的是 `'b` 这个 VarRef 节点
    return handle;
}

void rv_lifetime_param(int list, int decl) {
    rv_list_push(list, decl);
}

int rv_block(int stmts, int tail) {
    const int handle = make(NodeKind::Block, g_lastLine);
    Node *node = peek(handle);
    node->stmts = takeList(stmts);
    node->tail = take(tail);
    return handle;
}

int rv_let(int name, bool isMut, int init) {
    const int handle = make(NodeKind::Let, lineOf(name));
    Node *node = peek(handle);
    node->name = rv_text(name);
    node->isMut = isMut;
    node->init = take(init);
    return handle;
}

int rv_print(int expr) {
    const int handle = make(NodeKind::Print, lineOf(expr));
    peek(handle)->init = take(expr);
    return handle;
}

int rv_expr_stmt(int expr) {
    const int handle = make(NodeKind::ExprStmt, lineOf(expr));
    peek(handle)->init = take(expr);
    return handle;
}

int rv_int_lit(int tok) {
    const int handle = make(NodeKind::IntLit, lineOf(tok));
    Node *node = peek(handle);
    node->name = rv_text(tok);
    node->number = node->name.empty() ? 0.0 : std::strtod(node->name.c_str(), nullptr);
    return handle;
}

int rv_str_lit(int tok) {
    const int handle = make(NodeKind::StrLit, lineOf(tok));
    peek(handle)->name = unquote(rv_text(tok));
    return handle;
}

int rv_var(int tok) {
    const int handle = make(NodeKind::VarRef, lineOf(tok));
    peek(handle)->name = rv_text(tok);
    return handle;
}

int rv_borrow(int tok, bool isMut) {
    // 先造被借用的目标（`x`），再把它整体取进 Borrow 节点的 `base`。
    const int target = rv_var(tok);
    const int handle = make(NodeKind::Borrow, lineOf(target));
    Node *node = peek(handle);
    node->isMut = isMut;
    node->base = take(target);
    return handle;
}

int rv_call(int name, int args) {
    const int handle = make(NodeKind::Call, lineOf(name));
    Node *node = peek(handle);
    node->name = rv_text(name);
    node->args = takeList(args);
    return handle;
}

int rv_field_access(int base, int name) {
    const int handle = make(NodeKind::FieldAccess, lineOf(base));
    Node *node = peek(handle);
    node->name = rv_text(name);
    node->base = take(base);
    return handle;
}

/* ------------------------------------------------------------------ *
 * 解析驱动
 * ------------------------------------------------------------------ */

bool parseText(const std::string &source, NodePtr &program, std::vector<std::string> &errors) {
    // 每次解析都从干净状态开始：池子、行号游标都要复位，否则第二次解析会串味。
    g_pool.clear();
    g_lastLine = 1;

    // 统一换行 + 保证末行有换行：生成器的行列号按「一行一个 \n」算。
    std::string normalized;
    normalized.reserve(source.size() + 1);
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\r') {
            if (i + 1 < source.size() && source[i + 1] == '\n') continue;
            normalized.push_back('\n');
        } else {
            normalized.push_back(source[i]);
        }
    }
    if (normalized.empty() || normalized.back() != '\n') normalized.push_back('\n');

    Parser::Engine engine;
    Parser::ParserValue_T root = -1;
    const Parser::Status status = engine.runText(normalized, &root);
    if (status != Parser::Status::Ok) {
        for (const Parser::Error &error : engine.errors()) {
            std::ostringstream out;
            out << "第 " << error.line() << " 行第 " << error.column() << " 列："
                << error.message();
            if (!error.context().empty()) out << "\n          " << error.context();
            errors.push_back(out.str());
        }
        if (errors.empty()) {
            errors.push_back(std::string("语法分析失败（") + Parser::statusName(status) + "）");
        }
        g_pool.clear();
        return false;
    }

    program = take(root);
    if (!program || program->kind != NodeKind::Program) {
        errors.push_back("解析成功但没有得到程序节点");
        g_pool.clear();
        return false;
    }
    g_pool.clear();  // 根节点已经取走，剩下的都是解析中间产物
    return true;
}

} // namespace rustview
