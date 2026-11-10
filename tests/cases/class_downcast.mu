unit class_downcast;
extern int puts(const char* s);
class A { int a; A(){a=1;} ~A(){} int base(){return a;} }
class B : A { int b; B(){super(); b=2;} ~B(){} int child(){return b;} }
class D : A { int d; D(){super(); d=3;} ~D(){} }
interface I { int f(); }
class E : A, I { E(){super();} override int f(){return 9;} }
int main() {
    A ab = new B();
    B b1 = ab as B;
    if b1 == null { puts("valid downcast failed"); return 1; }
    if b1.child() != 2 { puts("downcast method failed"); return 1; }

    A ad = new D();
    B b2 = ad as B;
    if b2 != null { puts("invalid downcast not null"); return 1; }

    A an = null;
    B b3 = an as B;
    if b3 != null { puts("null downcast not null"); return 1; }

    E e = new E();
    I i = e;
    E e2 = i as E;
    if e2 == null { puts("iface downcast failed"); return 1; }
    if e2.f() != 9 { puts("iface downcast call failed"); return 1; }

    A other = new D();
    I i2 = e;
    B b4 = i2 as B;   // 动态类型 E，也不是 B
    if b4 != null { puts("iface downcast wrong"); return 1; }

    del ab; del ad; del e; del other;
    puts("downcast ok");
    return 0;
}

