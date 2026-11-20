unit match_const_variant_pat;

int main() {
    int n = 3;
    int x = match n {
        Some(v): 1;
        else: 0;
    };
    return 0;
}
