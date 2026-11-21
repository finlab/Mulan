// match_jump_cleanup — break/continue/return inside a match arm do not break the scrutinee temp
// cleanup of the current or later iterations (#223, #225).
unit match_jump_cleanup;

extern int puts(const char* s);

int live = 0;
class Box { public int id; Box(int i) { id = i; live += 1; } ~Box() { live -= 1; } }
variant Opt { Some Box; None; }
Opt make(int i) { return Some(new Box(i)); }

Box find_first() {
    for i in 0..20 {
        match make(i) {
            Some(b): { if b.id == 3 { return b; } }
            None: { }
        }
    }
    return null;
}

int main() {
    for i in 0..20 {
        match make(i) {
            Some(b): { if b.id % 3 == 0 { continue; } }
            None: { }
        }
    }
    if live != 0 { puts("continue leak"); return 1; }

    for i in 0..6 {
        match make(i) {
            Some(b): { if b.id == 5 { break; } }
            None: { }
        }
    }
    if live != 0 { puts("break leak"); return 1; }

    Box b = find_first();
    if b.id != 3 or live != 1 { puts("return leak"); return 2; }
    b = null;
    if live != 0 { puts("return release"); return 3; }
    puts("match jump cleanup ok");
    return 0;
}
