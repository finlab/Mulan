// move_cleanup_conditional — a `move` inside a branch that is not taken must not cancel the cleanup of
// the enclosing scope (issue #23). Whether a local still holds its value belongs to the control-flow
// path taken at run time, not to the lexical scope the `move` is written in.
unit move_cleanup_conditional;

extern int printf(const char* fmt, ...);

int alive = 0;

struct R {
    int id;
    R(int n) {
        id = n;
        alive += 1;
    }
    // `alive` counts objects: the moved-from value is still one (its destructor runs, SPEC 3.13), and
    // the move constructs a second one.
    R(move R other) {
        id = other.id;
        alive += 1;
    }
    ~R() { alive -= 1; }
}

int main() {
    // 1. The move is never executed: the enclosing scope must still destroy the value.
    {
        R x = R(1);
        if false {
            R y = move x;
        }
        if alive != 1 {
            printf("destroyed early: alive=%d\n", alive);
            return 1;
        }
    }
    if alive != 0 {
        printf("leaked: alive=%d\n", alive);
        return 2;
    }

    // 2. The move is executed: the inner scope owns the value from then on, and the outer scope must
    //    not destroy it a second time.
    {
        R x = R(2);
        if true {
            R y = move x;
            // `x` is a live shell after the move (its destructor runs at the outer scope's end).
            if alive != 2 {
                printf("move dropped the value: alive=%d\n", alive);
                return 3;
            }
            if y.id != 2 {
                printf("moved value id=%d\n", y.id);
                return 4;
            }
        }
        if alive != 1 {
            printf("inner scope cleanup: alive=%d\n", alive);
            return 5;
        }
    }

    // 3. Each loop iteration starts unmoved again: iteration 0 moves the value out of `x`, iteration 1
    //    leaves it in place and the loop body scope destroys it.
    {
        int n = 0;
        while n < 2 {
            R x = R(10 + n);
            if n == 0 {
                R y = move x;
            }
            n += 1;
        }
        if alive != 0 {
            printf("loop cleanup: alive=%d\n", alive);
            return 6;
        }
    }

    // 4. `break` runs the cleanup of the scopes it leaves, with the same path-sensitivity.
    {
        int k = 0;
        while k < 1 {
            R x = R(7);
            if false {
                R y = move x;
            }
            k += 1;
            break;
        }
        if alive != 0 {
            printf("break cleanup: alive=%d\n", alive);
            return 7;
        }
    }

    printf("move_cleanup_conditional ok\n");
    return 0;
}
