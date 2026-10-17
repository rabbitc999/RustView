/**
 * @file analyzer.cpp
 * @brief `Analyzer` 的实现：作用域栈 + 借用状态表 + 冲突判定。
 *
 * 阅读顺序建议：
 *   1. `run()` / `collectDeclarations()` —— 先登记签名，再逐个函数分析；
 *   2. `pushScope()` / `popScope()` —— 作用域栈的压 / 弹，含悬垂检测；
 *   3. `analyzeLet()` / `analyzeExpr()` —— 状态流转（拷贝 / Move / 借用 / 调用）；
 *   4. `checkBorrow()` / `acquireBorrow()` —— 借用冲突的三条判定规则。
 */
#include "analyzer.hpp"

#include <algorithm>
#include <sstream>

namespace rustview {

const char *varStateName(VarState state) {
    switch (state) {
    case VarState::Uninitialized: return "Uninitialized";
    case VarState::Initialized: return "Initialized";
    case VarState::Moved: return "Moved";
    case VarState::BorrowedImmutable: return "BorrowedImmutable";
    case VarState::BorrowedMutable: return "BorrowedMutable";
    }
    return "Unknown";
}

bool AnalysisResult::hasUnsupported() const {
    for (const Issue &issue : issues) {
        if (issue.unsupported) return true;
    }
    return false;
}

bool AnalysisResult::hasLifetimes() const {
    if (!lifetimeNotes.empty()) return true;
    // 没有泛型参数表、也没有 `&'a T` 时，图上一栏都不多画。
    for (const VarInfo &var : vars) {
        if (!var.lifetime.empty()) return true;
    }
    for (const CallInfo &call : calls) {
        if (!call.lifetime.empty() || !call.returnLifetime.empty()) return true;
    }
    return false;
}

namespace {

/** @brief 收集函数体里所有 `let` 出来的局部变量名（含嵌套块）。 */
void collectLocalNames(const Node &node, std::vector<std::string> &out) {
    if (node.kind == NodeKind::Let) out.push_back(node.name);
    for (const NodePtr &child : node.stmts) collectLocalNames(*child, out);
    for (const NodePtr &child : node.items) collectLocalNames(*child, out);
    if (node.tail) collectLocalNames(*node.tail, out);
    if (node.init) collectLocalNames(*node.init, out);
    if (node.body) collectLocalNames(*node.body, out);
    if (node.base) collectLocalNames(*node.base, out);
    for (const NodePtr &child : node.args) collectLocalNames(*child, out);
}

/** @brief 名字是否在列表里。 */
bool contains(const std::vector<std::string> &list, const std::string &name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

/** @brief 实参 / 表达式的简短描述（写进调用栈那一栏）。 */
std::string describeExpr(const Node &node) {
    switch (node.kind) {
    case NodeKind::VarRef: return node.name;
    case NodeKind::Borrow: return (node.isMut ? "&mut " : "&") +
                                  (node.base ? describeExpr(*node.base) : std::string("?"));
    case NodeKind::IntLit: return node.name;
    case NodeKind::StrLit: return "\"" + node.name + "\"";
    case NodeKind::Call: return node.name + "(…)";
    case NodeKind::FieldAccess:
        return (node.base ? describeExpr(*node.base) : std::string("<表达式>")) + "." + node.name;
    default: return "…";
    }
}

} // namespace

Analyzer::Analyzer(const Node &program) : program_(program) {}

/* ------------------------------------------------------------------ *
 * 第一步：登记所有签名
 * ------------------------------------------------------------------ */

void Analyzer::collectDeclarations() {
    for (const NodePtr &item : program_.items) {
        if (item->kind == NodeKind::StructDecl) {
            structs_[item->name] = item.get();
            continue;
        }
        if (item->kind != NodeKind::FnDecl) continue;

        FnSig sig;
        sig.decl = item.get();
        sig.returnType = item->ret ? item->ret->name : "()";
        sig.returnsRef = item->ret && item->ret->isRef;
        sig.returnsMutRef = item->ret && item->ret->isRef && item->ret->isMut;
        sig.returnLifetime = lifetimeOf(item->ret.get());
        for (const NodePtr &param : item->params) {
            const bool ref = param->type && param->type->isRef;
            const bool mutRef = param->type && param->type->isRef && param->type->isMut;
            sig.paramTypes.push_back(param->type ? param->type->name : "?");
            sig.paramIsRef.push_back(ref);
            sig.paramIsMutRef.push_back(mutRef);
            sig.paramLifetimes.push_back(lifetimeOf(param->type.get()));
        }
        // 生命周期参数表：先按声明顺序登记原文与名单，再查「重名 / 未声明 / 约束指向自己」。
        for (const NodePtr &lifetime : item->lifetimes) {
            sig.lifetimes.push_back(lifetime->name);
            if (!sig.lifetimeList.empty()) sig.lifetimeList += ", ";
            sig.lifetimeList += lifetime->name;
            if (lifetime->base) sig.lifetimeList += ": " + lifetime->base->name;
        }
        if (!sig.lifetimeList.empty()) sig.lifetimeList = "<" + sig.lifetimeList + ">";
        checkLifetimeDecls(*item, sig);

        // 悬垂预扫描：只看「函数把谁返回出去了」，为调用点提供判据。
        std::vector<std::string> locals;
        collectLocalNames(*item, locals);
        scanReturnExpr(*item, sig, locals);
        sigs_[item->name] = sig;
    }
}

void Analyzer::checkLifetimeDecls(const Node &fnNode, FnSig &sig) {
    // 这一组检查全部只是「名字对不上」这种低级错误，和借用检查器无关：
    //   ① 同一个函数里重复声明同名生命周期（`<'a, 'a>`）；
    //   ② 类型上用了没在 `<...>` 里声明的生命周期（含「整个函数压根没有泛型参数表」）；
    //   ③ 约束写成 `'a: 'a`（自己约束自己）。
    // 报错都用 `lifetime = true`：诊断列表里归到「生命周期」一类、退出码仍是 1。
    std::vector<std::string> seen;
    for (const NodePtr &lifetime : fnNode.lifetimes) {
        if (contains(seen, lifetime->name)) {
            addIssue("生命周期 `" + lifetime->name + "` 在 `fn " + fnNode.name +
                         "` 的泛型参数表里重复声明",
                     lifetime->line, false, false, -1, -1, /*lifetime=*/true);
        }
        seen.push_back(lifetime->name);
        if (lifetime->base && lifetime->base->name == lifetime->name) {
            addIssue("生命周期约束 `" + lifetime->name + ": " + lifetime->base->name +
                         "` 自己约束自己，没有意义",
                     lifetime->line, false, false, -1, -1, /*lifetime=*/true);
        }
    }

    // 未声明的生命周期按函数合报一条：同一个函数里 `&'a String` → `&'a String`
    // 必然两处都错，分两条说只是噪声；位置取第一次用到的行。
    if (sig.lifetimes.empty()) {
        std::string used;
        int line = 0;
        for (std::size_t i = 0; i < sig.paramLifetimes.size(); ++i) {
            if (sig.paramLifetimes[i].empty()) continue;
            if (used.empty()) used = sig.paramLifetimes[i];
            if (line == 0 && i < fnNode.params.size() && fnNode.params[i]->type) {
                line = fnNode.params[i]->type->line;
            }
        }
        if (used.empty()) used = sig.returnLifetime;
        if (!used.empty() && line == 0 && fnNode.ret) line = fnNode.ret->line;
        if (!used.empty()) {
            addIssue("生命周期 " + used + " 没有声明：`fn " + fnNode.name +
                         "` 的类型里用了它，但函数没有泛型参数表（应写成 `fn " + fnNode.name +
                         "<" + used + ">(...)`）",
                     line > 0 ? line : fnNode.line, false, false, -1, -1, /*lifetime=*/true);
        }
        return;
    }

    auto checkUse = [&](const std::string &name, int line, const std::string &where) {
        if (name.empty() || contains(sig.lifetimes, name)) return;
        addIssue("生命周期 `" + name + "` 没有在 `fn " + fnNode.name + "` 的泛型参数表里声明：" +
                     where + " 用了它",
                 line, false, false, -1, -1, /*lifetime=*/true);
    };
    for (const NodePtr &param : fnNode.params) {
        checkUse(lifetimeOf(param->type.get()),
                 param->type ? param->type->line : param->line,
                 "形参 `" + param->name + "`");
    }
    if (fnNode.ret) checkUse(fnNode.ret->lifetime, fnNode.ret->line, "返回类型");
}

void Analyzer::scanReturnExpr(const Node &fnNode, FnSig &sig, std::vector<std::string> &localNames) {
    // 只有「返回引用」的函数才可能悬垂：返回 `int` / `String` 的函数，
    // 值/所有权都被搬出来了，函数退出不销毁它。
    if (!sig.returnsRef) return;

    const Node *tail = fnNode.body ? fnNode.body->tail.get() : nullptr;
    if (!tail) return;  // 没有尾表达式（例如函数体最后是一条语句），交给调用点按「无绑定来源」处理

    auto isParam = [&](const std::string &name) {
        for (const NodePtr &param : fnNode.params) {
            if (param->name == name) return true;
        }
        return false;
    };
    auto paramIsRef = [&](const std::string &name) {
        for (const NodePtr &param : fnNode.params) {
            if (param->name == name) return param->type && param->type->isRef;
        }
        return false;
    };

    // 情形一：`&local` —— 借用了函数体里的局部变量，函数一返回它就销毁，必定悬垂。
    if (tail->kind == NodeKind::Borrow) {
        const std::string name = tail->base ? tail->base->name : std::string();
        if (!isParam(name) && contains(localNames, name)) {
            sig.bodyReturnsLocal = true;
            sig.localName = name;
        }
        return;
    }

    // 情形二：直接返回一个名字。返回引用形参是安全的（生命周期跟着调用方），
    // 返回局部变量则和上面一样悬垂。
    if (tail->kind == NodeKind::VarRef) {
        if (paramIsRef(tail->name)) return;
        if (contains(localNames, tail->name)) {
            sig.bodyReturnsLocal = true;
            sig.localName = tail->name;
        }
        return;
    }

    // 情形三：字面量 / 临时表达式 —— 没有可绑定的来源，同样按悬垂处理。
    sig.bodyReturnsLocal = true;
    sig.localName = "<表达式临时值>";
}

/* ------------------------------------------------------------------ *
 * 第二步：作用域栈
 * ------------------------------------------------------------------ */

int Analyzer::pushScope(const std::string &label, int line, bool fnScope) {
    // 压栈：先给结果表建一条 ScopeInfo（画图要按它套矩形框），再压一帧。
    // 「名字 -> 变量 id」放在帧里，所以内层同名变量天然遮蔽外层；
    // 弹栈时整帧丢掉，外层的同名变量自动重新可见 —— 这就是 shadowing 的实现。
    ScopeInfo info;
    info.label = label;
    info.depth = static_cast<int>(frames_.size());
    info.line = line;
    info.fnScope = fnScope;
    result_.scopes.push_back(info);

    Frame frame;
    frame.scope = static_cast<int>(result_.scopes.size()) - 1;
    frames_.push_back(std::move(frame));
    return frames_.back().scope;
}

void Analyzer::popScope() {
    if (frames_.empty()) return;
    Frame frame = std::move(frames_.back());
    frames_.pop_back();
    const std::vector<int> declared = result_.scopes[static_cast<std::size_t>(frame.scope)].vars;

    // 第零步：**清理本层变量的借用状态**。引用变量在本层声明、也在本层结束，
    // 它借出去的那份「占用」必须还回去——否则「块里借一下、块外再可变借」
    // 会被误判成冲突（那恰恰是块作用域的意义：借用在块结束时就释放了）。
    for (int id : declared) {
        if (result_.vars[static_cast<std::size_t>(id)].isRef) releaseBorrowsOf(id);
    }

    // 第一步：本层声明的变量**全部**标记为销毁。必须一次标完再检查，
    // 否则「同层的引用变量」会在自己被标记之前抢先报出悬垂（那是误报：
    // 引用和它指向的变量同生共死，本来就该一起走）。
    //
    // 这里只设 `destroyed`、**不**动 `state`：`destroyed` 表示「存储没了」，
    // `state` 表示「值还在不在 / 借出去没有」。把 state 一律改成 Moved，会让
    // 入口函数 main 里的变量在图上全变成灰色「已移动」，那是误导——真正被
    // Move 的变量在 analyzeLet() 里已经改成 Moved 了。
    for (int id : declared) {
        result_.vars[static_cast<std::size_t>(id)].destroyed = true;
    }

    // 第二步：**悬垂检测的触发条件** —— 被销毁的变量上，还挂着一个「活着的引用」。
    // 活着的引用只可能来自外层作用域（同层的引用跟它一起销毁了），
    // 也就是「引用的生命周期超过了它借用的那个变量」。
    for (int id : declared) {
        for (VarInfo &ref : result_.vars) {
            if (ref.destroyed || !ref.isRef || ref.target != id) continue;
            ref.dangling = true;
            addIssue("悬垂指针：引用 `" + ref.name + "` 指向的变量 `" +
                         result_.vars[static_cast<std::size_t>(id)].name +
                         "` 在离开作用域时被销毁，引用会指向已失效的内存",
                     ref.line, /*unsupported=*/false, /*dangling=*/true, ref.id, id);
        }
    }
}

int Analyzer::declareVar(const std::string &name, const std::string &typeName, bool isRef,
                         bool isMut, int line, bool fnParam, const std::string &lifetime) {
    VarInfo var;
    var.id = static_cast<int>(result_.vars.size());
    var.name = name;
    var.typeName = typeName;
    var.lifetime = isRef ? lifetime : std::string();  // 生命周期只属于引用
    var.isRef = isRef;
    var.isMut = isMut;
    var.valueSemantics = !isRef && isValueType(typeName);
    var.state = VarState::Initialized;
    var.scope = frames_.empty() ? -1 : frames_.back().scope;
    var.line = line;
    var.fnParam = fnParam;
    result_.vars.push_back(var);

    if (!frames_.empty()) {
        result_.scopes[static_cast<std::size_t>(frames_.back().scope)].vars.push_back(var.id);
        frames_.back().names[name] = var.id;  // 登记进当前帧，遮蔽外层同名变量
    }
    return var.id;
}

int Analyzer::lookup(const std::string &name) const {
    // 从最内层往外找：找到的第一个就是当前可见的绑定（shadowing）。
    for (std::size_t i = frames_.size(); i-- > 0;) {
        const auto it = frames_[i].names.find(name);
        if (it != frames_[i].names.end()) return it->second;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * 第三步：借用状态表
 * ------------------------------------------------------------------ */

void Analyzer::refreshState(VarInfo &var) {
    // Moved 是终态：一个变量被移走之后，再多借用也不会「复活」。
    if (var.state == VarState::Moved) return;
    if (var.mutableBorrows > 0) var.state = VarState::BorrowedMutable;
    else if (var.immutableBorrows > 0) var.state = VarState::BorrowedImmutable;
    else var.state = VarState::Initialized;
}

bool Analyzer::checkBorrow(int targetId, bool mut, int line) {
    if (targetId < 0) return true;
    VarInfo &target = result_.vars[static_cast<std::size_t>(targetId)];

    if (target.destroyed) {
        addIssue("不能借用 `" + target.name + "`：它已经离开作用域被销毁了", line, false, true,
                 targetId, -1);
        return false;
    }
    if (target.state == VarState::Moved) {
        addIssue("不能借用 `" + target.name + "`：它的所有权已经被移动走了", line, false, false,
                 targetId, -1);
        return false;
    }

    // 借用冲突的三条规则（第三条由前两条组合覆盖）：
    //   ① `&mut x` 期间不能再有**任何**借用（`&x` 或 `&mut x`）—— 独占；
    //   ② `&x` 期间不能有 `&mut x` —— 读不能和写并存；
    //   ③ 多个 `&x` 是允许的（都是只读）。
    conflictPartner_ = -1;
    if (mut && (target.mutableBorrows > 0 || target.immutableBorrows > 0)) {
        for (VarInfo &other : result_.vars) {
            if (other.isRef && other.target == targetId && !other.destroyed) {
                conflictPartner_ = other.id;
                break;
            }
        }
        addIssue("借用冲突：`" + target.name + "` 正被 " +
                     (target.mutableBorrows > 0 ? "可变借用 `&mut`" : "不可变借用 `&`") +
                     " 借出，不能再对它取 `&mut " + target.name + "`",
                 line, /*unsupported=*/false, /*dangling=*/false, targetId, conflictPartner_);
        return false;
    }
    if (!mut && target.mutableBorrows > 0) {
        for (VarInfo &other : result_.vars) {
            if (other.isRef && other.target == targetId && !other.destroyed) {
                conflictPartner_ = other.id;
                break;
            }
        }
        addIssue("借用冲突：`" + target.name + "` 正被可变借用 `&mut " + target.name +
                     "` 独占，不能再取 `&" + target.name + "`",
                 line, /*unsupported=*/false, /*dangling=*/false, targetId, conflictPartner_);
        return false;
    }
    return true;
}

void Analyzer::acquireBorrow(int targetId, int refVarId, bool mut, int line) {
    if (targetId < 0 || targetId >= static_cast<int>(result_.vars.size())) return;
    VarInfo &target = result_.vars[static_cast<std::size_t>(targetId)];
    if (mut) target.mutableBorrows++;
    else target.immutableBorrows++;
    refreshState(target);  // 状态由计数推导，见 refreshState()

    if (refVarId >= 0) {
        BorrowEdge edge;
        edge.from = refVarId;
        edge.to = targetId;
        edge.mut = mut;
        edge.line = line;
        result_.borrows.push_back(edge);
    }
}

void Analyzer::releaseBorrowsOf(int refVarId) {
    for (BorrowEdge &edge : result_.borrows) {
        if (edge.from != refVarId || edge.to < 0) continue;
        VarInfo &target = result_.vars[static_cast<std::size_t>(edge.to)];
        if (edge.mut) {
            if (target.mutableBorrows > 0) target.mutableBorrows--;
        } else {
            if (target.immutableBorrows > 0) target.immutableBorrows--;
        }
        refreshState(target);
        // 只打「已释放」标记，**不**把 to 抹成 -1：图表仍然要画这条边，
        // 只是画成浅色，告诉读者「这里当时确实借过，离开作用域时已经还回去了」。
        edge.released = true;
    }
}

void Analyzer::addIssue(const std::string &message, int line, bool unsupported, bool dangling,
                        int focus, int focus2, bool lifetime) {
    Issue issue;
    issue.message = message;
    issue.line = line;
    issue.unsupported = unsupported;
    issue.dangling = dangling;
    issue.lifetime = lifetime;
    issue.focus = focus;
    issue.focus2 = focus2;
    result_.issues.push_back(issue);
}

void Analyzer::addLifetimeNote(const std::string &fn, int line, const std::string &note,
                               bool warning) {
    // 注意：这里**不是** addIssue。生命周期标注的观察结果不进诊断列表，
    // 也就不影响退出码；它只作为中间一栏的注解画出来。
    LifetimeNote entry;
    entry.fn = fn;
    entry.line = line;
    entry.note = note;
    entry.warning = warning;
    result_.lifetimeNotes.push_back(entry);
}

/* ------------------------------------------------------------------ *
 * 第四步：函数与语句
 * ------------------------------------------------------------------ */

void Analyzer::analyzeFunction(const Node &fnNode) {
    pushScope("fn " + fnNode.name, fnNode.line, /*fnScope=*/true);
    // 生命周期参数表在这一层可见：只有 `let` / 类型标注能用到它，所以登记在签名表里。
    const auto current = sigs_.find(fnNode.name);
    currentFn_ = (current == sigs_.end()) ? nullptr : &current->second;

    // 形参就是这一层的绑定。引用形参的「目标」在函数内部不可知（它在调用方那边），
    // 所以 target 留 -1：既不会误判悬垂，也能让 `fn f(s: &String) -> &String { s }`
    // 这种「把引用原样返回」的通路走通。生命周期标注原样记在绑定上，画图用。
    for (const NodePtr &param : fnNode.params) {
        const bool ref = param->type && param->type->isRef;
        const bool mutRef = param->type && param->type->isRef && param->type->isMut;
        const std::string base = param->type ? param->type->name : "?";
        const int id = declareVar(param->name, base, ref, mutRef, param->line,
                                  /*fnParam=*/true, lifetimeOf(param->type.get()));
        VarInfo &var = result_.vars[static_cast<std::size_t>(id)];
        var.isMut = mutRef;
        var.valueSemantics = false;  // 形参是「外面传进来的东西」，本工具不按拷贝语义处理
    }

    // 函数体直接复用函数作用域（不再套一层），这样形参和函数体第一层变量同框。
    if (fnNode.body && fnNode.body->kind == NodeKind::Block) {
        analyzeBlock(*fnNode.body, /*pushOwnScope=*/false);
    }
    popScope();
    currentFn_ = nullptr;
}

void Analyzer::analyzeBlock(const Node &block, bool pushOwnScope) {
    // 进入 `{`：压栈。压栈之后所有新声明都算这一层，弹栈时一起销毁 ——
    // 这就是「变量生命周期严格限制在块内」的全部实现。
    if (pushOwnScope) pushScope("block", block.line, /*fnScope=*/false);
    for (const NodePtr &stmt : block.stmts) analyzeStmt(*stmt);
    // 尾表达式也是求值：它的值就是整个块的值。
    if (block.tail) analyzeExpr(*block.tail);
    if (pushOwnScope) popScope();
}

void Analyzer::analyzeStmt(const Node &stmt) {
    switch (stmt.kind) {
    case NodeKind::Let:
        analyzeLet(stmt);
        break;
    case NodeKind::Print:
        if (stmt.init) analyzeExpr(*stmt.init);
        break;
    case NodeKind::ExprStmt:
        if (stmt.init) analyzeExpr(*stmt.init);
        break;
    case NodeKind::Block:
        analyzeBlock(stmt, /*pushOwnScope=*/true);
        break;
    default:
        break;
    }
}

void Analyzer::analyzeLet(const Node &letNode) {
    // 先算右值：Rust 也是先算完右边再绑定左边，这个顺序决定了
    // `let s2 = s1;` 里读到的是「移动之前」的 s1。
    const ExprValue value = letNode.init ? analyzeExpr(*letNode.init) : ExprValue{};

    // ---- 情况 A：右值是一个引用（`&x` / `&mut x` / 函数返回的引用）----
    if (value.kind == ExprValue::Kind::Reference) {
        // 生命周期：`let r: &'a T = ...` 写在左值上（文法只给 let 留了 `let r = ...`
        // 这一种形状，所以实际来源是右值的标注），否则沿用被借变量的标注。
        std::string lifetime = value.lifetime;
        // `let r = &x;` / `let r = f(&x)` 这种写法：右值的 ExprValue 没带标注，
        // 但被借变量身上可能挂着别的引用带过来的标注，此时沿用那条边上的 'a。
        if (lifetime.empty()) {
            for (const VarInfo &source : result_.vars) {
                if (source.id == value.varId && source.isRef && !source.lifetime.empty()) {
                    lifetime = source.lifetime;
                    break;
                }
            }
        }
        const int id = declareVar(letNode.name, value.typeName, /*isRef=*/true,
                                  letNode.isMut || value.mutRef, letNode.line, /*fnParam=*/false,
                                  lifetime);
        VarInfo &var = result_.vars[static_cast<std::size_t>(id)];
        var.isMut = letNode.isMut || value.mutRef;

        if (value.dangling) {
            // 悬垂：绑定本身成立了（画图要画），但箭头指向的是「已销毁的东西」。
            var.dangling = true;
            var.target = -1;
            result_.danglingVar = id;
        } else {
            var.target = value.varId;
            // 借用计数只在「真的绑成了一个引用变量」时才加：
            // 临时借用（如 `print(&x)`）不占用生命周期，也就不会误报冲突。
            acquireBorrow(value.varId, id, value.mutRef, letNode.line);
        }
        return;
    }

    // ---- 情况 B：右值是堆对象（`String` / 结构体），语义是所有权转移 ----
    if (value.kind == ExprValue::Kind::Owned) {
        std::string heap = value.note;
        if (value.varId >= 0) {
            // 从另一个变量移过来：源变量必须先「活着」且没被借用。
            VarInfo &src = result_.vars[static_cast<std::size_t>(value.varId)];
            if (src.mutableBorrows > 0 || src.immutableBorrows > 0) {
                addIssue("`" + src.name + "` 正被借用（状态 " + varStateName(src.state) +
                             "），借用期间不能把所有权移走",
                         letNode.line, false, false, src.id, -1);
            }
            src.state = VarState::Moved;  // 原变量从此失效（画成灰色）
            src.heap.clear();             // 堆对象跟着所有权一起走
            MoveEdge move;
            move.from = src.id;
            move.line = letNode.line;
            const int id = declareVar(letNode.name, value.typeName, false, letNode.isMut,
                                      letNode.line, false);
            move.to = id;
            result_.moves.push_back(move);
            result_.vars[static_cast<std::size_t>(id)].heap = heap;
            return;
        }
        // 字面量 / 函数返回值：在调用点新建一个堆对象，所有权直接归新变量。
        const int id = declareVar(letNode.name, value.typeName, false, letNode.isMut,
                                  letNode.line, false);
        result_.vars[static_cast<std::size_t>(id)].heap = heap;
        return;
    }

    // ---- 情况 C：值语义（`int`）—— 直接拷贝，两个变量互不相干 ----
    const std::string typeName = value.typeName.empty() ? "int" : value.typeName;
    const int id = declareVar(letNode.name, typeName, /*isRef=*/false, letNode.isMut,
                              letNode.line, /*fnParam=*/false);
    VarInfo &var = result_.vars[static_cast<std::size_t>(id)];
    var.valueSemantics = true;
    if (value.kind == ExprValue::Kind::Unknown) {
        var.state = VarState::Uninitialized;
    }
}

/* ------------------------------------------------------------------ *
 * 第五步：表达式
 * ------------------------------------------------------------------ */

Analyzer::ExprValue Analyzer::analyzeExpr(const Node &expr) {
    ExprValue out;
    switch (expr.kind) {
    case NodeKind::IntLit:
        out.kind = ExprValue::Kind::Value;
        out.typeName = "int";
        return out;

    case NodeKind::StrLit:
        // 字符串字面量 = 在堆上新建一个 String；所有权归接收它的变量。
        out.kind = ExprValue::Kind::Owned;
        out.typeName = "String";
        out.note = "String \"" + expr.name + "\"";
        return out;

    case NodeKind::VarRef: {
        const int id = lookup(expr.name);
        if (id < 0) {
            addIssue("使用了未定义的变量 `" + expr.name + "`", expr.line, false, false, -1, -1);
            return out;
        }
        VarInfo &var = result_.vars[static_cast<std::size_t>(id)];
        // 「用了已经移走的变量」和「用了已经销毁的变量」是两个不同的错，
        // 分开报：前者是所有权问题，后者是生命周期问题。
        if (var.destroyed) {
            addIssue("使用了已离开作用域的变量 `" + expr.name + "`", expr.line, false, true, id, -1);
        } else if (var.state == VarState::Moved) {
            addIssue("使用了已被移动（move）的变量 `" + expr.name +
                         "`：所有权已经不在它身上了", expr.line, false, false, id, -1);
        }
        if (var.isRef) {
            out.kind = ExprValue::Kind::Reference;
            out.varId = var.target;
            out.mutRef = var.isMut;
            out.typeName = var.typeName;
            out.dangling = var.dangling;
            out.lifetime = var.lifetime;  // 引用转发时，生命周期标注跟着一起走
            return out;
        }
        out.kind = var.valueSemantics ? ExprValue::Kind::Value : ExprValue::Kind::Owned;
        out.varId = id;
        out.typeName = var.typeName;
        out.note = var.heap;
        return out;
    }

    case NodeKind::Borrow: {
        // 借用只在文法里支持 `&变量` / `&mut 变量`，所以 base 一定是 VarRef。
        const Node *base = expr.base.get();
        if (!base || base->kind != NodeKind::VarRef) {
            addIssue("只支持 `&变量` / `&mut 变量` 这种整体借用", expr.line,
                     /*unsupported=*/true, false, -1, -1);
            return out;
        }
        const int id = lookup(base->name);
        if (id < 0) {
            addIssue("借用了未定义的变量 `" + base->name + "`", expr.line, false, false, -1, -1);
            return out;
        }
        // 这里只做「能不能借」的检查（checkBorrow）；真正的借出计数在
        // 绑成引用变量的那一刻（analyzeLet / 调用点）才加，见 acquireBorrow。
        checkBorrow(id, expr.isMut, expr.line);
        const VarInfo &target = result_.vars[static_cast<std::size_t>(id)];
        out.kind = ExprValue::Kind::Reference;
        out.varId = id;
        out.mutRef = expr.isMut;
        out.typeName = target.typeName;
        // 表达式上的借用没有生命周期标注（文法只留了 `&x` 一种形状）；
        // 「'a 从哪儿来」在函数签名那一侧解释，这里不猜。
        return out;
    }

    case NodeKind::Call:
        return analyzeCall(expr, /*valueIsUsed=*/true);

    case NodeKind::FieldAccess: {
        // 结构体只支持**整体**移动 / 整体借用。字段访问意味着「部分移动」或
        // 「字段级借用」，本工具一律拒绝：这是 unsupported，调用方必须中止渲染。
        const std::string base = expr.base ? describeExpr(*expr.base) : std::string("<表达式>");
        addIssue("不支持结构体字段级访问 `" + base + "." + expr.name +
                     "`（部分移动 / 字段借用超出本工具边界）",
                 expr.line, /*unsupported=*/true, false, -1, -1);
        out.kind = ExprValue::Kind::Unknown;
        return out;
    }

    default:
        return out;
    }
}

Analyzer::ExprValue Analyzer::analyzeCall(const Node &call, bool valueIsUsed) {
    ExprValue out;
    std::vector<ExprValue> argv;
    for (const NodePtr &arg : call.args) argv.push_back(analyzeExpr(*arg));

    const auto it = sigs_.find(call.name);
    if (it == sigs_.end()) {
        addIssue("调用了未定义的函数 `" + call.name + "()`", call.line, false, false, -1, -1);
        return out;
    }
    const FnSig &sig = it->second;
    if (argv.size() != sig.paramTypes.size()) {
        addIssue("调用 `" + call.name + "()` 的实参个数与签名不符", call.line, false, false, -1, -1);
    }

    // 找「可绑定的入参」：第一个既是引用形参、实参又是引用表达式的参数。
    // 返回引用的生命周期就跟在这个实参上（Rust 的省略生命周期规则也是这么干的）。
    int bindTarget = -1;
    bool bindMut = false;
    for (std::size_t i = 0; i < argv.size() && i < sig.paramTypes.size(); ++i) {
        if (!sig.paramIsRef[i]) continue;
        if (argv[i].kind != ExprValue::Kind::Reference || argv[i].varId < 0) continue;
        bindTarget = argv[i].varId;
        bindMut = sig.paramIsMutRef[i];
        break;
    }

    CallInfo info;
    info.fn = call.name;
    info.line = call.line;
    info.returnsRef = sig.returnsRef;
    info.lifetime = sig.lifetimeList;
    info.returnLifetime = sig.returnLifetime;
    std::ostringstream signature;
    signature << call.name << sig.lifetimeList << "(";
    for (std::size_t i = 0; i < sig.paramTypes.size(); ++i) {
        if (i) signature << ", ";
        signature << typeSpelling(sig.paramTypes[i], sig.paramIsRef[i], sig.paramIsMutRef[i],
                                  sig.paramLifetimes[i]);
    }
    signature << ") -> "
              << typeSpelling(sig.returnType, sig.returnsRef, sig.returnsMutRef, sig.returnLifetime);
    info.signature = signature.str();
    for (const NodePtr &arg : call.args) info.args.push_back(describeExpr(*arg));

    // 生命周期标注的观察记录：把「声明的 'a 挂在哪个实参上」说清楚。
    // 这里**不判定**合法性（例如返回的 &'a 是否真的来自那个 'a 形参），那是借用检查器的事。
    std::string firstArg;
    int firstArgIndex = -1;
    if (bindTarget >= 0) {
        for (std::size_t i = 0; i < argv.size() && i < sig.paramTypes.size(); ++i) {
            if (sig.paramIsRef[i] && argv[i].varId == bindTarget) {
                firstArgIndex = static_cast<int>(i);
                break;
            }
        }
        if (firstArgIndex >= 0 && firstArgIndex < static_cast<int>(info.args.size())) {
            firstArg = info.args[static_cast<std::size_t>(firstArgIndex)];
        }
    }
    if (!sig.lifetimeList.empty()) {
        std::string note;
        if (bindTarget < 0) {
            note = "声明了 " + sig.lifetimeList + "，但这次调用没有可绑定的引用实参";
        } else if (sig.returnsRef && !sig.returnLifetime.empty()) {
            note = "返回的 " + sig.returnLifetime + " 绑定到第 " +
                   std::to_string(firstArgIndex + 1) + " 个实参 `" + firstArg + "` 上";
        } else {
            note = "把 " + sig.lifetimeList + " 的第一个可绑定实参记作 `" + firstArg + "`";
        }
        addLifetimeNote(call.name, call.line, note);
    }    if (sig.returnsRef) {
        // 判据一：函数体自己就把局部变量返回出去了 —— 无论怎么调用都悬垂。
        // 判据二：签名返回引用，但调用点给不出可绑定的引用实参 —— 生命周期无处可挂。
        const bool dangling = sig.bodyReturnsLocal || bindTarget < 0;
        if (dangling) {
            std::string reason;
            if (sig.bodyReturnsLocal) {
                reason = "`fn " + call.name + "()` 的返回引用指向函数体内的局部变量 `" +
                         sig.localName + "`，函数一返回它就被销毁";
            } else {
                reason = "`fn " + call.name + "()` 返回引用，但调用点没有引用实参可绑定，"
                         "返回的引用没有合法的生命周期";
            }
            addIssue("悬垂指针：" + reason, call.line, /*unsupported=*/false, /*dangling=*/true,
                     -1, -1);
            result_.danglingFn = call.name;
            result_.danglingLocal = sig.localName;
            result_.danglingLine = call.line;
            info.dangling = true;
            out.kind = ExprValue::Kind::Reference;
            out.typeName = sig.returnType;
            out.mutRef = sig.returnsMutRef;
            out.dangling = true;
            out.varId = -1;
        } else {
            // 绑定成功：返回的引用和实参共享生命周期，记一条借用边。
            out.kind = ExprValue::Kind::Reference;
            out.varId = bindTarget;
            out.mutRef = bindMut;
            out.typeName = sig.returnType;
            out.lifetime = sig.returnLifetime;  // 返回类型上标的生命周期，原样转给接收者
        }
        result_.calls.push_back(info);
        return out;
    }

    // 非引用返回：`int` 是值语义；`String` / 结构体是「调用点新建一个堆对象，
    // 所有权交给接收者」。这里不展开函数体，全靠签名 —— 这就是「签名黑盒化」。
    if (!isValueType(sig.returnType)) {
        out.kind = ExprValue::Kind::Owned;
        out.typeName = sig.returnType;
        out.note = sig.returnType + "（`" + call.name + "()` 新建）";
    } else {
        out.kind = ExprValue::Kind::Value;
        out.typeName = sig.returnType;
    }
    if (!valueIsUsed) out.kind = ExprValue::Kind::Value;
    result_.calls.push_back(info);
    return out;
}

/* ------------------------------------------------------------------ *
 * 入口
 * ------------------------------------------------------------------ */

AnalysisResult Analyzer::run() {
    collectDeclarations();
    for (const NodePtr &item : program_.items) {
        if (item->kind == NodeKind::FnDecl) analyzeFunction(*item);
    }
    // 每个函数的泛型参数表原文（没有的话是空串），调用点折行时用。
    for (LifetimeNote &note : result_.lifetimeNotes) {
        const auto it = sigs_.find(note.fn);
        if (it != sigs_.end()) note.declLabel = "fn " + note.fn + it->second.lifetimeList;
    }
    return std::move(result_);
}

} // namespace rustview
