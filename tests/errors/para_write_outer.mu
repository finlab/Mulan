unit para_write_outer;

int main() {
    int x = 0;
    para {
        { x = 1; }
    }
    return x;
}
