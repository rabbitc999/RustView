/**
 * @file rust_ast.hpp
 * @brief RustView 的语法树（AST）：由文法动作建树，交给 `Analyzer` 分析。
 *
 * 设计取舍（写给第一次读这份代码的人）：
 *   - 文法里的语义值类型是 `int`（`%value_type int`）。这个整数是**节点池的下标**，
 *     不是指针——生成出来的 C++ 解析器只保证 `%value_type` 是可平凡复制的，
 *     用下标可以躲开「解析器内部搬值」带来的所有权麻烦。
 *   - 节点池里只有一种节点结构 `Node`：字段多一点、但每个字段都有名字，
 *     比「用位置下标记子节点」好读得多——这本来就是一份教学代码。
 *   - 池子的所有权在 `%epilogue`（见 `epilogue/rust_parse.cpp`）里：动作把子节点
 *     `std::move` 进父节点，解析成功后根节点的所有权交给调用者，
 *     失败时随池子一起析构，不需要单独写释放逻辑。
 *
 * 节点形状（哪个 `NodeKind` 用哪些字段、字段之间是什么关系）在每个字段的注释里写；
 * 读分析器代码时对照着看即可。
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace rustview {

/**
 * @brief 语法树节点的种类。
 *
 * 命名与文法产生式一一对应：`Program` / `StructDecl` / `FnDecl` / `Block` /
 * `Let` / `Print` / `ExprStmt`，表达式是 `IntLit` / `StrLit` / `VarRef` /
 * `Borrow` / `Call` / `FieldAccess`。`List` 是文法里列表型非终结符
 * （顶层项、语句、形参、字段、实参）的中间结果。`Tok` 是词法单元的占位节点，
 * 只在解析过程中存在——建树完成后不会出现在最终 AST 里。
 */
enum class NodeKind {
    Tok,         ///< 词法单元占位（`rv_token_value` 产生，不进最终 AST）
    List,        ///< 列表（元素在 `items` 里）
    Program,     ///< 整个程序（顶层项在 `items` 里）
    StructDecl,  ///< `struct S { a: int, b: String }`（字段在 `fields` 里）
    StructField, ///< 结构体字段：`name` + `type`
    FnDecl,      ///< `fn f(...) -> T { ... }`：`name` + `params` + `ret` + `body`
    Param,       ///< 形参：`name` + `type`（`isMut` 表示形参本身是 `&mut`）
    Type,        ///< 类型：`name` 是类型名，`isRef` / `isMut` 表示 `&T` / `&mut T`
    Block,       ///< `{ ... }`：语句在 `stmts` 里，尾表达式在 `tail` 里（可为空）
    Let,         ///< `let [mut] name = init;`：`isMut` + `name` + `init`
    Print,       ///< `print(init);`
    ExprStmt,    ///< 表达式语句：`init` 是被求值的表达式
    IntLit,      ///< 整数字面量：`name` 是原文
    StrLit,      ///< 字符串字面量：`name` 是**已去引号**的正文
    VarRef,      ///< 标识符引用：`name`
    Borrow,      ///< `&x` / `&mut x`：`base` 是目标（`VarRef`），`isMut` 区分可变借用
    Call,        ///< `f(a, b)`：`name` 是函数名，`args` 是实参
    FieldAccess  ///< `s.a`：`base` 是基对象，`name` 是字段名（**本工具不支持**）
};

struct Node;
/// @brief AST 节点的独占所有权指针。父节点持有子节点，析构即递归释放。
using NodePtr = std::unique_ptr<Node>;

/**
 * @brief 一个语法树节点。
 *
 * 一个结构装下所有种类，是为了让文法动作只跟一种类型打交道；每个 `NodeKind`
 * 只使用自己那几个字段（见 `NodeKind` 注释与各字段注释）。
 */
struct Node {
    NodeKind kind = NodeKind::Tok;  ///< 节点种类，决定下面哪些字段有意义
    std::string name;               ///< 标识符 / 字面量原文 / 函数名 / 字段名 / 类型名
    bool isMut = false;             ///< `let mut` / `&mut` / `&mut T` 的「可变」标记
    bool isRef = false;             ///< 类型节点：是否为引用 `&T` / `&mut T`
    int line = 1;                   ///< 起始行号（1 起），报错和画图都用它
    double number = 0.0;            ///< 词法单元占位节点上的数值（整数字面量用）

    std::vector<NodePtr> items;   ///< `List` 的元素 / `Program` 的顶层项
    std::vector<NodePtr> stmts;   ///< `Block` 的语句序列
    std::vector<NodePtr> params;  ///< `FnDecl` 的形参（`Param`）
    std::vector<NodePtr> fields;  ///< `StructDecl` 的字段（`StructField`）
    std::vector<NodePtr> args;    ///< `Call` 的实参表达式

    NodePtr type;   ///< `Param` / `StructField` 的类型
    NodePtr ret;    ///< `FnDecl` 的返回类型（写 `-> T` 才有，否则为空）
    NodePtr init;   ///< `Let` 的初值 / `Print` 的对象 / `ExprStmt` 的表达式
    NodePtr tail;   ///< `Block` 的尾表达式（`{ ...; expr }` 里的 `expr`，可为空）
    NodePtr base;   ///< `Borrow` / `FieldAccess` 的目标（`VarRef` 或任意表达式）
    NodePtr body;   ///< `FnDecl` 的函数体（`Block`）
};

/**
 * @brief 读节点的名字，空指针安全。
 * @param node 任意节点，可为 `nullptr`。
 * @return 节点名字；`node` 为空时返回空串。
 */
inline const std::string &nodeName(const Node *node) {
    static const std::string kEmpty;
    return node ? node->name : kEmpty;
}

/** @brief 类型名是否为「值语义」——本工具只把 `int` 当作拷贝语义。 */
inline bool isValueType(const std::string &typeName) { return typeName == "int"; }

} // namespace rustview
