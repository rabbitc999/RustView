# rust-view —— 代码编写要求与提交要求

本文件是 ParseEngine 仓库 [`AGENTS.md`](https://github.com/rabbitc999/ParseEngine) 的副本，
按 rust-view 自己的路径、命令与远端改写了其中仓库相关的地方；通用规则（文法、注释、提交）
两份保持一致。

本文件只写**通用的代码编写要求与提交要求**；具体功能的设计与实现进度以代码、文档和
样例断言为准。

硬性要求：

1. **必须写完整的文法文件**：`rust_subset.gram` 是这门极简 Rust 的**权威语法定义**
   （`cpp17` 后端 + `ielr` 模式，24 个 Token、53 条产生式、97 个状态、0 冲突）。
   产生式要覆盖完整语法结构，关键字用 `%token NAME "字面量"` **逐个登记**；语义动作
   只调用 `%code`（`include/rust_syntax.hpp`）里声明的 `rv_*` 函数。
2. **`.gram` 不能当"词法的提取器"**：禁止用一个大 `KW` / 一个万能 Token / 一条
   "任意 token 流"的产生式把整门语言糊过去。关键字、语句、表达式都要能在产生式里
   被点名与组合。
3. **解析流程放进解析器生成器**：`src/main.cpp` 只调用生成的 `Parser::Engine::runText`
   与 `parseText()`，**绝不手写 lexer / parser**。建树逻辑（`rv_*`）内嵌在
   `%epilogue/rust_parse.cpp` 里，随生成产物一起编译——建树也在生成器流程内。
4. **生成器（ParseEngine）是外部依赖**：本仓库**只含 `.gram`，不含生成器**。`make` /
   `make gen` 会按项目根目录 → 旁边的 ParseEngine 仓库 `build/bin/` 的顺序自动找它，
   也可用 `make PARSER=<路径>` 显式指定。要能单独克隆、单独构建，**不依赖 ParseEngine 仓库**。
5. **产物只落在 `build/rust-view/`**：生成的分析器（`gen/parser.cpp` 一带）与可执行文件
   都在这里（已 gitignore）；源码目录保持干净。
6. **完整文档注释**：头文件与 `.cpp` 的公开类型/函数都要中文 doc comment
   （`@brief` / `@param` / `@return`），沿用仓库风格。
7. **验收靠断言，不靠肉眼**：`make check` 逐条断言六组样例（两组正常能出图、三种借用问题
   必须非 0 退出、不支持的结构**一个文件都不写**；生命周期样例还要断言图里画出了
   生命周期栏）。加样例就加断言。
8. **1.0 功能冻结的契约**：语言边界、借用分析规则、退出码约定与图表版面视为稳定契约，
   除明确 BUG 外不改动（1.1 按需求解冻过一次，只加了生命周期标注的「只画不校验」，
   见 [`CHANGELOG.md`](CHANGELOG.md)）；改动要同步 [`README.md`](README.md) 与
   [`CHANGELOG.md`](CHANGELOG.md)。
9. `doc/` 放效果图与演示页（`make preview` 重生成）；最后 `git add -A` → commit →
   `git push origin master`（远端 <https://github.com/rabbitc999/rust-view.git>）。

## Git 提交信息（全仓库强制约束）

Git 提交身份按以下条件强制规范，适用于新建、补写、amend 及历史重写提交：

- 分别检查作者（author）与提交者（committer）的身份名称。名称同时包含"小兔"和"糖糖"时，
  该身份的名称必须统一为 **`小兔糖糖OvO`**，邮箱必须同时统一为 **`rabbitc999@outlook.com`**。
- 不满足上述条件的身份保留其真实名称和邮箱，不得批量覆盖其他贡献者或平台提交者的身份。
  这里的"名称"指 Git 作者／提交者名称；提交标题仍遵守下方的中文要求。
- 当前工作身份满足上述条件，仓库本地配置使用 `git config --local user.name "小兔糖糖OvO"`
  和 `git config --local user.email "rabbitc999@outlook.com"`。提交前用
  `git var GIT_AUTHOR_IDENT`、`git var GIT_COMMITTER_IDENT` 核对实际身份，避免环境变量
  或其他配置导致名称、邮箱不符合要求。

本仓库所有 Git 提交的标题及正文（如有）**必须使用中文**，禁止使用纯英文提交说明。
文件名、命令、API 标识符和必要的技术术语可以保留原文，但改动说明必须用中文表达。
此约束适用于本仓库的所有修改。
