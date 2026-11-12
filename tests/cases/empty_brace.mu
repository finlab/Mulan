// empty_brace.mu — empty braces `{}` zero-init: array / struct / slice / string
unit empty_brace;

extern int puts(const char* s);

struct Point { int x, y; }
struct Box { int[3] v; Point p; string name; }

Point gpoint = {};   // unit-level aggregate zero value
int[3] garr = {};

int main() {
    Point p = {};
    if p.x != 0 or p.y != 0 { puts("bad point"); return 1; }

    int[3] a = {};
    if a[0] != 0 or a[1] != 0 or a[2] != 0 { puts("bad array"); return 2; }

    int[2,2] m = {};
    if m[0,1] != 0 or m[1,1] != 0 { puts("bad multidim"); return 3; }

    int[] d = {};
    if len(d) != 0 { puts("bad dynarray"); return 4; }

    string s = {};
    if len(s) != 0 or not (s == "") { puts("bad string"); return 5; }

    // nested empty braces: fields that are arrays / structs / strings
    Box b = {v = {}, p = {}, name = {}};
    if b.v[2] != 0 or b.p.y != 0 or len(b.name) != 0 { puts("bad nested"); return 6; }

    Box c = {};
    if c.v[0] != 0 or c.p.x != 0 or len(c.name) != 0 { puts("bad struct"); return 7; }

    if gpoint.x != 0 or garr[2] != 0 { puts("bad globals"); return 8; }

    puts("empty brace ok");
    return 0;
}
