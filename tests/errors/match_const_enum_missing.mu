unit match_const_enum_missing;

enum Color { Red, Green, Blue }

int main() {
    Color c = Red;
    int x = match c {
        Red: 1;
        Blue: 2;
    };
    return 0;
}
