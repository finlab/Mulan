// unit_init_exit.mu — `exit()` still runs the unit cleanup (the sequence is registered with atexit)
unit unit_init_exit;

extern int puts(const char* s);
extern void exit(int code);

struct Probe {
    Probe() { puts("probe init"); }
    ~Probe() { puts("probe fini"); }
}

Probe probe;

int main() {
    puts("before exit");
    exit(0);
    return 0;
}
