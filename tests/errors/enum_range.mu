unit enum_range;

enum Color { Red, Green }

int main() {
    int t = 0;
    for i in Red..Green {
        t += i;
    }
    return t;
}
