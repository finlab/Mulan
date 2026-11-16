// move_cleanup_nested_scope — re-assigning a moved variable inside a nested block restores it for the
// scope that declared it (issue #21). The revive must not register the variable with the inner block:
// the value still owns its resource after the inner block ends, and only the outer block releases it.
unit move_cleanup_nested_scope;

extern int printf(const char* fmt, ...);

int alive = 0;

struct R {
    int* p;
    R(int n) {
        p = alloc(1) as int*;
        p[0] = n;
        alive += 1;
    }
    R(move R other) {
        p = other.p;
        other.p = null;
    }
    ~R() {
        if p != null {
            free(p);
            alive -= 1;
        }
    }
}

int main() {
    {
        R x = R(1);
        {
            R temp = move x;
            x = move temp;
        }
        if alive != 1 {
            printf("inner block released x: alive=%d\n", alive);
            return 1;
        }
        if x.p == null {
            printf("x lost its value\n");
            return 2;
        }
        if x.p[0] != 1 {
            printf("x holds %d\n", x.p[0]);
            return 3;
        }
    }
    if alive != 0 {
        printf("outer block did not release x: alive=%d\n", alive);
        return 4;
    }
    printf("move_cleanup_nested_scope ok\n");
    return 0;
}
