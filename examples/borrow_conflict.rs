// RustView 示例（借用冲突）：不可变借用 r1 还活着时，又对 x 发起可变借用。
// 期望：控制台报错，附带红框诊断图，退出码 1。

fn main() {
    let mut x = "world";
    let r1 = &x;
    let r3 = &mut x; // 冲突：x 正在被 &x 借用，不能再 &mut 借用
}
