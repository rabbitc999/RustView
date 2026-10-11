#!/bin/sh
# rust-view/ci.sh —— RustView 的发布门禁：一条命令跑完所有"必须绿"的检查。
#
# 用法（在本目录跑）：
#   sh ci.sh                 全部
#   sh ci.sh --no-preview    跳过 doc/ 效果图重生成（只验构建与样例）
#   sh ci.sh --help
#
# 环境要求：g++ / make 必需；还要一份 **ParseEngine 文法生成器**
# （本项目只有 .gram，生成器是外部依赖）：用 PARSER=<路径> 指定，或让它待在
# 项目根目录 / 旁边的 ParseEngine 仓库 build/bin/；不在就报错退出 2。
# 重生成 doc/ 效果图那步还需要 python3 + cairosvg（缺 cairosvg 时只留 SVG，不算失败）。
#
# 退出码：0 全绿；1 有检查失败；2 环境不满足。

set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 2
cd "$root" || exit 2

run_preview=1
for arg in "$@"; do
    case "$arg" in
    --no-preview) run_preview=0 ;;
    -h | --help)
        awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
        exit 0
        ;;
    *)
        echo "未知参数：$arg（用 --help 看用法）" >&2
        exit 2
        ;;
    esac
done

have() { command -v "$1" >/dev/null 2>&1; }

# make 的名字按平台挑：Windows 的 MinGW / MSYS 常常只装了 `mingw32-make`。
MAKE=''
for candidate in make mingw32-make; do
    if have "$candidate"; then MAKE="$candidate"; break; fi
done

echo "== 环境检测 =="
missing=''
if [ -n "$MAKE" ]; then
    printf '  %-8s %s\n' make "$(command -v "$MAKE")"
else
    printf '  %-8s 缺失（必需）\n' make
    missing="$missing make"
fi
if have g++; then
    printf '  %-8s %s\n' g++ "$(command -v g++)"
else
    printf '  %-8s 缺失（必需）\n' g++
    missing="$missing g++"
fi
# 文法生成器是外部依赖：本项目只有 .gram，生成器由 ParseEngine 仓库产出。
PARSER=${PARSER:-}
if [ -z "$PARSER" ]; then
    for candidate in "./ParseEngine.exe" "./ParseEngine" \
                     "../ParseEngine/build/bin/ParseEngine.exe" "../ParseEngine/build/bin/ParseEngine" \
                     "../build/bin/ParseEngine.exe" "../build/bin/ParseEngine"; do
        if [ -x "$candidate" ]; then PARSER="$candidate"; break; fi
    done
fi
if [ -n "$PARSER" ] && [ -x "$PARSER" ]; then
    printf '  %-8s %s\n' engine "$PARSER"
else
    printf '  %-8s 缺失（必需）：用 PARSER=<路径> 指一份 ParseEngine 生成器\n' engine
    missing="$missing ParseEngine"
fi
if [ -n "$missing" ]; then
    echo
    echo "缺少必需工具：$missing —— 装好再跑。" >&2
    exit 2
fi

step=0
failed=''
run_step() { # 标题 命令...
    title=$1
    shift
    step=$((step + 1))
    printf '\n[%2d] %s ... ' "$step" "$title"
    started=$(date +%s)
    if "$@" >"build/ci/logs/$(printf '%02d' "$step").log" 2>&1; then
        echo "ok（$(($(date +%s) - started))s）"
        return 0
    fi
    echo "失败（$(($(date +%s) - started))s）"
    echo "     完整输出：build/ci/logs/$(printf '%02d' "$step").log"
    failed="$failed
  - $title"
    return 1
}

mkdir -p build/ci/logs

# ---- 1. 生成前端 + 编译 ----
run_step "生成分析器并编译（make）" "$MAKE" all "PARSER=$PARSER"

# ---- 2. 五组样例断言 ----
run_step "样例断言（make check）" "$MAKE" check "PARSER=$PARSER"

# ---- 3. doc/ 效果图重生成（可选；与仓库里已提交的产物对比能发现"图没跟上代码"）----
if [ "$run_preview" = 1 ]; then
    run_step "重生成 doc/ 效果图（make preview）" "$MAKE" preview "PARSER=$PARSER"
fi

# ---- 汇总 ----
echo
echo "== 汇总 =="
if [ -z "$failed" ]; then
    echo "  步骤：$step，全部通过"
    echo "  版本：$(cat VERSION 2>/dev/null || echo '（无 VERSION 文件）')"
    exit 0
fi
fail_count=$(printf '%s\n' "$failed" | grep -c '^  - ')
echo "  步骤：$step，成功 $((step - fail_count))，失败 $fail_count"
echo "  失败：$failed"
exit 1
