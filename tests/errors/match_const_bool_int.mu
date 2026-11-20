unit match_const_bool_int;

int f(bool b) {
    return match b {
        1: 1;
        else: 0;
    };
}

int main() { return f(true); }
