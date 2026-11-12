unit pe1;

int f(const int x) {
    x = 3;
    return x;
}

int main() {
    return f(1);
}
