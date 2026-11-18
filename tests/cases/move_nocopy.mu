// `nocopy` is about copying: the value can still be moved (the compiler transfers the bits and empties
// the source), which is what a handle type wants.
unit move_nocopy;

extern int printf(const char* fmt, ...);

nocopy struct Handle { int fd; }

int take(Handle h) { return h.fd; }

int main() {
    Handle h;
    h.fd = 9;
    Handle k = move h;
    if k.fd != 9 { printf("moved %d\n", k.fd); return 1; }
    // `h` is dead after the move (reading it is a compile error); the value travels on through `k`.
    if take(move k) != 9 { printf("by value failed\n"); return 3; }
    printf("move_nocopy ok\n");
    return 0;
}
