// buffer_ptr.mu — `buffer<T>.ptr`: the data pointer of a managed sequence (C interop)
unit buffer_ptr;

extern int puts(const char* s);

int main() {
    buffer<int> b = new int[3];
    int* p = b.ptr;
    p[0] = 5;
    p[1] = 6;
    p[2] = 7;
    if b[1] != 6 { puts("ptr write failed"); return 1; }
    if p[2] != 7 { puts("ptr read failed"); return 1; }

    buffer<int> empty = {};
    if empty.ptr != null { puts("empty ptr should be null"); return 1; }

    puts("buffer ptr ok");
    return 0;
}
