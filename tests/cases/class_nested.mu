unit class_nested;
extern int puts(const char* s);
int live = 0;
class C { int v; C(int x){v=x;live+=1;} ~C(){live-=1;} int get(){return v;} }
class Holder { C c; Holder(C x){ c = x; } ~Holder(){} C get(){return c;} }
class Animal { int s; Animal(int x){s=x;} ~Animal(){} int sound(){return s;} }
class Dog : Animal { Dog(){super(7);} ~Dog(){} }
int main() {
    Holder h = new Holder(new C(4));
    if h.get().get() != 4 { puts("nested new fail"); return 1; }
    if live != 1 { puts("nested new leak"); return 1; }
    h = null;
    if live != 0 { puts("holder dtor fail"); return 1; }

    Animal[2] arr = {};
    Dog d = new Dog();
    arr[0] = d;
    if arr[0].sound() != 7 { puts("base array virtual fail"); return 1; }
    d = null;
    if arr[0].sound() != 7 { puts("base array rc fail"); return 1; }
    puts("class nested ok");
    return 0;
}

