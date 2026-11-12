// generics.mu — generics are monomorphized: one copy of the code per type-argument combination.
//
// There is no boxing, no vtable and no runtime type information. `<>` is expanded during
// semantic analysis (clone the AST, substitute the type parameters, canonicalize the name),
// so the backend never sees generics at all and LLVM can inline every instantiation.
//
// Constraints are structural interfaces: a type satisfies `IAddable` just by having a matching
// `operator+`, with no `implements` clause anywhere. Builtin types satisfy them at zero cost.

unit generics;

extern int printf(const char* fmt, ...);

// ---------- Constraints ----------
// `Self` refers to the implementor, so one interface covers every type.
interface IAddable {
    Self operator+(Self a, Self b);
}

interface IOrdered {
    bool operator<(Self a, Self b);
}

T max<T: IOrdered>(T a, T b) {
    return a < b ? b : a;
}

// ---------- Generic Constraints
// 你可以在泛型约束中使用多个接口，使用 `&` 连接它们。下面的函数要求类型参数 `T` 同时满足 `IAddable` 和 `IOrdered` 接口。
T foo<T: IAddable & IOrdered>(T a, T b) {
    return a + b;
}

T bar<T: int| long |string>(T a, T b) {
    return a + b;
}
// ---------- Generic struct with methods ----------
struct Vec<T> {
    T[] data;

    usize length() { return len(data); }

    T at(usize i) { return data[i]; }
}

// ---------- Generic free functions ----------
// One definition; the compiler emits a separate function for every type argument.
T max<T: IOrdered>(T a, T b) {
    return a < b ? b : a;
}

// The parameter type is itself a generic instantiation (`Vec<T>`): nested instantiation works.
T sum<T: IAddable>(Vec<T> v, T zero) {
    T total = zero;
    for i in 0..v.length() {
        total = total + v.at(i);
    }
    return total;
}

// ---------- A user type that satisfies the constraints without declaring anything ----------
struct Money {
    long cents;
}

Money operator+(Money a, Money b) { return Money(a.cents + b.cents); }
bool operator<(Money a, Money b) { return a.cents < b.cents; }

int main() {
    // Instantiation for a builtin type: no wrapper, no allocation.
    int big = max<int>(3, 41);

    Vec<int> v = {new int[3]};
    v.data[0] = 1;
    v.data[1] = 2;
    v.data[2] = 3;
    int s = sum<int>(v, 0);

    // Same source, a second set of machine code: `Vec<Money>` and `Money` are baked in.
    Vec<Money> mv = {new Money[2]};
    mv.data[0] = Money(10);
    mv.data[1] = Money(30);
    Money total = sum<Money>(mv, Money(2));
    Money top = max<Money>(Money(3), total);

    printf("max<int>(3, 41) = %d\n", big);
    printf("sum<int>        = %d\n", s);
    printf("sum<Money>      = %lld cents\n", total.cents);
    printf("max<Money>      = %lld cents\n", top.cents);

    if (big == 41 and s == 6 and total.cents == 42 and top.cents == 42) {
        printf("generics ok\n");
    }
    return 0;
}
