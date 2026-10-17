/**
 * @file visualizer.hpp
 * @brief `Visualizer`：把 `AnalysisResult` 拼成 SVG，再包成 HTML 页面。
 *
 * 为什么手写字符串拼 SVG：本工具的硬边界是「零第三方依赖」，不能引 Graphviz、
 * 也不能引任何绘图库。SVG 本身是文本格式，节点不多时直接拼反而最省事、
 * 最容易调试（生成的 `.svg` 用浏览器打开就能看，也能直接进版本库做对照）。
 *
 * 版面分三栏（从左到右）：
 *   - **栈**（作用域）：一层套一层的矩形框，框里是这一层声明的变量。
 *     框的嵌套关系直接对应源码里 `{ }` 的嵌套，离开块就消失的变量一眼能看出来。
 *   - **函数调用栈**：每次调用画一个「签名节点」，显示参数怎么借、返回值绑给谁。
 *   - **堆**：`String` / 结构体这类「引用语义」的值画成堆对象；
 *     值语义的 `int` 不占堆，只在栈里有一个方块。
 *
 * 源码里出现生命周期标注（`fn f<'a>` / `&'a T`）时，画布会变宽并在右边多出第四栏
 * **生命周期**：上半截列出函数声明的生命周期参数表，下半截列出每个带标注的调用点——
 * `'a` 被绑定到哪个实参上。这一栏只解释「标注怎么读」，**不**做合法性校验。
 */
#pragma once

#include <string>
#include <vector>

#include "analyzer.hpp"

namespace rustview {

/** @brief 画图选项。 */
struct VisualOptions {
    std::string title = "RustView · Rust 借用语义可视化"; ///< 页面 / 图标题
    std::string sourceName;  ///< 被分析的文件名（显示在标题下面）
};

/**
 * @brief 图表生成器：只读 `AnalysisResult`，不改任何状态。
 *
 * 典型用法：
 * @code
 *   Visualizer viz(result, options);
 *   const std::string svg = viz.renderSvg();
 *   writeFile("borrow.svg", svg);
 *   writeFile("index.html", viz.renderHtml(svg));
 * @endcode
 */
class Visualizer {
public:
    /**
     * @brief 构造生成器。
     * @param result 分析结果（必须比生成器活得久）。
     * @param options 标题 / 文件名等显示选项。
     */
    Visualizer(const AnalysisResult &result, VisualOptions options);

    /**
     * @brief 生成 SVG 全文。
     * @return 一段自包含的 `<svg>…</svg>`，含箭头 marker、红色冲突高亮与悬垂断线符号。
     */
    std::string renderSvg() const;

    /**
     * @brief 把 SVG 包进一张 HTML 页面。
     * @param svg `renderSvg()` 的返回值（内联进页面，页面自包含、离线可看）。
     * @return 完整 HTML 文本：顶部是状态条（成功 / 失败），中间是图，底部是诊断列表。
     */
    std::string renderHtml(const std::string &svg) const;

    /** @brief 一行状态摘要，如 `分析失败 · 2 处冲突`。 */
    std::string statusLine() const;

private:
    /** @brief 一个矩形框（画布坐标）。 */
    struct Box {
        double x = 0, y = 0, w = 0, h = 0;
        double cx() const { return x + w / 2; }
        double cy() const { return y + h / 2; }
    };

    const AnalysisResult &result_;  ///< 分析结果
    VisualOptions options_;         ///< 显示选项
};

} // namespace rustview
