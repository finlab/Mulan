// is_rvalue_release — an owned scrutinee of `is` is released at scope end (#215).
unit is_rvalue_release;

extern int puts(const char* s);

int live = 0;
class Animal { Animal() { live += 1; } ~Animal() { live -= 1; } }
class Dog : Animal { public int tag; Dog(int t) { tag = t; } }
Animal fetch() { return new Dog(1); }

int main() {
    {
        if fetch() is Dog d {
            if d.tag != 1 { puts("tag"); return 1; }
        }
    }
    if live != 0 { puts("is leak"); return 1; }
    puts("is rvalue release ok");
    return 0;
}
