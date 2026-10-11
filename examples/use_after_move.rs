// RustView 示例（使用已 move 的变量）：s1 的所有权已经移给 s2，
// 再读 s1 就是「使用已被移动的变量」。期望：报错、生成诊断图、退出码 1。

fn main() {
    let s1 = "hello";
    let s2 = s1;
    print(s1); // 错误：s1 已经被 move 给 s2，这里已经没有值可读了
}
