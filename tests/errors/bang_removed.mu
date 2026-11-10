unit bang_removed;
int main() {
    int x = 1;
    bool b = false;
    if !b { return x; }
    return ~x;
}