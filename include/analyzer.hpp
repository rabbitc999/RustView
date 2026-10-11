/**
 * @file analyzer.hpp
 * @brief `Analyzer`：作用域栈 + 借用状态表，把语法树变成「可画图的分析结果」。
 *
 * 这份文件的注释是按「没写过 Rust 的 C++ 开发者」写的：每一处判断都解释
 * **为什么**要这么判，而不是重复代码在做什么。
 */
#pragma once

#include <map>
#include <string>
#include <vector>

#include "rust_ast.hpp"

namespace rustview {

/**
 * @brief 变量在借用分析里的状态。
 *
 * 状态之间怎么流转让 `Analyzer::analyzeExpr` / `analyzeLet` 里的注释说话，这里只说含义：
 *   - `Uninitialized`：名字已经登记、但还没有值（本工具里 `let` 必带初值，
 *     所以这是给「未来支持先声明后赋值」留的位置，正常输入不会出现）；
 *   - `Initialized`：持有值（`int`）或堆对象（`String` / 结构体），**可以读、可以移动**；
 *   - `Moved`：所有权已经被移走，名字还在、值没了，再用就是编译错误；
 *   - `BorrowedImmutable`：正被 `&T` 借用（可以同时借给多个人）；
 *   - `BorrowedMutable`：正被 `&mut T` 借用（独占，期间谁都不能读也不能借）。
 *
 * 「要不要把状态写成 Moved / Borrowed 之一」由 `refreshState()` 按借用计数**推导**，
 * 而不是各个分支各自赋值——这样「借出去 -> 还回来」的来回切换只有一处实现。
 */
enum class VarState {
    Uninitialized,     ///< 未初始化（占位，正常输入不出现）
    Initialized,       ///< 已初始化、可读可移
    Moved,             ///< 已被 move，原名字失效
    BorrowedImmutable, ///< 正被不可变借用
    BorrowedMutable    ///< 正被可变借用（独占）
};

/**
 * @brief 状态名（英文枚举名，画图和报错都用它，方便和 Rust 术语对上）。
 * @param state 变量状态。
 * @return 静态字符串，如 `"Initialized"`。
 */
const char *varStateName(VarState state);

/** @brief 变量表里的一行：一个具名绑定（形参、`let` 出来的变量都算）。 */
struct VarInfo {
    int id = -1;                 ///< 变量 id（= 在 `AnalysisResult::vars` 里的下标）
    std::string name;            ///< 源码里的名字
    std::string typeName;        ///< 类型名（引用变量存的是被指向的类型名，如 `String`）
    bool isRef = false;          ///< 这个绑定本身是不是引用（`&T` / `&mut T` / 返回引用）
    bool isMut = false;          ///< `let mut` 或 `&mut`
    bool valueSemantics = false; ///< 值语义（`int`）：赋值是拷贝；否则赋值是 Move
    VarState state = VarState::Initialized; ///< 借用状态表里的当前状态
    int scope = -1;              ///< 所在作用域（在 `AnalysisResult::scopes` 里的下标）
    int line = 1;                ///< 声明行（1 起）
    int target = -1;             ///< 引用变量：被指向的变量 id；-1 = 指向函数外部（形参）
    int immutableBorrows = 0;    ///< 当前有几个 `&T` 借用它
    int mutableBorrows = 0;      ///< 当前有几个 `&mut T` 借用它
    bool destroyed = false;      ///< 是否已随作用域结束被销毁（悬垂检测看它）
    bool fnParam = false;        ///< 是不是函数形参
    bool dangling = false;       ///< 这个绑定接收了一个悬垂引用（画图时画断线）
    std::string heap;            ///< 堆对象描述（`String "hello"`）；空 = 不占堆
};

/** @brief 一条借用边：`from`（引用变量）指向 `to`（被借用变量）。 */
struct BorrowEdge {
    int from = -1;      ///< 引用变量的 id
    int to = -1;        ///< 被借用变量的 id
    bool mut = false;   ///< `true` = `&mut`，`false` = `&`
    bool conflict = false; ///< 这条借用触发了冲突（画红）
    bool released = false; ///< 引用随作用域结束而释放（画成浅色：当时借过、现在已还）
    int line = 1;       ///< 借用写在源码第几行
};

/** @brief 一条 Move 边：所有权从 `from` 移到 `to`。 */
struct MoveEdge {
    int from = -1; ///< 原所有者（画灰）
    int to = -1;   ///< 新所有者
    int line = 1;  ///< Move 写在源码第几行
};

/** @brief 一个作用域：画成一层套一层的矩形框。 */
struct ScopeInfo {
    std::string label;        ///< 「fn main」「block」这类标签
    int depth = 0;            ///< 嵌套深度（0 = 最外层函数作用域）
    int line = 1;             ///< `{` 所在行
    bool fnScope = false;     ///< 是不是函数作用域（画粗一点的框）
    std::vector<int> vars;    ///< 在这个作用域里声明的变量 id
};

/** @brief 一次函数调用（画在中间的「函数调用栈」一栏）。 */
struct CallInfo {
    std::string fn;                          ///< 被调函数名
    std::string signature;                   ///< 还原出的签名，如 `borrow_and_return(&String) -> &String`
    bool returnsRef = false;                 ///< 返回值是不是引用
    bool dangling = false;                   ///< 这次调用是否检出悬垂
    int line = 1;                            ///< 调用点行号
    std::vector<std::string> args;           ///< 实参的简短描述
};

/**
 * @brief 一条诊断。
 *
 * `unsupported` 与非 `unsupported` 的区别决定了工具的行为：
 *   - `unsupported == true`：**本工具不支持的结构**（字段级借用 / 部分移动）。
 *     这类问题必须在控制台报错并**直接退出**，绝不生成任何图表；
 *   - `unsupported == false`：借用语义冲突（借用冲突 / 用已移动的变量 / 悬垂指针）。
 *     这类问题会生成一张**诊断图**（冲突区域标红、悬垂画断线），方便教学，
 *     但退出码仍然是 1。
 */
struct Issue {
    std::string message;        ///< 给人看的中文说明
    int line = 1;               ///< 源码行号
    bool unsupported = false;   ///< 不支持的结构（必须中止渲染）
    bool dangling = false;      ///< 悬垂指针（画断线箭头）
    int focus = -1;             ///< 要标红的变量 id
    int focus2 = -1;            ///< 第二个要标红的变量 id（借用冲突的两方）
};

/** @brief 分析产物：图表要画的东西全在这里，`Visualizer` 只读它。 */
struct AnalysisResult {
    std::vector<VarInfo> vars;        ///< 变量表（顺序 = 声明顺序）
    std::vector<BorrowEdge> borrows;  ///< 借用边
    std::vector<MoveEdge> moves;      ///< 所有权转移边
    std::vector<ScopeInfo> scopes;    ///< 作用域栈（下标被 `VarInfo::scope` 引用）
    std::vector<CallInfo> calls;      ///< 函数调用记录
    std::vector<Issue> issues;        ///< 诊断

    std::string danglingFn;           ///< 检出悬垂时：问题函数名
    std::string danglingLocal;        ///< 检出悬垂时：被销毁的局部变量名
    int danglingLine = 0;             ///< 检出悬垂时：调用点行号
    int danglingVar = -1;             ///< 检出悬垂时：接收悬垂引用的变量 id

    /** @brief 有没有诊断。 */
    bool ok() const { return issues.empty(); }
    /** @brief 有没有「不支持的结构」类诊断（有就必须中止渲染）。 */
    bool hasUnsupported() const;
};

/**
 * @brief 借用分析器：消费语法树，产出 `AnalysisResult`。
 *
 * 它维护两张表：
 *   - **作用域栈** `frames_`：`{` 压栈、`}` 弹栈。每一帧带自己的「名字 -> 变量 id」
 *     映射，所以内层同名变量会自然遮蔽（shadow）外层，弹栈时又自动恢复。
 *     每一帧还记着这一层声明过哪些变量，弹栈时按「先全部标记销毁、再找活着的引用」
 *     两步做**悬垂检测**。
 *   - **借用状态表** `result_.vars`：每个变量的状态、被借次数、引用指向谁。
 *     所有冲突判定都只读这张表，判定规则写在 `addBorrow()` / `acquireBorrow()` 上。
 *
 * 用法只有一步：`Analyzer(program).run()`。分析器**不抛异常**：所有问题都进
 * `AnalysisResult::issues`，让调用方决定是报错退出还是画诊断图。
 */
class Analyzer {
public:
    /**
     * @brief 构造分析器。
     * @param program 语法树根节点（`NodeKind::Program`），必须在分析器生命周期内有效。
     */
    explicit Analyzer(const Node &program);

    /**
     * @brief 跑完整套分析。
     * @return 分析结果（变量表 / 借用边 / Move 边 / 作用域 / 调用记录 / 诊断）。
     *         不抛异常；即使有诊断也会返回已经分析出来的部分，供画诊断图。
     */
    AnalysisResult run();

private:
    /** @brief 函数签名（调用点是黑盒：只看签名，不展开函数体）。 */
    struct FnSig {
        const Node *decl = nullptr;      ///< 定义节点（拿行号用）
        std::string returnType;          ///< 返回类型名（`int` / `String` / 结构体名）
        bool returnsRef = false;         ///< 返回 `&T`
        bool returnsMutRef = false;      ///< 返回 `&mut T`
        std::vector<std::string> paramTypes; ///< 形参类型名
        std::vector<bool> paramIsRef;    ///< 形参是不是引用
        std::vector<bool> paramIsMutRef; ///< 形参是不是 `&mut`
        bool bodyReturnsLocal = false;   ///< 函数体把**局部变量**的引用返回出去了
        std::string localName;           ///< 上面那个局部变量的名字
    };

    /** @brief 表达式求值的结果：告诉调用方「这个表达式的值是什么、从哪儿来」。 */
    struct ExprValue {
        enum class Kind { Unknown, Value, Owned, Reference }; ///< 值 / 堆对象 / 引用
        Kind kind = Kind::Unknown;
        int varId = -1;              ///< 来自变量时：变量 id（`Owned` / `Reference` / `Value` 都可能有）
        std::string typeName;        ///< 类型名
        bool mutRef = false;         ///< 引用是不是 `&mut`
        bool dangling = false;       ///< 这个引用是悬垂的
        std::string note;            ///< 「新建的堆对象」「悬垂：指向已销毁的 local」这类说明
    };

    /** @brief 作用域栈的一帧。 */
    struct Frame {
        std::map<std::string, int> names; ///< 这一层里的「名字 -> 变量 id」
        int scope = -1;                   ///< 在 `result_.scopes` 里的下标
    };

    // ---- 声明收集 ----
    void collectDeclarations();
    static void scanReturnExpr(const Node &fnNode, FnSig &sig, std::vector<std::string> &localNames);

    // ---- 作用域栈 ----
    int pushScope(const std::string &label, int line, bool fnScope);
    void popScope();
    int declareVar(const std::string &name, const std::string &typeName, bool isRef,
                   bool isMut, int line, bool fnParam);
    int lookup(const std::string &name) const;

    // ---- 借用状态表 ----
    void refreshState(VarInfo &var);
    bool checkBorrow(int targetId, bool mut, int line);
    void acquireBorrow(int targetId, int refVarId, bool mut, int line);
    void releaseBorrowsOf(int refVarId);

    // ---- 语句 / 表达式 ----
    void analyzeFunction(const Node &fnNode);
    void analyzeBlock(const Node &block, bool pushOwnScope);
    void analyzeStmt(const Node &stmt);
    void analyzeLet(const Node &letNode);
    ExprValue analyzeExpr(const Node &expr);
    ExprValue analyzeCall(const Node &call, bool valueIsUsed);

    // ---- 诊断 ----
    void addIssue(const std::string &message, int line, bool unsupported, bool dangling,
                  int focus, int focus2);

    const Node &program_;              ///< 语法树根
    AnalysisResult result_;            ///< 分析产物
    std::map<std::string, FnSig> sigs_;      ///< 函数签名表（先全部登记，再分析函数体）
    std::map<std::string, const Node *> structs_; ///< 结构体表
    std::vector<Frame> frames_;        ///< 作用域栈
    int conflictPartner_ = -1;         ///< 最近一次借用冲突里「已经存在的那个借用者」（画图标红用）
};

} // namespace rustview
