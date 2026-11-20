// Nested patterns are a later phase: a payload position binds a name, not another pattern.
unit match_nested_pattern;

variant O {
    Some int;
    None;
}

int f(O o) {
    return match o {
        Some(Some(x)): 1;
        None: 0;
    };
}

int main() { return f(Some(1)); }
