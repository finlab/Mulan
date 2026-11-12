unit exception_generic;

extern int printf(const char* fmt, ...);

class Err : Exception {
    Err() { super("g"); }
}

T recover<T>(T value) {
    try {
        if true { throw new Err(); }
    } catch (e: Err) {
        return value;
    }
    return value;
}

int main() {
    printf("%d\n", recover<int>(7));
    return 0;
}
