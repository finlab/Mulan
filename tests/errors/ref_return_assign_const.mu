unit rr4;

struct Counter {
    int n;

    const ref int peek() { return n; }
}

int main() {
    Counter c = {1};
    c.peek() = 5;
    return 0;
}
