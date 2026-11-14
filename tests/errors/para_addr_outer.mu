unit para_addr_outer;

int main() {
    int x = 0;
    para {
        { int* p = &x; }
    }
    return 0;
}
