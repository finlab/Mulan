unit export_surface_public;

extern int puts(const char* s);

public int pub_fn(int x) { return x + 1; }

export int exp_fn(int x) { return x + 2; }

int main() {
    puts("hi");
    return 0;
}
