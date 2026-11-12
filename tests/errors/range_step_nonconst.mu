unit rs1;

int main() {
    int s = 2;
    int n = 0;
    for i in 0..10 step s {
        n += i;
    }
    return n;
}
