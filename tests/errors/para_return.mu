unit para_return;

void f() {
    para {
        { return; }
    }
}

int main() {
    f();
    return 0;
}
