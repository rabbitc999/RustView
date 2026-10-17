# RustView —— Rust 借用语义可视化工具（生成 → 编译）
#
# 独立仓库：产物全部落在本仓库的 build/rust-view 里，源码目录保持干净。
#
#   make            从文法生成 C++17 前端，并编译出 build/rust-view/rust-view
#   make gen        只重新生成解析器（改了 rust_subset.gram / epilogue 之后跑）
#   make check      跑五组样例：正常 / 借用冲突 / 悬垂 / move 后使用 / 不支持结构
#   make preview    重生成 doc/ 里的效果图与演示页（README 用）
#   make overview   重生成 doc/overview/ 里的项目总览图（需要 Pillow）
#   make clean      清掉本工程的构建产物
#
# 前端由外部的 ParseEngine 生成器产出（本项目只有 .gram），src/ 里不写 lexer / parser。
.DEFAULT_GOAL := all

# 生成器（ParseEngine）是**外部依赖**：本项目只有 .gram，不含生成器。按下面的顺序找，
# 也可用 PARSER=<路径> 指定（与 metro-lang 独立仓库保持同一套候选顺序）：
#   ./ParseEngine[.exe]                          本项目根目录（最省事）
#   ../ParseEngine/build/bin/ParseEngine[.exe]   与 ParseEngine 仓库并排时
#   ../build/bin/ParseEngine[.exe]               还住在 ParseEngine 仓库里时
ifeq ($(origin PARSER),undefined)
PARSER := $(firstword $(wildcard $(CURDIR)/ParseEngine.exe) $(wildcard $(CURDIR)/ParseEngine) \
                        $(wildcard $(CURDIR)/../ParseEngine/build/bin/ParseEngine.exe) $(wildcard $(CURDIR)/../ParseEngine/build/bin/ParseEngine) \
                        $(wildcard $(CURDIR)/../build/bin/ParseEngine.exe) $(wildcard $(CURDIR)/../build/bin/ParseEngine))
endif

# 找不到生成器时给一条清楚的提示（不是让编译过程在后续步骤里报莫名的错）。
ifeq ($(strip $(PARSER)),)
$(error 找不到 ParseEngine 文法生成器（本项目只有 .gram，生成器是外部依赖）。任选一种：1) make -C ../ParseEngine/source -j4 构建出 ../ParseEngine/build/bin/ParseEngine；2) 复制到本项目根目录；3) make PARSER=/path/to/ParseEngine.exe)
endif

# 只有 `make preview` 需要 Python 3（重生成 doc/ 里的效果图）。
# Windows 上 `python3` 常常是 Microsoft Store 的占位程序（跑起来只会提示去应用商店），
# 所以这里两个都试一次，拿到真正能跑的解释器；都拿不到才报错。
# 显式指定：make preview PYTHON=/path/to/python
#
# 空设备的坑：`$(shell ...)` 走的是 make 自己的 shell。即使在 Windows 上，
# 如果 make 跑在 Git Bash / MSYS 里，那个 shell 是 POSIX sh，`2>NUL` 会被当成
# 「重定向到一个名叫 NUL 的文件」——于是每次 make 都在当前目录留下一个空文件
# NUL（`git add -A` 会因它报错）。所以这里像 hydra-86 一样在**解析期**探测有没有
# sh：有就用 /dev/null，只有真的在 cmd 下才用 NUL。
ifeq ($(OS),Windows_NT)
ifeq ($(shell command -v sh 2>/dev/null),)
PYTHON_NULL := NUL
else
PYTHON_NULL := /dev/null
endif
else
PYTHON_NULL := /dev/null
endif
PYTHON ?= $(or $(shell python3 -c "import sys; print('python3' if sys.version_info.major == 3 else '')" 2>$(PYTHON_NULL)), \
              $(shell python -c "import sys; print('python' if sys.version_info.major == 3 else '')" 2>$(PYTHON_NULL)))
PYTHON := $(PYTHON)
ifeq ($(strip $(PYTHON)),)
$(error 找不到可用的 Python 3（make preview 需要它来渲染效果图），请安装 python3 或 python，或设置 PYTHON)
endif

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-switch -MMD -MP

# 产物全部落在本仓库的 build/ 里。注意顺序：INCS 用 `:=` 立即展开，
# 所以 BUILD/GEN 必须定义在 INCS 之前。
BUILD   := $(CURDIR)/build/rust-view
GEN     := $(BUILD)/gen
# Windows 上可执行文件带 .exe 后缀：sh 执行 `.../rust-view` 时会自动补 .exe，
# 但 `make preview` 把路径交给 Python 的 subprocess，Python **不会**补，
# 所以这里直接把后缀拼进 BIN，一处修好、四条目标（all/check/preview）都不会再踩。
ifeq ($(OS),Windows_NT)
EXE_SUFFIX := .exe
else
EXE_SUFFIX :=
endif
BIN     := $(BUILD)/rust-view$(EXE_SUFFIX)
PREVIEW_SCALE ?= 3

INCS     := -I$(CURDIR) -I$(CURDIR)/include -I$(GEN)

OBJS := \
	$(BUILD)/obj/src/main.o \
	$(BUILD)/obj/src/analyzer.o \
	$(BUILD)/obj/src/visualizer.o \
	$(BUILD)/obj/gen/parser.o

DEPS := $(OBJS:.o=.d)
-include $(DEPS)

# 显式钉死默认目标：`-include` 进来的 .d 文件里带着 `obj/xx.o: ...` 规则，
# 若不钉住，make 会把「第一个遇到的规则」当成默认目标（只会去编某个 .o，
# 却不链接可执行文件）。加这一行后 `make` 一定走 `all`。
.PHONY: all gen check preview overview clean

all: $(BIN)

# 生成器读文法时会展开 %code / %epilogue 两个 include，所以它们也是依赖：
# 只改了 epilogue 里的建树动作，也必须重生成 parser.cpp 才会生效。
# 下面这条自检的提示语只能写 ASCII：Windows 上的 mingw32-make 构造命令行时会把
# 参数按 ANSI(CP936) 转一遍，中文消息里奇数个字节会把紧邻的转义引号吃掉，整条
# 命令被拆成几段，报 `unexpected EOF while looking for matching '"'`。
# 中文含义：ParseEngine 生成失败，请看上面的错误（旧 parser.cpp 已删除，绝不静默用旧产物）。
$(GEN)/parser.cpp: $(CURDIR)/rust_subset.gram $(CURDIR)/epilogue/rust_parse.cpp \
		$(CURDIR)/include/rust_syntax.hpp $(PARSER)
	@mkdir -p $(GEN)
	@rm -f $(GEN)/parser.cpp
	$(PARSER) $(CURDIR)/rust_subset.gram --lang cpp17 --mode ielr --out-dir $(GEN)
	@test -s $(GEN)/parser.cpp || { echo "ParseEngine generation failed - see errors above (old parser.cpp removed)"; exit 1; }

gen: $(GEN)/parser.cpp

$(BUILD)/obj/src/%.o: $(CURDIR)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCS) -c $< -o $@

$(BUILD)/obj/gen/%.o: $(GEN)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCS) -c $< -o $@

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

# 六组验收样例：正常样例能出图（退出 0）；冲突 / 悬垂 / move 后使用样例必须给出
# 非 0 退出码；不支持的结构样例必须报错且**一个文件都不写**；生命周期样例必须
# 出图且退出 0，并且图里真的画出了生命周期栏（断言第四栏标题出现在 SVG 里）。
# （下面逐条断言的注释见每个 echo。）
# 冲突与悬垂**仍会生成诊断图**，所以这里断言的是「退出码」，不是「有没有文件」。
#
# 注意：本目标的 echo 提示语一律写 ASCII。Windows 上的 mingw32-make 构造命令行时会把
# 参数按 ANSI(CP936) 转一遍，中文消息里奇数个字节会把紧邻的转义引号吃掉，整条命令被
# 拆成几段，报 `unexpected EOF while looking for matching '"'`。中文注释不受影响。
# 各条断言的中文含义：
#   1/6 正常样例：examples/borrow_ok.rs  —— 必须出图、退出 0
#   2/6 借用冲突：examples/borrow_conflict.rs —— 必须非 0 退出（诊断图已生成）
#   3/6 悬垂指针：examples/dangling.rs —— 必须非 0 退出（诊断图已生成）
#   4/6 使用已 move 的变量：examples/use_after_move.rs —— 必须非 0 退出（诊断图已生成）
#   5/6 不支持的结构：examples/unsupported_field.rs —— 必须非 0 退出，且一个文件都不写
#   6/6 生命周期标注：examples/lifetime.rs —— 必须出图、退出 0，且画出生命周期栏
check: $(BIN)
	@mkdir -p $(BUILD)/out
	@echo "== 1/6 borrow_ok.rs (expect: render ok, exit 0) =="
	$(BIN) $(CURDIR)/examples/borrow_ok.rs -o $(BUILD)/out
	@echo "== 2/6 borrow_conflict.rs (expect: non-zero exit) =="
	@if $(BIN) $(CURDIR)/examples/borrow_conflict.rs -o $(BUILD)/out; then \
		echo "!! expected borrow-conflict error, but exit code was 0"; exit 1; \
	else echo "errored as expected (diagnostic figure written)"; fi
	@echo "== 3/6 dangling.rs (expect: non-zero exit) =="
	@if $(BIN) $(CURDIR)/examples/dangling.rs -o $(BUILD)/out; then \
		echo "!! expected dangling-reference error, but exit code was 0"; exit 1; \
	else echo "errored as expected (diagnostic figure written)"; fi
	@echo "== 4/6 use_after_move.rs (expect: non-zero exit) =="
	@if $(BIN) $(CURDIR)/examples/use_after_move.rs -o $(BUILD)/out; then \
		echo "!! expected use-after-move error, but exit code was 0"; exit 1; \
	else echo "errored as expected (diagnostic figure written)"; fi
	@echo "== 5/6 unsupported_field.rs (expect: non-zero exit, write nothing) =="
	@if $(BIN) $(CURDIR)/examples/unsupported_field.rs -o $(BUILD)/unsupported_out; then \
		echo "!! expected unsupported-structure error, but exit code was 0"; exit 1; \
	fi
	@test ! -e $(BUILD)/unsupported_out/unsupported_field.svg \
		|| { echo "!! unsupported structure still wrote a figure"; exit 1; }
	@echo "render aborted as expected, no figure written"
	@echo "== 6/6 lifetime.rs (expect: render ok, exit 0, lifetime panel drawn) =="
	$(BIN) $(CURDIR)/examples/lifetime.rs -o $(BUILD)/out
	@grep -q "lifetime-panel" $(BUILD)/out/lifetime.svg \
		|| { echo "!! lifetime panel missing in generated figure"; exit 1; }
	@echo "lifetime panel rendered as expected"
	@echo "check passed, outputs in $(BUILD)/out"

# 重新生成 doc/ 里的效果图与演示页（README 用）。
# PREVIEW_SCALE 控制 PNG 倍率，默认 3（1320 画布 -> 3960px 位图）；
# 想换清晰度：make preview PREVIEW_SCALE=4
# 样例里冲突 / 悬垂 / move 后使用本来就是「预期失败」，脚本只保留它们生成的诊断图。
# echo 提示语同样写 ASCII（理由见 check 目标上方注释）。
preview: $(BIN)
	@mkdir -p $(CURDIR)/doc
	$(PYTHON) $(CURDIR)/tools/make_preview.py --bin $(BIN) \
		--examples $(CURDIR)/examples --out $(CURDIR)/doc --scale $(PREVIEW_SCALE)
	@echo "previews updated: doc/preview*.svg / .png / .html"

# 重生成 doc/overview/ 里的项目总览图（PNG + SVG）。只依赖 Python 3 + Pillow，
# 不依赖本工程的编译产物；改了 tools/make_overview.py 的文案就重跑它。
overview:
	$(PYTHON) $(CURDIR)/tools/make_overview.py
	@echo "overview updated: doc/overview/RustView-overview.png / .svg"

clean:
	@rm -rf $(BUILD)
	@echo cleaned $(BUILD)
