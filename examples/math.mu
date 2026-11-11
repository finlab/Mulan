// math.mu — 跨 unit 调用示例
// 使用方式：在另一个 unit 中写 `use math;`，然后通过 `math.symbol` 访问。

unit math;

public int square(int x) {
    return x * x;
}

public int abs(int x) {
    if x < 0 {
        return -x;
    }
    return x;
}
