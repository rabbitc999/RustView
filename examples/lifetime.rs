// RustView 示例（生命周期标注）：读法演示 —— `fn f<'a>(s: &'a String) -> &'a String`。
// 期望：分析通过、退出码 0；图上第四栏列出 first() 声明了 'a，并说明调用时 'a 绑定在哪个实参上。
//
// 记住一句话就够了：`'a` 不是「活得久」的意思，它是「这里用的和那里用的是同一个东西」——
// 返回的引用和形参 s 共享同一个生命周期参数，所以调用方给 s 传多长的借用，返回值就活多长。
//
// 本工具只解释标注，不校验它是否成立（那是借用检查器的事）。

fn first<'a>(s: &'a String) -> &'a String {
    s
}

// 两个参数各带一个生命周期，再加一条约束 `'b: 'a`（'b 不短于 'a）。
// 返回 `&'a String`：结果只能来自 x，不可能来自 y —— 约束保证了这一点。
fn longer<'a, 'b: 'a>(x: &'a String, y: &'b String) -> &'a String {
    x
}

fn main() {
    let s = "hello";

    // 这里 'a 就是 s 的借用：r 只能活到 s 不再被借用为止。
    let r = first(&s);

    // 两个实参用的是同一个 s，'a 与 'b 都落在 s 上。
    let m = longer(&s, &s);

    print(r);
    print(m);
    print(&s);
}
