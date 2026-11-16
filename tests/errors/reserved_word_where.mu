// SPEC 2.5: `where` is reserved for a future extension (the constraint syntax uses `<T: I>` today), so
// it cannot be used as an identifier.
unit reserved_word_where;

int where = 1;

int main() {
    return where;
}
