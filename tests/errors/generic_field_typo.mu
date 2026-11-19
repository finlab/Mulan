unit generic_field_typo;

class Box<T> {
    public T _value;
    Box(move T value) { this.value = move value; }   // typo: the field is `_value`
}

int main() {
    Box<int> b = new Box<int>(42);
    return 0;
}
