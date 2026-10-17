/**
 * @file visualizer.cpp
 * @brief `Visualizer` 的实现：手写字符串拼 SVG + 包一层 HTML。
 *
 * 版面（坐标都是「画布单位」，SVG 用 viewBox 自适应宽度）：
 *
 *   ┌ 标题 / 状态 ───────────────────────────────────────────────────┐
 *   │ 栈 · 作用域        │ 函数调用栈        │ 堆对象                  │
 *   │  ┌ fn main ──────┐ │ ┌ sig() -> &T ┐  │ ┌ String "hello" ┐     │
 *   │  │ ┌ block ────┐ │ │ └──────────────┘  │ └────────────────┘     │
 *   │  │ │ let a     │ │ │                   │                        │
 *   │  │ └───────────┘ │ │                   │                        │
 *   │  └───────────────┘ │                   │                        │
 *   └ 诊断列表（红色）───────────────────────────────────────────────┘
 *
 * 连线的走法：Move 边与借用边都从「变量框的右边缘」出发，走右侧预留的
 * 几条泳道（lane）绕一圈再回到目标框，这样同一列里的多条边不会糊在一起。
 */
#include "visualizer.hpp"

#include <algorithm>
#include <climits>
#include <sstream>

namespace rustview {
namespace {

/** @brief 与仓库其它总览图一致的字体栈（Windows / Linux / macOS 都能落到中文字体）。 */
constexpr const char *kFont =
    "&#39;Microsoft YaHei&#39;, &#39;Segoe UI&#39;, &#39;Noto Sans SC&#39;, sans-serif";

/* ---- 配色（和仓库其它示意图同一套浅色主题）---- */
constexpr const char *kBackground = "#f4f7fb";
constexpr const char *kPanel = "#ffffff";
constexpr const char *kPanelEdge = "#d8e1ee";
constexpr const char *kTitle = "#10233f";
constexpr const char *kSubtitle = "#4a5b73";
constexpr const char *kPanelTitle = "#14507e";
constexpr const char *kText = "#22364f";
constexpr const char *kNote = "#55677f";
constexpr const char *kBoxFill = "#ffffff";
constexpr const char *kBoxEdge = "#c9dcf0";
constexpr const char *kMovedFill = "#e6e6e6";
constexpr const char *kMovedEdge = "#b9c0c8";
constexpr const char *kMovedText = "#8b939c";
constexpr const char *kImmFill = "#eaf4ff";
constexpr const char *kImmEdge = "#7bb3e8";
constexpr const char *kMutFill = "#fff3e0";
constexpr const char *kMutEdge = "#e8a33d";
constexpr const char *kRefFill = "#f2ecff";
constexpr const char *kRefEdge = "#9b7be8";
constexpr const char *kHeapFill = "#eaf6ef";
constexpr const char *kHeapEdge = "#bfe0cc";
constexpr const char *kDanger = "#d93a3a";
constexpr const char *kDangerFill = "#ffecec";
constexpr const char *kCallFill = "#f7f9fc";
constexpr const char *kCallEdge = "#c9dcf0";
constexpr const char *kScope = "#fbfcfe";
constexpr const char *kLane = "#8ea2bb";
/* 生命周期栏（第四栏）：青色系，和借用边的蓝 / 变量框的紫都区分得开。 */
constexpr const char *kLifeFill = "#eaf7f8";
constexpr const char *kLifeEdge = "#5fabae";
constexpr const char *kLifeText = "#146b6e";
constexpr const char *kWarnFill = "#fff8e6";
constexpr const char *kWarnEdge = "#d9a441";
constexpr const char *kWarnText = "#8a5a00";

/** @brief 版本号 + 维护状态。显示在图表右下角与 HTML 页脚。 */
constexpr const char *kVersion = "1.1";
constexpr const char *kVersionNote =
    "RustView 1.1 · 生命周期标注只画不校验（1.0 版面契约保持不变）";

/** @brief SVG 文本转义：`&` `<` `>` 必须转义，否则中文没问题但这几个符号会炸。 */
std::string xmlEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

/** @brief 数字格式化：整数就不带小数点，避免 SVG 里出现 `40.000000` 这种噪声。 */
std::string fmt(double value) {
    std::ostringstream out;
    if (value == static_cast<long long>(value)) out << static_cast<long long>(value);
    else out.precision(1), out << std::fixed << value;
    return out.str();
}

/** @brief `<rect>`；`id` 非空时给元素加 id（make check 的 grep 锚点用）。 */
std::string rect(double x, double y, double w, double h, const std::string &fill,
                 const std::string &edge = "", double radius = 6, double strokeWidth = 1.0,
                 const std::string &dash = "", const std::string &id = "") {
    std::ostringstream out;
    out << "<rect";
    if (!id.empty()) out << " id=\"" << id << "\"";
    out << " x=\"" << fmt(x) << "\" y=\"" << fmt(y) << "\" width=\"" << fmt(w)
        << "\" height=\"" << fmt(h) << "\" rx=\"" << fmt(radius) << "\" fill=\"" << fill << "\"";
    if (!edge.empty()) out << " stroke=\"" << edge << "\" stroke-width=\"" << fmt(strokeWidth) << "\"";
    if (!dash.empty()) out << " stroke-dasharray=\"" << dash << "\"";
    out << "/>";
    return out.str();
}

/** @brief `<text>`；`anchor` 取 `start` / `middle` / `end`，`bold` 控制字重。 */
std::string text(double x, double y, const std::string &content, double size,
                 const std::string &fill, bool bold = false,
                 const std::string &anchor = "start") {
    std::ostringstream out;
    out << "<text x=\"" << fmt(x) << "\" y=\"" << fmt(y) << "\" font-family=\"" << kFont
        << "\" font-size=\"" << fmt(size) << "\" fill=\"" << fill << "\"";
    if (bold) out << " font-weight=\"700\"";
    if (anchor != "start") out << " text-anchor=\"" << anchor << "\"";
    out << ">" << xmlEscape(content) << "</text>";
    return out.str();
}

/** @brief 三次贝塞尔曲线（连线用），`marker` 是箭头 id，`dash` 是虚线样式。 */
std::string curve(double x1, double y1, double x2, double y2, double lane, const std::string &stroke,
                  const std::string &marker, const std::string &dash = "") {
    std::ostringstream out;
    out << "<path d=\"M " << fmt(x1) << " " << fmt(y1) << " C " << fmt(lane) << " " << fmt(y1)
        << ", " << fmt(lane) << " " << fmt(y2) << ", " << fmt(x2) << " " << fmt(y2)
        << "\" fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\"1.6\"";
    if (!dash.empty()) out << " stroke-dasharray=\"" << dash << "\"";
    if (!marker.empty()) out << " marker-end=\"url(#" << marker << ")\"";
    out << "/>";
    return out.str();
}

/** @brief 悬垂指针的「断线闪电」：一段红色的折线，压在箭头中间。 */
std::string lightning(double cx, double cy) {
    std::ostringstream out;
    out << "<polyline points=\""
        << fmt(cx - 5) << "," << fmt(cy - 9) << " " << fmt(cx + 4) << "," << fmt(cy - 2) << " "
        << fmt(cx - 3) << "," << fmt(cy + 1) << " " << fmt(cx + 5) << "," << fmt(cy + 9)
        << "\" fill=\"none\" stroke=\"" << kDanger << "\" stroke-width=\"2.4\" "
        << "stroke-linejoin=\"round\" stroke-linecap=\"round\"/>";
    return out.str();
}

} // namespace

Visualizer::Visualizer(const AnalysisResult &result, VisualOptions options)
    : result_(result), options_(std::move(options)) {}

std::string Visualizer::statusLine() const {
    if (result_.ok()) {
        std::string out = "分析通过 · " + std::to_string(result_.vars.size()) + " 个绑定、" +
                          std::to_string(result_.borrows.size()) + " 条借用、" +
                          std::to_string(result_.moves.size()) + " 次所有权转移";
        if (result_.hasLifetimes()) out += "、" + std::to_string(result_.calls.size()) +
                                           " 次调用带生命周期标注";
        return out;
    }
    std::size_t conflict = 0, dangling = 0, unsupported = 0, lifetime = 0;
    for (const Issue &issue : result_.issues) {
        if (issue.unsupported) ++unsupported;
        else if (issue.lifetime) ++lifetime;
        else if (issue.dangling) ++dangling;
        else ++conflict;
    }
    std::string out = "分析失败";
    if (conflict) out += " · " + std::to_string(conflict) + " 处借用冲突";
    if (dangling) out += " · " + std::to_string(dangling) + " 处悬垂指针";
    if (lifetime) out += " · " + std::to_string(lifetime) + " 处生命周期标注错误";
    if (unsupported) out += " · " + std::to_string(unsupported) + " 处不支持的结构";
    return out;
}

namespace {

/** @brief 颜色一组的「变量框样式」。 */
struct BoxStyle {
    std::string fill;
    std::string edge;
    std::string textColor;
    bool dashed = false;
};

/** @brief 按变量状态决定框的配色，顺带把「已移动 / 已借出」写进说明文字。
 *  @param destroyed 是否把这次「销毁」画出来：函数自己那一层作用域的销毁发生在
 *         快照点之后（见 renderSvg 里的说明），所以调用方会传 false。 */
BoxStyle styleOf(const VarInfo &var, bool destroyed, bool conflict, bool dangling) {
    BoxStyle style;
    style.fill = kBoxFill;
    style.edge = kBoxEdge;
    style.textColor = kText;
    if (var.isRef) {
        style.fill = kRefFill;
        style.edge = kRefEdge;
    }
    if (var.state == VarState::Moved || destroyed) {
        style.fill = kMovedFill;
        style.edge = kMovedEdge;
        style.textColor = kMovedText;
    } else if (var.state == VarState::BorrowedMutable) {
        style.fill = kMutFill;
        style.edge = kMutEdge;
    } else if (var.state == VarState::BorrowedImmutable) {
        style.fill = kImmFill;
        style.edge = kImmEdge;
    }
    if (dangling) {
        style.fill = kDangerFill;
        style.edge = kDanger;
        style.textColor = kDanger;
        style.dashed = true;
    }
    if (conflict) {
        style.edge = kDanger;
        style.fill = kDangerFill;
        style.textColor = kDanger;
    }
    return style;
}

/** @brief 变量框里的第二行（类型 / 状态 / 指向 / 堆对象）。
 *  @param destroyed 见 `styleOf()`：只有内层块退出导致的销毁才写成「已销毁」。
 *  带生命周期标注的引用把标注一起写出来（`&'a String · Initialized`），
 *  形状和源码里的 `&'a String` 一致。 */
std::string metaOf(const VarInfo &var, bool destroyed) {
    std::string meta = var.typeName.empty() ? "?" : var.typeName;
    if (var.isRef) meta = "&" + (var.lifetime.empty() ? "" : var.lifetime + " ") + meta;
    if (destroyed) return meta + " · 已销毁";
    if (var.state == VarState::Moved) return meta + " · 已移动";
    if (var.state == VarState::BorrowedMutable) return meta + " · 可变借出";
    if (var.state == VarState::BorrowedImmutable) return meta + " · 只读借出";
    if (var.dangling) return meta + " · 悬垂";
    return meta + " · " + varStateName(var.state);
}

} // namespace

std::string Visualizer::renderSvg() const {
    /* ---------------- 布局常量 ----------------
     * 出现生命周期标注时把画布加宽 440，多出第四栏（右边缘仍留 40 的边距），
     * 前三栏的坐标一动不动 —— 冻结的版面照旧，只是右边多一列注解。 */
    const bool showLife = result_.hasLifetimes();
    const double lifeW = showLife ? 440 : 0;
    const double stackX = 40, stackW = 380;
    const double callX = 470, callW = 340;
    const double heapX = 850, heapW = 430;
    const double lifeX = 1320;
    const double canvasW = 1320 + lifeW;
    const double panelTop = 92;
    const double bodyTop = 132;
    const double rowH = 48;
    const double boxH = 38;
    const double laneBase = stackX + stackW + 6;

    /* ---------------- 生命周期栏的行分配 ----------------
     * 上半截：声明了生命周期参数的函数，一个函数一行（`fn first<'a>`）。
     * 下半截：带标注的调用点，一次调用一行（`'a` 绑定到哪个实参）。
     * 两截之间留一行画分隔线，行号从 0 起（和左边三栏同一套坐标系）。 */
    std::vector<std::string> lifeDecls;
    for (const CallInfo &call : result_.calls) {
        if (call.lifetime.empty()) continue;
        const std::string label = "fn " + call.fn + call.lifetime;
        if (std::find(lifeDecls.begin(), lifeDecls.end(), label) == lifeDecls.end()) {
            lifeDecls.push_back(label);
        }
    }
    std::vector<LifetimeNote> lifeNotes;
    for (const LifetimeNote &note : result_.lifetimeNotes) {
        const bool dup = std::any_of(lifeNotes.begin(), lifeNotes.end(),
                                     [&](const LifetimeNote &other) {
                                         return other.fn == note.fn && other.line == note.line;
                                     });
        if (!dup) lifeNotes.push_back(note);
    }
    const int lifeDeclRows = static_cast<int>(lifeDecls.size());
    const int lifeNoteRows = static_cast<int>(lifeNotes.size());
    const int lifeTotalRows = showLife ? lifeDeclRows + (lifeNoteRows ? lifeNoteRows + 1 : 0) : 0;

    /* ---------------- 只画入口函数 `main` 的作用域 ----------------
     * 别的函数（get_string / borrow_and_return / dangling）的形参与局部变量
     * 画出来只会让图变糊；它们的**调用**照样画在中间一栏。 */
    int mainScope = -1;
    for (std::size_t i = 0; i < result_.scopes.size(); ++i) {
        if (result_.scopes[i].fnScope && result_.scopes[i].label == "fn main") {
            mainScope = static_cast<int>(i);
            break;
        }
    }
    auto parentOf = [&](int scope) {
        if (scope < 0 || result_.scopes[static_cast<std::size_t>(scope)].depth == 0) return -1;
        for (int j = scope - 1; j >= 0; --j) {
            if (result_.scopes[static_cast<std::size_t>(j)].depth ==
                result_.scopes[static_cast<std::size_t>(scope)].depth - 1) {
                return j;
            }
        }
        return -1;
    };
    auto inMain = [&](int scope) {
        if (mainScope < 0) return true;
        for (int s = scope; s >= 0; s = parentOf(s)) {
            if (s == mainScope) return true;
        }
        return false;
    };

    /* ---------------- 行号分配（每个作用域先占一行标题） ----------------
     * 每个作用域先占**一行**画标题（「fn main（第 4 行）」「block（第 6 行）」…），
     * 再从下一行起按声明顺序摆变量。好处有两个：
     *   ① 标题永远不会压在变量框上（之前两者抢同一行，糊成一团）；
     *   ② 作用域矩形从自己的标题行一直包到子树最后一个变量，块嵌套在图上就是
     *      干净的「矩形套矩形」。
     * 行号是全局递增的：外层作用域的标题先出现，内层块的标题后出现。 */
    std::vector<int> row(result_.vars.size(), -1);
    std::vector<int> headerRow(result_.scopes.size(), -1);
    auto chainToMain = [&](int scope) {          // 从最外层到 scope 的作用域链
        std::vector<int> chain;
        for (int s = scope; s >= 0; s = parentOf(s)) chain.push_back(s);
        std::reverse(chain.begin(), chain.end());
        return chain;
    };
    int cursor = 0;
    for (std::size_t i = 0; i < result_.vars.size(); ++i) {
        if (!inMain(result_.vars[i].scope)) continue;
        // 变量出现之前，把它那一串还没露过面的祖先作用域标题先排进来（外层在前）。
        for (int s : chainToMain(result_.vars[i].scope)) {
            if (headerRow[static_cast<std::size_t>(s)] < 0) {
                headerRow[static_cast<std::size_t>(s)] = cursor++;
            }
        }
        row[i] = cursor++;
    }
    const int rows = cursor;
    std::vector<int> scopeMin(result_.scopes.size(), INT_MAX);
    std::vector<int> scopeMax(result_.scopes.size(), -1);
    for (std::size_t i = 0; i < result_.vars.size(); ++i) {
        if (row[i] < 0) continue;
        for (int s = result_.vars[i].scope; s >= 0; s = parentOf(s)) {
            scopeMin[static_cast<std::size_t>(s)] = std::min(scopeMin[static_cast<std::size_t>(s)], row[i]);
            scopeMax[static_cast<std::size_t>(s)] = std::max(scopeMax[static_cast<std::size_t>(s)], row[i]);
        }
    }
    // 标题行也要算进作用域范围：矩形上边就从标题行开始，而不是第一个变量。
    for (std::size_t s = 0; s < result_.scopes.size(); ++s) {
        if (headerRow[s] < 0) continue;
        scopeMin[s] = std::min(scopeMin[s], headerRow[s]);
        scopeMax[s] = std::max(scopeMax[s], headerRow[s]);
    }

    /* ---------------- 变量框坐标 ---------------- */
    std::vector<Box> boxes(result_.vars.size());
    for (std::size_t i = 0; i < result_.vars.size(); ++i) {
        if (row[i] < 0) continue;
        const int depth = result_.vars[i].scope >= 0
                              ? result_.scopes[static_cast<std::size_t>(result_.vars[i].scope)].depth
                              : 0;
        const double inset = 14.0 * depth;
        boxes[i].x = stackX + 12 + inset;
        boxes[i].w = stackW - 34 - inset;
        boxes[i].y = bodyTop + row[i] * rowH + (rowH - boxH) / 2;
        boxes[i].h = boxH;
    }

    /* ---------------- 冲突 / 悬垂高亮集合 ---------------- */
    std::vector<bool> conflict(result_.vars.size(), false);
    std::vector<bool> dangling(result_.vars.size(), false);
    for (const Issue &issue : result_.issues) {
        if (issue.focus >= 0 && issue.focus < static_cast<int>(conflict.size())) {
            if (issue.dangling) dangling[static_cast<std::size_t>(issue.focus)] = true;
            else conflict[static_cast<std::size_t>(issue.focus)] = true;
        }
        if (issue.focus2 >= 0 && issue.focus2 < static_cast<int>(conflict.size())) {
            conflict[static_cast<std::size_t>(issue.focus2)] = true;
        }
    }

    /* ---------------- 堆对象框坐标 ---------------- */
    std::vector<int> heapVars;
    for (std::size_t i = 0; i < result_.vars.size(); ++i) {
        if (!result_.vars[i].heap.empty() && inMain(result_.vars[i].scope)) {
            heapVars.push_back(static_cast<int>(i));
        }
    }
    std::vector<Box> heapBoxes(result_.vars.size());
    for (std::size_t k = 0; k < heapVars.size(); ++k) {
        Box &box = heapBoxes[static_cast<std::size_t>(heapVars[k])];
        box.x = heapX + 12;
        box.w = heapW - 24;
        box.y = bodyTop + static_cast<double>(k) * rowH + (rowH - boxH) / 2;
        box.h = boxH;
    }

    /* ---------------- 调用栈栏的行游标 ----------------
     * 普通调用卡高 `boxH`、占一个 `rowH` 行距（与 1.0 一致）；
     * 带生命周期标注的卡要放四行字（名字 / 签名 / 实参 / 绑定说明），
     * 卡高 `lifeCardH`、占 `lifeCardRows` 行距——必须**各自用游标累加**，
     * 不能共用一个行号，否则高卡会压到下面那张卡上（那正是之前的比例问题）。 */
    const double lifeCardH = 66;
    const double lifeCardGap = 8;
    const int lifeCardRowSpan = static_cast<int>((lifeCardH + lifeCardGap) / rowH) + 1;
    std::vector<double> callY(result_.calls.size(), 0.0);
    std::vector<double> callH(result_.calls.size(), boxH);
    int callRows = 0;
    for (std::size_t i = 0; i < result_.calls.size(); ++i) {
        const bool tall = !result_.calls[i].lifetime.empty();
        const double h = tall ? lifeCardH : boxH;
        callH[i] = h;
        // 卡片在行带里垂直居中；高卡的行带足够高，不会越界。
        callY[i] = bodyTop + static_cast<double>(callRows) * rowH +
                   (tall ? 4.0 : (rowH - boxH) / 2);
        callRows += tall ? lifeCardRowSpan : 1;
    }

    /* ---------------- 画布高度 ---------------- */
    const int contentRows = std::max<int>({rows, callRows, static_cast<int>(heapVars.size()),
                                           lifeTotalRows, 1});
    const double bodyBottom = bodyTop + contentRows * rowH + 16;
    const int issueCount = static_cast<int>(result_.issues.size());
    const bool showDangling = !result_.danglingLocal.empty();
    const double danglingY = bodyBottom + 12;
    const double danglingH = showDangling ? 64 : 0;
    const double issueTop = danglingY + danglingH + 18;
    const double canvasH = issueTop + (issueCount ? issueCount * 22.0 + 34 : 30) + 22;

    /* ---------------- 拼 SVG ---------------- */
    std::ostringstream svg;
    svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << fmt(canvasW) << " "
        << fmt(canvasH) << "\" width=\"" << fmt(canvasW) << "\" height=\"" << fmt(canvasH)
        << "\" font-family=\"" << kFont << "\">\n";

    svg << "<defs>";
    const char *markers[4] = {"rv-arrow-move", "rv-arrow-imm", "rv-arrow-mut", "rv-arrow-bad"};
    const char *markerColors[4] = {"#6b7c93", "#2f7fd1", "#e08a1e", kDanger};
    for (int i = 0; i < 4; ++i) {
        svg << "<marker id=\"" << markers[i]
            << "\" viewBox=\"0 0 10 10\" refX=\"9\" refY=\"5\" markerWidth=\"7\" markerHeight=\"7\" "
               "orient=\"auto\"><path d=\"M 0 0 L 10 5 L 0 10 z\" fill=\""
            << markerColors[i] << "\"/></marker>";
    }
    svg << "</defs>\n";

    svg << rect(0, 0, canvasW, canvasH, kBackground, "", 0);

    /* ---- 标题条 ---- */
    svg << text(40, 46, options_.title, 24, kTitle, true);
    std::string subtitle = options_.sourceName;
    if (!subtitle.empty()) subtitle += " · ";
    subtitle += statusLine();
    svg << text(40, 70, subtitle, 13, result_.ok() ? kSubtitle : kDanger, false);
    // 右上角状态胶囊
    const std::string badge = result_.ok() ? "SUCCESS · 无冲突" : "FAILED · 已阻止渲染";
    const std::string badgeFill = result_.ok() ? "#e8f6ef" : kDangerFill;
    const std::string badgeEdge = result_.ok() ? "#8fc9a9" : kDanger;
    const std::string badgeText = result_.ok() ? "#1e6b46" : kDanger;
    svg << rect(canvasW - 40 - 170, 24, 170, 30, badgeFill, badgeEdge, 15, 1.4);
    svg << text(canvasW - 40 - 85, 44, badge, 13, badgeText, true, "middle");

    /* ---- 三栏面板 ---- */
    svg << rect(stackX, panelTop, stackW, bodyBottom - panelTop, kPanel, kPanelEdge, 12);
    svg << rect(callX, panelTop, callW, bodyBottom - panelTop, kPanel, kPanelEdge, 12);
    svg << rect(heapX, panelTop, heapW, bodyBottom - panelTop, kPanel, kPanelEdge, 12);
    svg << text(stackX + 14, panelTop + 26, "栈 · 作用域", 15, kPanelTitle, true);
    svg << text(callX + 14, panelTop + 26, "函数调用栈", 15, kPanelTitle, true);
    svg << text(heapX + 14, panelTop + 26, "堆对象", 15, kPanelTitle, true);
    if (showLife) {
        // "lifetime-panel" 是给 make check 的 grep 断言用的锚点（id 对渲染无影响）。
        svg << rect(lifeX, panelTop, lifeW, bodyBottom - panelTop, kPanel, kPanelEdge, 12,
                    1.0, "", "lifetime-panel");
        svg << text(lifeX + 14, panelTop + 26, "生命周期标注", 15, kPanelTitle, true);
        svg << text(lifeX + lifeW - 14, panelTop + 26, "只解释标注，不做合法性校验", 10, kNote,
                    false, "end");
    }

    /* ---- 作用域矩形（先画外层的，再画里层变量，保证变量压在框上面）---- */
    for (std::size_t s = 0; s < result_.scopes.size(); ++s) {
        if (!inMain(static_cast<int>(s)) || scopeMax[s] < 0) continue;
        const ScopeInfo &scope = result_.scopes[s];
        const double inset = 14.0 * scope.depth;
        const double x = stackX + 8 + inset;
        const double w = stackW - 16 - inset * 1.0 - 14;
        const double y = bodyTop + scopeMin[s] * rowH - 6;
        const double h = (scopeMax[s] - scopeMin[s] + 1) * rowH + 4;
        svg << rect(x, y, w, h, kScope, kPanelEdge, 8, scope.fnScope ? 1.6 : 1.0);
        // 标题行下面压一条浅色分隔线，把「作用域名」和「里面的变量」分开。
        const double sepY = y + rowH - 2;
        svg << "<line x1=\"" << fmt(x + 6) << "\" y1=\"" << fmt(sepY) << "\" x2=\"" << fmt(x + w - 6)
            << "\" y2=\"" << fmt(sepY) << "\" stroke=\"" << kPanelEdge << "\" stroke-width=\"1\"/>";
        svg << text(x + 10, y + rowH - 15, scope.label + "（第 " + std::to_string(scope.line) + " 行）",
                    12, scope.fnScope ? kPanelTitle : kNote, scope.fnScope);
    }

    /* ---- 变量框 ---- */
    for (std::size_t i = 0; i < result_.vars.size(); ++i) {
        if (row[i] < 0) continue;
        const VarInfo &var = result_.vars[i];
        const bool isConflict = conflict[i];
        const bool isDangling = dangling[i] || var.dangling;
        // 快照点取「入口函数 main 返回之前」：函数自己那一层作用域此刻还没拆，
        // 所以不把它的 destroyed 画成灰色。只有**内层块**退出时被销毁的变量
        // （例如 `{ let x = ...; }` 里的 x）才显示「已销毁」——那正是块作用域的
        // 教学点。悬垂指针另有红色高亮 + 底部断裂箭头，不受这里影响。
        const bool shownDestroyed =
            var.destroyed && !(var.scope >= 0 &&
                               result_.scopes[static_cast<std::size_t>(var.scope)].fnScope);
        const BoxStyle style = styleOf(var, shownDestroyed, isConflict, isDangling);
        const Box &box = boxes[i];
        svg << rect(box.x, box.y, box.w, box.h, style.fill, style.edge, 7,
                    isConflict || isDangling ? 2.2 : 1.0, style.dashed ? "5 3" : "");
        svg << text(box.x + 12, box.y + 17, var.name + (var.fnParam ? "（形参）" : ""), 13,
                    style.textColor, true);
        svg << text(box.x + 12, box.y + 32, metaOf(var, shownDestroyed), 11, style.textColor);
        if (isConflict) {
            svg << text(box.x + box.w - 10, box.y + 17, "冲突", 11, kDanger, true, "end");
        }
        if (isDangling) {
            svg << text(box.x + box.w - 10, box.y + 17, "悬垂", 11, kDanger, true, "end");
        }
    }

    /* ---- 堆对象框 ---- */
    for (std::size_t k = 0; k < heapVars.size(); ++k) {
        const int id = heapVars[k];
        const VarInfo &var = result_.vars[static_cast<std::size_t>(id)];
        const Box &box = heapBoxes[static_cast<std::size_t>(id)];
        svg << rect(box.x, box.y, box.w, box.h, kHeapFill, kHeapEdge, 7);
        svg << text(box.x + 12, box.y + 17, var.heap, 13, "#1e6b46", true);
        svg << text(box.x + 12, box.y + 32, "所有者：" + var.name + "（第 " + std::to_string(var.line) +
                                             " 行创建）",
                    11, "#3f6c53");
        // 栈里的变量 -> 堆对象：一条细实线，表示「谁拿着这块内存」。
        const Box &owner = boxes[static_cast<std::size_t>(id)];
        svg << curve(owner.x + owner.w, owner.cy(), box.x, box.cy(), stackX + stackW + 130, "#9bb8d4", "");
    }

    /* ---- Move 边：旧变量 -> 新变量，实线灰箭头 ---- */
    for (const MoveEdge &move : result_.moves) {
        if (move.from < 0 || move.to < 0) continue;
        const Box &from = boxes[static_cast<std::size_t>(move.from)];
        const Box &to = boxes[static_cast<std::size_t>(move.to)];
        if (row[static_cast<std::size_t>(move.from)] < 0 || row[static_cast<std::size_t>(move.to)] < 0) continue;
        svg << curve(from.x + from.w, from.cy(), to.x + to.w, to.cy(), laneBase + 10, "#6b7c93",
                     "rv-arrow-move");
        svg << text(laneBase + 14, (from.cy() + to.cy()) / 2 - 4,
                    "move（所有权转移，第 " + std::to_string(move.line) + " 行）", 10, "#6b7c93");
    }

    /* ---- 借用边：虚线彩色箭头，`&` 蓝、`&mut` 橙 ---- */
    int borrowLane = 0;
    for (const BorrowEdge &edge : result_.borrows) {
        if (edge.from < 0 || edge.to < 0) continue;
        const Box &from = boxes[static_cast<std::size_t>(edge.from)];
        const Box &to = boxes[static_cast<std::size_t>(edge.to)];
        if (row[static_cast<std::size_t>(edge.from)] < 0 || row[static_cast<std::size_t>(edge.to)] < 0) continue;
        const double lane = laneBase + 24 + (borrowLane % 4) * 9;
        // 三种画法：
        //   ① 触发了冲突的借用 —— 红色实线 + 红色箭头，哪怕它已经随作用域释放，
        //      冲突这件事也必须留在图上（这是这张图最想告诉读者的东西）；
        //   ② 已随作用域释放、又没冲突的借用 —— 浅灰细虚线、不带箭头
        //      （「当时借过、现在已还」）；
        //   ③ 仍然生效的借用 —— 彩色虚线带箭头（`&` 蓝、`&mut` 橙）。
        const bool hot = (edge.from < static_cast<int>(conflict.size()) && conflict[edge.from]) ||
                         (edge.to < static_cast<int>(conflict.size()) && conflict[edge.to]);
        std::string stroke;
        std::string marker;
        std::string dash;
        if (hot) {
            stroke = kDanger;
            marker = "rv-arrow-bad";
            dash = "6 3";
        } else if (edge.released) {
            stroke = "#b9c6d6";
            marker.clear();
            dash = "3 4";
        } else {
            stroke = edge.mut ? "#e08a1e" : "#2f7fd1";
            marker = edge.mut ? "rv-arrow-mut" : "rv-arrow-imm";
            dash = "6 4";
        }
        svg << curve(from.x + from.w, from.cy(), to.x + to.w, to.cy(), lane, stroke, marker, dash);
        ++borrowLane;
    }

    /* ---- 函数调用栈 ---- */
    for (std::size_t i = 0; i < result_.calls.size(); ++i) {
        const CallInfo &call = result_.calls[i];
        const double y = callY[i];
        std::string args;
        for (std::size_t a = 0; a < call.args.size(); ++a) {
            if (a) args += ", ";
            args += call.args[a];
        }
        if (!call.lifetime.empty()) {
            // 带生命周期标注的调用：卡片高 `lifeCardH`（四行字：名字 / 签名 / 实参 / 绑定说明），
            // 青色边框，让读者把「这次调用的 'a 绑给了谁」和右边第四栏对上。
            const LifetimeNote *note = nullptr;
            for (const LifetimeNote &candidate : lifeNotes) {
                if (candidate.fn == call.fn && candidate.line == call.line) note = &candidate;
            }
            const bool warn = note && note->warning;
            const std::string edge = warn ? kWarnEdge : call.dangling ? kDanger : kLifeEdge;
            const std::string fill = warn ? kWarnFill : call.dangling ? kDangerFill : kLifeFill;
            svg << rect(callX + 12, y, callW - 24, callH[i], fill, edge, 7,
                        call.dangling || warn ? 2.0 : 1.2);
            svg << text(callX + 24, y + 16, call.fn + "()", 13,
                        call.dangling ? kDanger : kLifeText, true);
            // 签名可能很长（`longer<'a, 'b: 'a>(&'a String, &'b String) -> &'a String`），
            // 字号压到 9.5，正好落在卡片内宽里，不会被裁掉。
            svg << text(callX + 24, y + 31, call.signature, 9.5, kNote);
            if (!call.args.empty()) svg << text(callX + 24, y + 46, "实参：" + args, 10, kNote);
            if (note) {
                svg << text(callX + 24, y + 60, note->note, 9.5, warn ? kWarnText : kLifeText);
            }
            if (call.dangling) {
                svg << text(callX + callW - 24, y + 16, "悬垂", 11, kDanger, true, "end");
            }
            continue;
        }
        const std::string edge = call.dangling ? kDanger : kCallEdge;
        svg << rect(callX + 12, y, callW - 24, boxH, call.dangling ? kDangerFill : kCallFill, edge, 7,
                    call.dangling ? 2.0 : 1.0);
        svg << text(callX + 24, y + 17, call.fn + "()", 13, call.dangling ? kDanger : kText, true);
        svg << text(callX + 24, y + 32, call.signature, 10, kNote);
        if (!args.empty()) {
            svg << text(callX + callW - 24, y + 32, "实参：" + args, 10, kNote, false, "end");
        }
        if (call.dangling) {
            svg << text(callX + callW - 24, y + 17, "悬垂", 11, kDanger, true, "end");
        }
    }

    /* ---- 生命周期栏：上半截是函数声明的参数表，下半截是调用的绑定关系 ---- */
    if (showLife) {
        int lifeRow = 0;
        for (const std::string &decl : lifeDecls) {
            const double y = bodyTop + static_cast<double>(lifeRow++) * rowH + (rowH - boxH) / 2;
            svg << rect(lifeX + 12, y, lifeW - 24, boxH, kLifeFill, kLifeEdge, 7, 1.2);
            svg << text(lifeX + 24, y + 17, decl, 13, kLifeText, true);
            svg << text(lifeX + 24, y + 32,
                        "声明的生命周期参数：引用可以活多久，由调用方决定", 10, kNote);
        }
        if (lifeNoteRows) {
            // 分隔线：上面是「声明」，下面是「每次调用把 'a 绑到谁身上」。
            const double sepY = bodyTop + static_cast<double>(lifeDeclRows) * rowH + 6;
            svg << "<line x1=\"" << fmt(lifeX + 12) << "\" y1=\"" << fmt(sepY) << "\" x2=\""
                << fmt(lifeX + lifeW - 12) << "\" y2=\"" << fmt(sepY) << "\" stroke=\"" << kPanelEdge
                << "\" stroke-width=\"1\"/>";
            svg << text(lifeX + 14, sepY + 22, "调用点：'a 绑定到谁", 11, kPanelTitle, true);
            lifeRow = lifeDeclRows + 1;
            for (const LifetimeNote &note : lifeNotes) {
                const double y = bodyTop + static_cast<double>(lifeRow++) * rowH + (rowH - boxH) / 2;
                const std::string edge = note.warning ? kWarnEdge : kLifeEdge;
                const std::string fill = note.warning ? kWarnFill : kLifeFill;
                const std::string ink = note.warning ? kWarnText : kLifeText;
                svg << rect(lifeX + 12, y, lifeW - 24, boxH, fill, edge, 7, 1.0);
                svg << text(lifeX + 24, y + 16, note.declLabel + "()", 11.5, ink, true);
                svg << text(lifeX + 24, y + 31, "第 " + std::to_string(note.line) + " 行：" +
                                                    note.note,
                            10, ink);
            }
        }
    }

    /* ---- 悬垂示意：被销毁的局部变量 + 断裂的箭头 ---- */
    if (showDangling) {
        const double dy = danglingY;
        svg << rect(stackX, dy, canvasW - 2 * stackX, danglingH - 8, kDangerFill, kDanger, 10, 1.8, "7 4");
        svg << text(stackX + 16, dy + 24,
                    "悬垂指针：`fn " + result_.danglingFn + "()` 在函数退出时销毁了局部变量 `" +
                        result_.danglingLocal + "`",
                    13, kDanger, true);
        svg << text(stackX + 16, dy + 44,
                    "第 " + std::to_string(result_.danglingLine) + " 行的调用把它的引用带了出来 —— " +
                        "引用指向的是一块已经失效的栈内存（箭头已经断掉）",
                    11, "#8a2f2f");
        const double lx = canvasW - stackX - 150;
        const double ly = dy + 30;
        svg << rect(lx, dy + 10, 140, 34, "#ffffff", kDanger, 7, 1.6, "5 3");
        svg << text(lx + 70, dy + 32, result_.danglingLocal + "（已销毁）", 12, kDanger, true, "middle");
        svg << curve(lx, ly, lx - 30, ly, lx - 20, kDanger, "rv-arrow-bad", "5 4");
        svg << lightning(lx - 20, ly);
    }

    /* ---- 诊断列表 ---- */
    if (issueCount) {
        svg << text(40, issueTop + 6, "诊断（控制台同样会打印这些内容）", 13, kDanger, true);
        for (std::size_t i = 0; i < result_.issues.size(); ++i) {
            const Issue &issue = result_.issues[i];
            const double y = issueTop + 30 + static_cast<double>(i) * 22;
            std::string kind = issue.unsupported ? "不支持"
                               : issue.lifetime  ? "生命周期"
                               : issue.dangling  ? "悬垂"
                                                 : "冲突";
            svg << text(40, y, "· [第 " + std::to_string(issue.line) + " 行 · " + kind + "] " +
                                  issue.message,
                        12, issue.unsupported ? "#8a2f2f" : kDanger);
        }
    } else {
        svg << text(40, issueTop + 6,
                    "本示例没有借用冲突：值语义各自独立，Move 之后原变量已失效，借用关系合法。（快照点：入口函数 main 返回之前；内层块退出后其变量标灰为「已销毁」）",
                    12, "#1e6b46");
        if (showLife) {
            svg << text(40, issueTop + 28,
                        "生命周期标注：`fn f<'a>(s: &'a String) -> &'a String` 读作「返回的引用和 s 活得一样久」；"
                        "第四栏列出声明的 'a 在每次调用里绑定到哪个实参。本工具只画标注，不校验它是否成立。",
                        11, kLifeText);
        }
    }

    // 右下角版本戳：图是谁生成的、什么维护状态，一眼可见。
    svg << text(canvasW - 40, canvasH - 9, kVersionNote, 10, kNote, false, "end");

    svg << "</svg>\n";
    return svg.str();
}

std::string Visualizer::renderHtml(const std::string &svg) const {
    const bool ok = result_.ok();
    std::ostringstream html;
    html << "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
         << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
         << "<title>" << xmlEscape(options_.title) << "</title>\n<style>\n"
         << ":root{--bg:#f4f7fb;--panel:#fff;--line:#d8e1ee;--ink:#10233f;--note:#55677f;"
            "--bad:#d93a3a;--ok:#1e6b46}\n"
         << "*{box-sizing:border-box}\n"
         << "body{margin:0;background:var(--bg);color:var(--ink);"
            "font-family:'Microsoft YaHei','Segoe UI','Noto Sans SC',system-ui,sans-serif}\n"
         << ".wrap{max-width:1360px;margin:0 auto;padding:24px}\n"
         << "header{display:flex;align-items:center;gap:14px;padding:16px 20px;border-radius:12px;"
            "border:1px solid var(--line);background:var(--panel)}\n"
         << "header.ok{border-left:6px solid var(--ok)}\n"
         << "header.bad{border-left:6px solid var(--bad)}\n"
         << "header h1{margin:0;font-size:18px}\n"
         << "header .status{margin-left:auto;font-weight:700;font-size:14px}\n"
         << "header.ok .status{color:var(--ok)}\n"
         << "header.bad .status{color:var(--bad)}\n"
         << ".meta{margin:6px 0 0;color:var(--note);font-size:13px}\n"
         << ".canvas{margin:18px 0;padding:8px;border-radius:12px;border:1px solid var(--line);"
            "background:var(--panel);overflow:auto}\n"
         << ".canvas svg{display:block;max-width:100%;height:auto}\n"
         << "section.issues{padding:14px 18px;border-radius:12px;border:1px solid var(--line);"
            "background:var(--panel)}\n"
         << "section.issues h2{margin:0 0 8px;font-size:15px}\n"
         << "ul{margin:0;padding-left:20px}\n"
         << "li{margin:4px 0;font-size:13px;line-height:1.6}\n"
         << "li.unsupported{color:#8a2f2f;font-weight:700}\n"
         << "li.error{color:var(--bad)}\n"
         << "li.life{color:#146b6e}\n"
         << "li.ok{color:var(--ok)}\n"
         << "footer{margin-top:14px;color:var(--note);font-size:12px}\n"
         << "</style>\n</head>\n<body>\n<div class=\"wrap\">\n";

    html << "  <header class=\"" << (ok ? "ok" : "bad") << "\">\n"
         << "    <div><h1>" << xmlEscape(options_.title) << "</h1>\n"
         << "    <p class=\"meta\">" << xmlEscape(options_.sourceName) << "</p></div>\n"
         << "    <div class=\"status\">" << (ok ? "成功 · 无冲突" : "失败 · 已阻止渲染") << "</div>\n"
         << "  </header>\n";

    html << "  <div class=\"canvas\">\n" << svg << "  </div>\n";

    html << "  <section class=\"issues\">\n    <h2>分析结果：" << xmlEscape(statusLine())
         << "</h2>\n    <ul>\n";
    if (result_.issues.empty()) {
        html << "      <li class=\"ok\">没有借用冲突：值语义各自独立，Move 之后原变量已失效，"
                "借用关系合法。</li>\n";
        if (result_.hasLifetimes()) {
            html << "      <li class=\"life\">生命周期标注只做解释：图上第四栏列出每个函数声明的参数表，"
                    "以及每次调用把 'a 绑定到哪个实参；合法性（借用检查器的活）本工具不校验。</li>\n";
        }
    } else {
        for (const Issue &issue : result_.issues) {
            html << "      <li class=\""
                 << (issue.unsupported ? "unsupported" : issue.lifetime ? "life" : "error")
                 << "\">第 " << issue.line << " 行：";
            if (issue.unsupported) html << "【不支持的结构】";
            else if (issue.lifetime) html << "【生命周期】";
            else if (issue.dangling) html << "【悬垂指针】";
            else html << "【借用冲突】";
            html << xmlEscape(issue.message) << "</li>\n";
        }
    }
    html << "    </ul>\n  </section>\n";
    html << "  <footer>由 RustView " << kVersion << " 生成："
            "文法由本仓库的 ParseEngine 从 <code>rust_subset.gram</code> 生成"
            "（IELR(1) / C++17），图表是纯字符串拼接的 SVG。"
            "生命周期标注只画不校验；1.0 的版面契约保持不变。</footer>\n";
    html << "</div>\n</body>\n</html>\n";
    return html.str();
}

} // namespace rustview
