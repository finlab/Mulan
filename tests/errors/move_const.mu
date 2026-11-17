unit move_const;

struct S { int x; }

int main() {
    const S a = {1};
    S b = move a;
    return 0;
}
