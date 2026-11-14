// `export` publishes a symbol for C consumers, so the same signature rules as `extern` apply: a
// dynamic array carries an RC header that C cannot manage.
unit export_slice;

export int total(int[] xs) {
    int sum = 0;
    for x in xs { sum = sum + x; }
    return sum;
}

int main() {
    return 0;
}
