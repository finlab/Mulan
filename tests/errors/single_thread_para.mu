unit single_thread_para;

class Box {
    public int a;
    public int b;
}

int work(int n) {
    return n * 2;
}

// `--single-thread` emits reference counting without atomics, so concurrency must be rejected
// rather than silently compiled into a data race.
int main() {
    Box out = new Box();
    int n = 10;
    para {
        { out.a = work(n); }
        { out.b = work(n); }
    }
    return out.a + out.b;
}
