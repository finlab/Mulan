unit match_const_bool_range;

int f(bool b) {
    return match b {
        false..true: 1;
        else: 0;
    };
}

int main() { return f(true); }
