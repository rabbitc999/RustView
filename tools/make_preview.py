#!/usr/bin/env python3
"""把 README 要用的效果图 / 演示页批量生成到 rust-view/doc/。

用法：
    python3 tools/make_preview.py --bin <rust-view 可执行文件> \\
        --examples rust-view/examples --out rust-view/doc

对每个样例：
  1. 跑一遍 rust-view（冲突 / 悬垂 / 不支持样例**本来就是非 0 退出**，属预期，不算失败）；
  2. 把生成的 `.svg` / `.html` 拷进 doc/（HTML 是自包含的单文件演示页）；
  3. 装了 cairosvg 就把 SVG 转成同名 `.png`（GitHub README 显示 PNG 最稳），
     没装就跳过、只留 SVG —— 脚本不会因此失败。
"""

import argparse
import re
import shutil
import tempfile
import subprocess
import sys
from pathlib import Path

# (样例文件, doc/ 里的输出基名, 人类可读说明)
CASES = [
    ("borrow_ok.rs", "preview", "正常样例：值语义 / Move / 借用 / 块作用域 / 函数调用"),
    ("borrow_conflict.rs", "preview-conflict", "借用冲突：不可变借用还活着时又取 &mut"),
    ("dangling.rs", "preview-dangling", "悬垂指针：返回局部变量的引用"),
    ("use_after_move.rs", "preview-use-after-move", "使用已被 move 的变量"),
    ("lifetime.rs", "preview-lifetime", "生命周期标注：<'a> 的读法与绑定关系（只画不校验）"),
]


def convert_png(svg_path: Path, png_path: Path, scale: float) -> bool:
    """把 SVG 转成 PNG；任何原因失败都只返回 False（不致命）。

    优先用 cairosvg（LibreOffice 系的纯 Python 依赖）；本机没装 libcairo 时
    退回 Edge / Chrome 的无头截图（Windows 上基本都有 Edge）：
      1. 把 SVG 包进一个零边距的临时 HTML；
      2. `--headless --screenshot=... --window-size=W*3,H*3` 一次截图；
      3. 截完即删临时 HTML。
    两条路都失败才返回 False，脚本不会因此中断。

    @param scale 输出倍率：SVG 的固有宽度会乘以它。默认 3 倍，README 里
                 缩放查看 / 高 DPI 屏幕上文字才不会糊。
    """
    try:
        import cairosvg  # type: ignore
    except Exception:
        cairosvg = None
    if cairosvg is not None:
        try:
            cairosvg.svg2png(url=str(svg_path), write_to=str(png_path), scale=scale)
            return True
        except Exception as exc:  # pragma: no cover - 取决于本机环境
            print(f"   （cairosvg 转换失败，改用浏览器截图：{exc}）")

    svg_text = svg_path.read_text(encoding="utf-8")
    m = re.search(r'width="(\d+)" height="(\d+)"', svg_text)
    if not m:
        return False
    width, height = int(m.group(1)), int(m.group(2))
    wrapper = svg_path.with_suffix(".png-wrap.html")
    wrapper.write_text(
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<style>*{margin:0;padding:0}body{overflow:hidden}</style></head><body>"
        + svg_text.replace("<?xml version=\"1.0\" encoding=\"UTF-8\"?>", "")
        + "</body></html>",
        encoding="utf-8")
    try:
        import shutil
        for name in ("msedge", "chrome"):
            exe = shutil.which(name)
            if exe:
                break
        else:
            # shutil.which 找不到 PATH 里的浏览器时，按 Windows 默认安装位置兜底。
            from pathlib import Path as P
            for candidate in (
                r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                r"C:\Program Files\Google\Chrome\Application\chrome.exe",
            ):
                if P(candidate).exists():
                    exe = candidate
                    break
            else:
                return False
        subprocess.run(
            [exe, "--headless", "--disable-gpu", "--hide-scrollbars",
             f"--force-device-scale-factor={scale}",
             f"--screenshot={png_path.resolve()}",
             f"--window-size={width},{height}",
             wrapper.resolve().as_uri()],
            capture_output=True, timeout=120, check=False)
        return png_path.exists() and png_path.stat().st_size > 0
    except Exception:
        return False
    finally:
        wrapper.unlink(missing_ok=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="rust-view 可执行文件")
    ap.add_argument("--examples", required=True, help="样例目录")
    ap.add_argument("--out", required=True, help="doc 输出目录")
    ap.add_argument("--scale", type=float, default=3.0,
                    help="PNG 输出倍率（默认 3：1320px 的画布出 3960px 的位图，更清晰）")
    args = ap.parse_args()

    binary = Path(args.bin).resolve()
    examples = Path(args.examples).resolve()
    out = Path(args.out).resolve()
    if not binary.exists():
        print(f"找不到可执行文件：{binary}", file=sys.stderr)
        return 1
    out.mkdir(parents=True, exist_ok=True)
    # 中间产物写进临时目录：跑完就删，doc/ 里只留最终资源。
    work = Path(tempfile.mkdtemp(prefix="rust-view-preview-"))

    missing_png = []
    for sample, base, note in CASES:
        src = examples / sample
        if not src.exists():
            print(f"跳过（样例不存在）：{src}", file=sys.stderr)
            continue
        # 非 0 退出是这些「错误样例」的预期结果，所以不检查 returncode。
        subprocess.run([str(binary), str(src), "-o", str(work), "--name", base],
                       check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        svg = work / f"{base}.svg"
        html = work / "index.html"
        if not svg.exists():
            print(f"生成失败（没有 SVG）：{sample}", file=sys.stderr)
            return 1
        shutil.copyfile(svg, out / f"{base}.svg")
        if html.exists():
            shutil.copyfile(html, out / f"{base}.html")
        png = out / f"{base}.png"
        if not convert_png(svg, png, args.scale):
            missing_png.append(base)
        print(f"  {sample:22s} -> doc/{base}.svg / .html / .png({args.scale:g}x)   {note}")

    shutil.rmtree(work, ignore_errors=True)
    if missing_png:
        print("提示：以下 PNG 未生成（缺 cairosvg，README 仍可用 SVG）："
              + ", ".join(missing_png))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
