unit derive_compare_missing;

struct EqOnly {
    int v;

    bool operator==(EqOnly o) { return v == o.v; }
}

int main() {
    EqOnly a = {1};
    EqOnly b = {2};
    // no `>` and no `<` to derive it from
    return (a > b) ? 1 : 0;
}
