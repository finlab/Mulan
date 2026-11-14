unit delegate_this_outside;
type Get = delegate () => int;

int main() {
    // `[this]` outside a method has no receiver
    Get g = [this] () => 1;
    return g();
}
