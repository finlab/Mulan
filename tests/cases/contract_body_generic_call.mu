// contract_body_generic_call.mu — a constrained generic may call another one; the definition-time
// check must not leave a witness instantiation behind that fails to link
unit contract_body_generic_call;

extern int puts(const char* s);

interface IAdd {
    Self operator+(Self a, Self b);
}

struct N {
    int v;

    N operator+(N o) { return N(v + o.v); }
}

T twice<T: IAdd>(T a) { return a + a; }

// calls a generic from inside a generic: `twice<T>` is checked at this definition with the witness
// bound to T, and at the real instantiation with the concrete type
T quad<T: IAdd>(T a) {
    T doubled = twice<T>(a);
    return twice<T>(doubled);
}

int main() {
    N n = {3};
    N q = quad<N>(n);
    if q.v != 12 { puts("quad"); return 1; }
    puts("contract generic call ok");
    return 0;
}
