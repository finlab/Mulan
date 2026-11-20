unit match_const_bool_missing;

int f(bool b) {
    return match b {
        true: 1;
    };
}

int main() { return f(true); }
