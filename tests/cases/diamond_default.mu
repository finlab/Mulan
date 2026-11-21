// diamond_default — a default method inherited through a diamond interface is collected once (#144).
unit diamond_default;

extern int puts(const char* s);

interface Root {
    int tag();
    int value() { return 7; }
}
interface Left : Root { }
interface Right : Root { }
class Diamond : Left, Right {
    public int t;
    Diamond() { t = 0; }
    override int tag() { return t; }
}

int main() {
    Diamond d = new Diamond();
    if d.value() != 7 { puts("default"); return 1; }
    puts("diamond default ok");
    return 0;
}
