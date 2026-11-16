// nocopy_use — a `nocopy` struct cannot be copied, but it is an ordinary value otherwise: it can be
// initialized, assigned field by field, stored as a field, and reached through a pointer or a `ref`
// (agents/value-semantics.md §8.6).
unit nocopy_use;

extern int printf(const char* fmt, ...);

nocopy struct Handle { int fd; }

struct Holder { Handle h; int n; }

void bump(ref Handle h) { h.fd = h.fd + 1; }

int main() {
    Handle a = { 3 };             // aggregate initialization is not a copy
    if a.fd != 3 { printf("init\n"); return 1; }

    a.fd = 4;                     // field assignment is fine
    bump(a);                      // borrowing is fine
    if a.fd != 5 { printf("borrow\n"); return 2; }

    Handle* p = &a;
    p->fd = 6;
    if a.fd != 6 { printf("pointer\n"); return 3; }

    Holder o;
    o.h.fd = 7;                   // a field of a nocopy type is fine; only copying the host is refused
    o.n = 1;
    if o.h.fd != 7 { printf("field\n"); return 4; }
    if o.n != 1 { printf("host\n"); return 5; }

    printf("nocopy_use ok\n");
    return 0;
}
