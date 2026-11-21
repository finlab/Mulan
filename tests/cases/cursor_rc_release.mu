// cursor_rc_release — a cursor yielding RC elements does not retain them again (#214), and a struct
// cursor with an RC field is released after the loop (#221).
unit cursor_rc_release;

extern int puts(const char* s);

int live = 0;
class Item { public int id; Item(int i) { id = i; live += 1; } ~Item() { live -= 1; } }

struct Cursor {
    Item[] items;
    int i;
    bool next() { if i < items.length() { i += 1; return true; } return false; }
    Item current() { return items[i - 1]; }
}

class Src {
    public Item[] items;
    Src() { items = new Item[3]; for i in 0..3 { items[i] = new Item(i); } }
    Cursor iterator() { Cursor c = { items = items, i = 0 }; return c; }
}

int main() {
    {
        Src s = new Src();
        for it in s { if it.id < 0 { puts("never"); } }
    }
    if live != 0 { puts("cursor leak"); return 1; }
    puts("cursor rc release ok");
    return 0;
}
