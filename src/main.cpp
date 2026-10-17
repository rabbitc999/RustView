/**
 * @file main.cpp
 * @brief RustView 的命令行入口：源码 → 语法树 → 借用分析 → SVG / HTML。
 *
 * 整个工具是一条**单向流水线**，任何一步失败都不会污染后面的产物：
 *
 *   ┌──────────┐  失败：打印行列号，退出 1，不写任何文件
 *   │  parseText│────────────────────────────────────────────┐
 *   └────┬─────┘                                             │
 *        │ 成功（拿到 Program 根节点）                          │
 *   ┌────▼─────┐  失败（不支持的结构）：打印原因，退出 2，不写任何文件
 *   │ Analyzer │────────────────────────────────────────────┤
 *   └────┬─────┘                                             │
 *        │ 成功（拿到 AnalysisResult）                          │
 *   ┌────▼──────┐  有借用冲突 / 悬垂：仍然画**诊断图**，退出 1
 *   │ Visualizer│────────────────────────────────────────────┤
 *   └────┬──────┘                                             │
 *        └───────────────────────────────────────────────────┘
 *
 * 这里刻意把「不支持的结构」（语法上合法、但本工具分析不了，如字段级访问 `s.a`）
 * 和「借用语义冲突」（能分析、但用户代码有错，如 `&mut` 与 `&` 同时借）分开：
 *   - 前者必须**硬失败**，连图都不画——画出来只会误导人；
 *   - 后者要画出一张把冲突标红的**教学图**，让人看清楚错在哪里，但退出码非 0，
 *     方便脚本 / CI 判断「这次运行其实是失败的」。
 *
 * 用法：
 * @code
 *   rust-view <源码.rs> [--out <输出目录>] [--name <文件基名>]
 * @endcode
 */

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "analyzer.hpp"
#include "rust_syntax.hpp"
#include "visualizer.hpp"

namespace {

/** @brief 一条命令行选项解析结果。 */
struct Options {
    std::string input;   ///< 必填：要分析的源码文件
    std::string outDir = ".";   ///< `-o/--out`：SVG / HTML 的输出目录（默认当前目录）
    std::string baseName;       ///< 输出文件基名；默认取输入文件名去掉扩展名
    bool showHelp = false;      ///< `-h/--help`
};

/** @brief 打印用法。 */
void printUsage(const char *argv0) {
    std::cout
        << "rust-view —— Rust 借用语义可视化工具（教学用）\n\n"
        << "用法：\n"
        << "  " << argv0 << " <源码.rs> [-o <输出目录>] [--name <基名>]\n\n"
        << "说明：\n"
        << "  解析一份极简 Rust 伪代码，生成 <基名>.svg 与 index.html。\n"
        << "  支持函数泛型参数表里的生命周期标注（<'a> / &'a T），但只画不校验。\n"
        << "  语法错误：打印行列号并退出（不生成文件）。\n"
        << "  生命周期标注写错（'a 没声明 / 重复声明）：报错并退出（不生成文件）。\n"
        << "  不支持的结构（如字段级访问 s.a）：报错并退出（不生成文件）。\n"
        << "  借用冲突 / 悬垂指针：生成诊断图（冲突标红），退出码仍为 1。\n";
}

/**
 * @brief 解析命令行。
 * @param argc / argv 原始参数。
 * @param opts [out] 解析结果。
 * @return 参数合法返回 `true`；否则打印原因并返回 `false`。
 *
 * 参数很少，手写循环即可——这里不需要也不应该引第三方参数库。
 */
bool parseArgs(int argc, char **argv, Options &opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            opts.showHelp = true;
            return true;
        } else if (arg == "-o" || arg == "--out") {
            if (i + 1 >= argc) {
                std::cerr << "参数错误： " << arg << " 需要一个输出目录\n";
                return false;
            }
            opts.outDir = argv[++i];
        } else if (arg == "--name") {
            if (i + 1 >= argc) {
                std::cerr << "参数错误： --name 需要一个文件基名\n";
                return false;
            }
            opts.baseName = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "参数错误：未知选项 " << arg << "\n";
            return false;
        } else if (opts.input.empty()) {
            opts.input = arg;
        } else {
            std::cerr << "参数错误：只能指定一个输入文件（多余的是 " << arg << "）\n";
            return false;
        }
    }
    if (!opts.showHelp && opts.input.empty()) {
        std::cerr << "参数错误：缺少输入文件\n";
        return false;
    }
    return true;
}

/** @brief 读整个文件；失败打印原因并返回 `false`。 */
bool readFile(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "无法打开输入文件：" << path << "\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

/** @brief 写整个文件；失败打印原因并返回 `false`。 */
bool writeFile(const std::string &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "无法写出文件：" << path << "\n";
        return false;
    }
    out << content;
    return static_cast<bool>(out);
}

/** @brief 把路径里的目录部分 + '/' 拼到基名前面，拼出完整输出路径。 */
std::string joinPath(const std::string &dir, const std::string &name) {
    if (dir.empty() || dir == ".") return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

/** @brief 取 `path` 的文件名（去掉目录）、再去掉最后一个扩展名，作为默认输出基名。 */
std::string defaultBaseName(const std::string &path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string file = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::size_t dot = file.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return file.empty() ? "rust-view" : file;
    return file.substr(0, dot);
}

} // namespace

/**
 * @brief 程序入口。
 *
 * 退出码约定（方便脚本判断）：
 *   - `0`：分析成功、无任何诊断，已写出 SVG / HTML；
 *   - `1`：语法错误，或存在借用冲突 / 悬垂指针（冲突时仍会写诊断图）；
 *   - `2`：出现了本工具不支持的结构（**不写任何文件**）。
 *
 * @param argc / argv 命令行参数。
 * @return 见上面的退出码约定。
 */
int main(int argc, char **argv) {
    Options opts;
    if (!parseArgs(argc, argv, opts)) {
        printUsage(argv[0]);
        return 2;
    }
    if (opts.showHelp) {
        printUsage(argv[0]);
        return 0;
    }
    if (opts.baseName.empty()) opts.baseName = defaultBaseName(opts.input);

    std::string source;
    if (!readFile(opts.input, source)) return 1;

    // ---------- 第 1 步：语法分析（绝不手写 lexer / parser，全部交给生成器） ----------
    rustview::NodePtr program;
    std::vector<std::string> parseErrors;
    if (!rustview::parseText(source, program, parseErrors)) {
        std::cerr << "语法分析失败（" << opts.input << "）：\n";
        for (const std::string &line : parseErrors) std::cerr << "  " << line << "\n";
        std::cerr << "已中止，未生成任何图表。\n";
        return 1;
    }

    // ---------- 第 2 步：借用分析（作用域栈 + 借用状态表） ----------
    rustview::Analyzer analyzer(*program);
    const rustview::AnalysisResult result = analyzer.run();

    // 诊断先全部打到 stderr：不管后面画不画图，用户都应该看到完整原因。
    if (!result.issues.empty()) {
        std::cerr << "借用分析发现 " << result.issues.size() << " 处问题：\n";
        for (const rustview::Issue &issue : result.issues) {
            const char *kind = issue.unsupported ? "[不支持] "
                               : issue.lifetime  ? "[生命周期] "
                               : issue.dangling  ? "[悬垂] "
                                                 : "[冲突] ";
            std::cerr << "  · 第 " << issue.line << " 行：" << kind << issue.message << "\n";
        }
    }

    // 不支持的结构 => 硬失败：不写任何文件，避免画出带错误状态的图。
    if (result.hasUnsupported()) {
        std::cerr << "遇到本工具不支持的结构，已中止渲染（未生成任何文件）。\n";
        return 2;
    }

    // 生命周期标注本身写错（`'a` 没声明、重复声明）=> 没有可画的东西：
    // 这类错在**签名处**，图上并没有对应的变量框可以标红，画出来只会是一张
    // 「到处都正常」的图。所以不写文件，退出码和语法错误一样是 1。
    bool lifetimeError = false;
    for (const rustview::Issue &issue : result.issues) {
        if (issue.lifetime) lifetimeError = true;
    }
    if (lifetimeError) {
        std::cerr << "生命周期标注有误（见上），已中止渲染（未生成任何文件）。\n";
        return 1;
    }

    // ---------- 第 3 步：生成图表（冲突 / 悬垂仍画诊断图，但退出码为 1） ----------
    // 到这里才建输出目录：上面任何一步失败都**不会**留下空目录 / 半成品文件。
    if (opts.outDir != "." && !opts.outDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opts.outDir, ec);
        if (ec) {
            std::cerr << "无法创建输出目录：" << opts.outDir << "（" << ec.message() << "）\n";
            return 1;
        }
    }

    rustview::VisualOptions options;
    options.sourceName = opts.input;
    rustview::Visualizer visualizer(result, options);

    const std::string svg = visualizer.renderSvg();
    const std::string html = visualizer.renderHtml(svg);

    const std::string svgPath = joinPath(opts.outDir, opts.baseName + ".svg");
    const std::string htmlPath = joinPath(opts.outDir, "index.html");
    // 除了固定的 index.html，再按基名留一份 <基名>.html，方便一个目录里放多份对照。
    const std::string aliasPath = joinPath(opts.outDir, opts.baseName + ".html");

    if (!writeFile(svgPath, svg)) return 1;
    if (!writeFile(htmlPath, html)) return 1;
    if (aliasPath != htmlPath && !writeFile(aliasPath, html)) return 1;

    std::cout << visualizer.statusLine() << "\n"
              << "  SVG : " << svgPath << "\n"
              << "  HTML: " << htmlPath << "\n";

    // 有冲突 / 悬垂：图已经画好了（教学用），但这次运行算失败。
    return result.ok() ? 0 : 1;
}
