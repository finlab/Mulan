// The type argument appears only in the return type, and a call statement has no target type to
// determine it: an explicit list is the only way to call this template (SPEC 3.15.3).
unit generic_call_no_type_args;

T empty<T>() {
    T[] none = null;
    return none[0];
}

int main() {
    empty();
    return 0;
}
