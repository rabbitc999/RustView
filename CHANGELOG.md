# RustView 变更记录

版本号写在 [`VERSION`](VERSION) 里，格式 `MAJOR.MINOR.PATCH`。

**1.0.0 是冻结版本。** 语言边界（[`rust_subset.gram`](rust_subset.gram) 覆盖的那一小撮
Rust 伪代码）、借用分析规则（作用域栈 + 借用状态表）、退出码约定与 SVG / HTML 版面，
都视为稳定契约：**除明确 BUG 外不再改动**，也不再接收新特性。

## 1.0.0 — 冻结

首个正式版本，功能冻结。

**语言与文法**
- 关键字集合冻结：`fn` / `let` / `mut` / `print` / `struct` / `int` / `String`，各占一个 `%token`。
- 类型只认 `int` / `String` / `&T` / `&mut T` 与用户结构体名；借用只支持
  `&变量` / `&mut 变量` 这种整体借用。
- 20 个 Token、43 条产生式；`cpp17` + IELR(1) 生成，76 状态、0 未消解冲突。

**分析口径（1.0 冻结）**
- 变量状态：`Uninitialized` / `Initialized` / `Moved` / `BorrowedImmutable` /
  `BorrowedMutable`，由借用计数在 `refreshState()` 统一推导。
- 出作用域三步：先归还借用计数，再整层标记销毁，最后做「引用活过被借对象」的悬垂检测。
- 退出码：`0` 通过 / `1` 语法错误或借用冲突或悬垂 / `2` 不支持的结构（且不写任何文件）。

**输出（1.0 冻结）**
- 三栏版面：栈 · 作用域 / 函数调用栈 / 堆对象；快照点取「入口函数 `main` 返回之前」。
- 冲突标红、悬垂画断裂箭头；同时输出 `<基名>.svg`、`index.html` 与 `<基名>.html`。

## 维护方式

发现明确 BUG 时修复，并在这里记一条 `PATCH`；其余情况保持现状。
