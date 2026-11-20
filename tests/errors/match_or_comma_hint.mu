// `|` is the bitwise-or operator; alternatives without a payload are written with a comma list.
unit match_or_comma_hint;

variant V {
    None;
    Empty;
}

int f(V v) {
    return match v {
        None | Empty: 1;
    };
}

int main() { return f(None); }
