// RustView 示例（悬垂指针）：把样例里的 `let d = dangling();` 取消注释。
// 期望：控制台报错，诊断图里 local 标红「已销毁」、返回引用画断裂闪电，退出码 1。

fn dangling() -> &String {
    let local = "hello";
    &local // 返回指向局部变量 local 的引用：local 在函数返回时被销毁
}

fn main() {
    let d = dangling(); // 悬垂：d 绑定到一个指向已销毁局部变量的引用
}
