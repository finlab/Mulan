unit cmw;

struct P {
    int x;
    int bad() const { x = 1; return x; }
}

int main() { return 0; }
