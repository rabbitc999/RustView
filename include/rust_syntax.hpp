/**
 * @file rust_syntax.hpp
 * @brief 文法动作（`rv_*`）与解析入口的声明——`rust_subset.gram` 的 `%code` 会包含它。
 *
 * 这个头文件是**生成物与手写代码之间的唯一接缝**：
 *   - `rust_subset.gram` 的产生式只调用这里的 `rv_*`，自己不写任何逻辑；
 *   - 定义在 `epilogue/rust_parse.cpp` 里，由 `%epilogue include` 内嵌进生成的
 *     `parser.cpp`（所以「词法 + 语法 + 建树」是同一份生成结果，`src/` 里没有
 *     手写的解析驱动）；
 *   - `main.cpp` 只调用 `parseText()`，看不到节点池的存在。
 *
 * 句柄（`int`）的含义：**同一张节点池的下标**。词法单元和语法树节点放在同一个池子里，
 * 所以动作里 `$1`（终结符）和 `$1`（非终结符）可以用同一套 `rv_text` / `rv_line` 读。
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "rust_ast.hpp"

namespace rustview {

/* ---------------------------------------------------------------- *
 * 词法单元 -> 语义值
 * ---------------------------------------------------------------- */

/**
 * @brief 把一个词法单元存进节点池，返回它在池子里的下标。
 * @param text  词素原文（字符串字面量含引号；标识符不含）。
 * @param number 词法层解析出的数值（整数字面量时有效，其它为 0）。
 * @param line  词法单元所在行号（1 起）。
 * @return 池子下标，可直接当语义值用；`rv_text` / `rv_line` 认这个下标。
 *
 * 生成器会在**每个**被移进分析栈的 Token 上调用它（通过下面的
 * `PARSER_USER_TOKEN_VALUE` 宏），所以产生式里的 `$n` 天然就是干净的文本句柄。
 */
int rv_token_value(std::string_view text, double number, int line);

/** @brief 读句柄的原文（标识符名 / 字符串正文 / 字面量原文）。越界返回空串。 */
std::string rv_text(int handle);

/** @brief 读句柄所在行号；越界返回 1。 */
int rv_line(int handle);

/* ---------------------------------------------------------------- *
 * 建树动作（产生式调用；全部是「取子节点 -> 造父节点」的池子操作）
 * ---------------------------------------------------------------- */

/** @brief 空列表节点（`item_list` / `stmt_list` / 形参表 / 字段表 / 实参表的初值）。 */
int rv_list();
/** @brief 往列表里追加一个元素（元素所有权移交给列表）。 */
void rv_list_push(int list, int item);
/** @brief 占位句柄：表达能力里「这里什么都没有」（无返回类型、块没有尾表达式）。 */
int rv_none();

/** @brief 造根节点 `Program`。 */
int rv_program(int items);

/** @brief 造 `struct S { ... }`。 */
int rv_struct(int name, int fields);
/** @brief 往字段表里加一个 `name: type`。 */
void rv_struct_field(int list, int name, int type);

/** @brief 造 `fn f<生命周期参数>(参数) -> 返回类型 { 体 }`；`lifetimes` / `ret` 可以是 `rv_none()`。 */
int rv_fn(int name, int lifetimes, int params, int ret, int body);
/** @brief 往形参表里加一个 `name: type`；`isMut` 表示形参写成 `&mut T`。 */
void rv_param(int list, int name, int type);

/** @brief 造类型节点：`name` 是类型名，`isRef` / `isMut` 表示 `&T` / `&mut T`。 */
int rv_type(int name, bool isRef, bool isMut);
/** @brief 造带生命周期标注的类型 `&'a T` / `&'a mut T`；`lifetime` 是不带撇号的参数句柄。 */
int rv_lifetime_type(int lifetime, int name, bool isRef, bool isMut);
/** @brief 造一个生命周期参数 `'a`（`constraint` 是约束的 `'b`，没有就传 `rv_none()`）。 */
int rv_lifetime_decl(int lifetime, int constraint);
/** @brief 往泛型参数表里追加一个生命周期参数节点。 */
void rv_lifetime_param(int list, int decl);

/** @brief 造 `Block`；`tail` 是尾表达式（没有就传 `rv_none()`），它是块的值。 */
int rv_block(int stmts, int tail);
/** @brief 造 `let [mut] name = init;`。 */
int rv_let(int name, bool isMut, int init);
/** @brief 造 `print(expr);`。 */
int rv_print(int expr);
/** @brief 造表达式语句 `expr;`。 */
int rv_expr_stmt(int expr);

/** @brief 整数字面量。 */
int rv_int_lit(int tok);
/** @brief 字符串字面量（存的是**已去引号**的正文）。 */
int rv_str_lit(int tok);
/** @brief 标识符引用 `name`。 */
int rv_var(int tok);
/** @brief `&name` / `&mut name`。 */
int rv_borrow(int tok, bool isMut);
/** @brief 函数调用 `name(args)`。 */
int rv_call(int name, int args);
/** @brief 字段访问 `s.a`（本工具不支持，分析器会报错）。 */
int rv_field_access(int base, int name);

/* ---------------------------------------------------------------- *
 * 解析入口
 * ---------------------------------------------------------------- */

/**
 * @brief 解析一段 RustView 源码，产出语法树。
 * @param source 源码全文（`\r\n` 会被归一成 `\n`；末行缺 `\n` 会自动补）。
 * @param program [out] 成功时接收根节点（`Program`）的所有权；失败时不动它。
 * @param errors  [out] 失败原因，一行一条（带行列号，来自生成的解析器）。
 * @return 词法 / 语法分析成功返回 `true`；否则 `false`，失败原因写进 `errors`。
 *
 * 只做「文本 -> 语法树」：借用语义的全部判断都在 `Analyzer` 里，两者互不耦合，
 * 所以语法错误一定会**在生成任何图表之前**被挡住。
 */
bool parseText(const std::string &source, NodePtr &program, std::vector<std::string> &errors);

} // namespace rustview

/**
 * 生成器认这个宏：把 Token 换成语义值。
 *
 * 宏体在只有解析器内部才展开（那时 `Parser::Token` 已定义），所以这里放心写
 * `pToken->text` / `pToken->value` / `pToken->line`。
 */
#define PARSER_USER_TOKEN_VALUE(pToken) \
    (::rustview::rv_token_value((pToken)->text, (pToken)->value, (pToken)->line))
