// packed structs cannot directly contain a string (unaligned pointer access)
unit packed_string_field;

packed struct Bad {
    byte tag;
    string s;
}

int main() { return 0; }
