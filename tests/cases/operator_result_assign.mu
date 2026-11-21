// operator_result_assign — an operator result assigned to a struct/class destination is taken over
// instead of being copied and leaked (#157).
unit operator_result_assign;

extern int puts(const char* s);

int live = 0;

class Box {
    public int id;
    Box(int x) { id = x; live += 1; }
    ~Box() { live -= 1; }
}

Box operator+(Box a, Box b) {
    return new Box(a.id + b.id);
}

int main() {
    Box x = new Box(0);
    Box a = new Box(1);
    Box b = new Box(2);
    x = a + b;
    if x.id != 3 or live != 3 { puts("operator result"); return 1; }
    puts("operator result assign ok");
    return 0;
}
