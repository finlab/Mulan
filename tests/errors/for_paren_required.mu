unit for_paren_required;

// The C-style head requires the parentheses: without them `for` is the for-in form.

int main() {
    int i = 0;
    for i = 0; i < 3; i++ { }
    return 0;
}
