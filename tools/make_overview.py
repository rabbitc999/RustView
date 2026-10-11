#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 RustView 总览图（PNG + SVG），与仓库现有总览图保持同一风格。

运行：python tools/make_overview.py

四栏：① 文法与生成前端 ② 借用分析 ③ 可视化 ④ 样例与验收。
图和 README.md 说的是同一件事：一份极简 Rust 伪代码，经外部 ParseEngine 生成的前端
变成 AST，再由作用域栈 + 借用状态表分析，最后手写拼出 SVG / HTML。文案改动时记得
同时改 README.md。
"""

from __future__ import annotations

from pathlib import Path

from overview_canvas import (
    CARD, CARD_EDGE, FONT_SANS, FONT_SANS_BOLD, NOTE, PANEL, PANEL_EDGE,
    PTITLE, SUBTITLE, TITLE, Canvas, font,
)

COL_X = (40, 410, 780, 1150)
COL_W = 360

# 每栏三个分组；分组内的条目按「卡片」逐条排下去。这里只写内容，
# 具体 y 坐标由 draw() 按统一节奏推出来（见下面的表头注释）。
COLUMNS = (
    (
        "① 文法与前端", "rust_subset.gram · include/ · epilogue/",
        (
            ("词法层", (
                ("关键字 Token", "fn / let / mut / print / struct / int / String"),
                ("字面量与标识符", "字符串 · 数字走 %token regex；标识符走 %identifier"),
                ("注释与空白", "// 与 /* */ 走 %trivia；空白默认跳过"),
            )),
            ("文法结构", (
                ("显式产生式", "20 个 Token · 43 条产生式"),
                ("块与尾表达式", "{ } 可嵌套；{ ...; expr } 以尾表达式为块的值"),
                ("类型", "int / String / &T / &mut T 各占一条产生式"),
            )),
            ("生成器", (
                ("cpp17 + IELR(1)", "由外部 ParseEngine 生成：76 状态 · 0 未消解冲突"),
                ("建树内嵌", "%code 声明 rv_* 动作；%epilogue 内嵌节点池与 runText 驱动"),
                ("语义值即下标", "节点池下标（POD），躲开所有权搬运"),
            )),
        ),
    ),
    (
        "② 借用分析", "src/analyzer.cpp · include/analyzer.hpp",
        (
            ("作用域栈", (
                ("压栈 / 弹栈", "{ 压栈、} 弹栈；内层同名变量自然遮蔽外层"),
                ("出栈清理", "归还借用计数 —— 块结束，借出去的那份就释放"),
                ("悬垂检测", "外层还活着的引用指向刚销毁的变量 → 悬垂"),
            )),
            ("借用状态表", (
                ("五个状态", "Uninitialized / Initialized / Moved / Borrowed*"),
                ("状态推导", "由借用计数在 refreshState() 一处推导"),
                ("Move 是终态", "搬走不再复活；再读 / 再借即报错"),
            )),
            ("冲突与边界", (
                ("&mut 独占", "有 &mut 时不能再有任何借用"),
                ("读不并存写", "已有 &mut 时不能再取 &；多个 & 可以共存"),
                ("硬边界", "字段级访问 s.a → 不支持，退出码 2、不写文件"),
            )),
        ),
    ),
    (
        "③ 可视化", "src/visualizer.cpp · doc/",
        (
            ("三栏版面", (
                ("栈 · 作用域", "作用域矩形套矩形；出块变量置灰「已销毁」"),
                ("函数调用栈", "签名节点：实参借用与返回值绑定关系"),
                ("堆对象", "String / 结构体；细线连到持有它的变量"),
            )),
            ("连线语义", (
                ("Move", "灰实线箭头；原变量 fill=#e6e6e6"),
                ("借用", "虚线彩色箭头：& 蓝、&mut 橙；已释放转浅灰"),
                ("冲突 / 悬垂", "冲突借用边标红；悬垂画断裂闪电 +「已销毁」"),
            )),
            ("输出", (
                ("SVG", "纯标准库字符串拼接，零第三方依赖"),
                ("HTML", "自包含 index.html：内联 CSS + SVG，离线可看"),
                ("快照点", "main 返回之前；只有内层块退出才标「已销毁」"),
            )),
        ),
    ),
    (
        "④ 样例与验收", "examples/ · Makefile · tools/",
        (
            ("示例（5 组）", (
                ("borrow_ok.rs", "值语义 / Move / 借用 / 块作用域，分析通过"),
                ("borrow_conflict.rs", "& 还活着时又取 &mut → 借用冲突"),
                ("dangling.rs", "返回局部变量引用 → 悬垂指针"),
                ("use_after_move.rs", "读一个已被 move 的变量 → 报错"),
                ("unsupported_field.rs", "字段级访问，硬失败且不写任何文件"),
            )),
            ("构建与验收", (
                ("make", "生成前端并编译；make check 跑 5 组断言"),
                ("make preview", "重生成 doc/ 效果图与演示页（PNG 默认 3 倍）"),
                ("退出码 / 报告", "0 通过 · 1 冲突或悬垂 · 2 不支持；RESULT.md 76 状态 0 冲突"),
            )),
        ),
    ),
)


def draw() -> Canvas:
    """按四栏结构画出 RustView 总览图。"""
    heading = font(FONT_SANS_BOLD, 30)
    subtitle = font(FONT_SANS, 15)
    panel_title = font(FONT_SANS_BOLD, 17)
    section_title = font(FONT_SANS_BOLD, 13)
    item_title = font(FONT_SANS_BOLD, 12)
    item_detail = font(FONT_SANS, 11)
    footer = font(FONT_SANS, 12)

    canvas = Canvas()
    canvas.text(40, 58, "RustView · Rust 借用语义可视化总览", heading, TITLE)
    canvas.text(40, 88,
                "独立仓库 · 文法 → 生成前端 → 借用分析 → 手写 SVG / HTML 四层 · "
                "1.0 功能冻结",
                subtitle, SUBTITLE)

    # 统一节奏：分组标题 + 组内卡片（每张 52 高，卡间留 6、组间留白），四栏共用同一套 y。
    # 3 组 × 3 张时内容底边约在 y=780，面板底边 818，留 ~38 px 余量。
    card_pitch, header_lead, group_tail = 58, 14, 10
    for x, (title, location, groups) in zip(COL_X, COLUMNS):
        canvas.rect(x, 118, COL_W, 700, PANEL, PANEL_EDGE, radius=14, width=1.5)
        canvas.text(x + 18, 148, title, panel_title, PTITLE)
        canvas.text(x + 18, 170, location, item_detail, NOTE)
        cursor = 202
        for group_title, entries in groups:
            canvas.text(x + 18, cursor, group_title, section_title, TITLE)
            y = cursor + header_lead
            for name, detail in entries:
                canvas.rect(x + 18, y, COL_W - 36, 52, CARD, CARD_EDGE, radius=8)
                canvas.text(x + 29, y + 21, name, item_title, PTITLE)
                canvas.text(x + 29, y + 41, detail, item_detail, NOTE)
                y += card_pitch
            cursor = y + group_tail

    canvas.text(40, 859,
                "仓库关系：RustView 是独立仓库（https://gitee.com/rabbitc999/rust-view），"
                "前端由外部 ParseEngine 从 rust_subset.gram 生成（cpp17 + IELR(1)），",
                footer, NOTE)
    canvas.text(40, 885,
                "版本 1.0，功能冻结：除明确 BUG 外不再改动。图表是零依赖的标准库字符串拼 SVG；"
                "效果图与演示页见 doc/，语言边界与版本口径见 README.md 与 VERSION。",
                footer, NOTE)
    return canvas


def main() -> None:
    image_dir = Path(__file__).resolve().parents[1] / "doc" / "overview"
    image_dir.mkdir(parents=True, exist_ok=True)
    canvas = draw()
    canvas.save(str(image_dir / "RustView-overview.png"),
                str(image_dir / "RustView-overview.svg"))
    print("doc/overview/RustView-overview.png + .svg")


if __name__ == "__main__":
    main()
