// math.mu — cross-unit call example
// usage: write `use math;` in another unit, then access it through `math.symbol`.

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
