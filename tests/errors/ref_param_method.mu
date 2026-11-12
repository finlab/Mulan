unit rp7;

struct P {
    int x;

    void bump(ref P other) {
        other.x = 2;
    }
}

int main() {
    P a = {1};
    a.bump(a);
    return a.x;
}
