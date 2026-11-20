unit match_const_overlap;

int main() {
    int n = 3;
    int x = match n {
        1..4: 1;
        3: 2;
        else: 0;
    };
    return 0;
}
