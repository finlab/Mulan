// The type argument appears only in the return type, so no argument can determine it: an explicit
// list is the only way to call this template (SPEC 3.15.3).
unit generic_call_no_type_args;

T empty<T>() {
    T[] none = null;
    return none[0];
}

int main() {
    int a = empty();
    return a;
}
