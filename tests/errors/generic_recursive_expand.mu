unit generic_recursive_expand;

// Each expansion asks for a strictly larger type argument (`Deep<Box<T>>`), so no exact key repeats
// and the cycle guard alone would recurse until the stack overflows (#31). The depth limit reports a
// diagnostic instead.
struct Box<T> {
    T v;
}

struct Deep<T> {
    Deep<Box<T>> inner;
}

int main() {
    Deep<int> d;
    return 0;
}
