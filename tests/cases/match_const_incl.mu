// match_const_incl.mu — match branch ranges support `..=` (inclusive)
unit match_const_incl;

extern int puts(const char* s);

int classify(int v) {
    match v {
        0..=9: {
            return 1;
        }
        10..=99: {
            return 2;
        }
        else: {
            return 3;
        }
    }
}

int main() {
    if classify(0) == 1 and classify(9) == 1 and classify(10) == 2 and classify(99) == 2 and classify(100) == 3 {
        puts("match inclusive ok");
    }
    return 0;
}
