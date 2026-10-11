// RustView 示例（正常路径）—— 即需求里的「示例输入」全文。
// 两处会报错的语句（&mut 借用冲突、悬垂指针调用）都按原样注释掉，
// 这份文件应当**分析成功**并生成 SVG + HTML。

fn get_number() -> int {
    42
}

fn get_string() -> String {
    "hello"
}

fn borrow_and_return(s: &String) -> &String {
    s
}

// 悬垂指针示例：返回局部变量的引用。
// 函数体本身是允许被解析的；只有**调用它**才判定为悬垂（见 examples/dangling.rs）。
fn dangling() -> &String {
    let local = "hello";
    &local
}

fn main() {
    let a = 42;
    let b = a; // 值语义，独立节点

    let s1 = "hello";
    let s2 = s1; // 引用语义，转移所有权，s1 标灰

    {
        let mut x = "world";
        let r1 = &x;
        let r2 = &x;
        // 合理情况：对「引用」再取一次共享引用（简化模型里记作 &&String）。
        // 共享借用可以共存，所以 r1 / r2 / r3 同时活着完全合法。
        let r3 = &r1;
        // let r4 = &mut x; // 如果取消注释，必须报错冲突并停止生成！
    }

    // 函数调用示例
    let n = get_number();          // 值语义，返回数值
    let s = get_string();          // 引用语义，返回字符串，所有权移交给 s
    let r = borrow_and_return(&s); // 传入引用，返回引用，r 的生命周期与 s 的借用绑定

    // print 只当「用这个值一次」：用来暴露「打印一个已被 move 的变量」这类错误，
    // 它自己不会把所有权搬走。这里的 `&s2` 是临时借用，用完即还。
    print(n);
    print(&s2);

    // 悬垂指针调用示例（必须报错）
    // let d = dangling();         // 检测到悬垂指针，必须报错并终止！
}
