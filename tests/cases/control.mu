// control.mu — if/elif/else, while + break/continue, for i in 0..n
unit control;

extern int puts(const char* s);
extern int putchar(int c);

void print_int(int v) {
    if v == 0 {
        putchar('0');
        return;
    }
    if v < 0 {
        putchar('-');
        v = -v;
    }
    int[20] digits;
    int i = 0;
    while v > 0 {
        digits[i] = v % 10;
        v /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        putchar('0' + digits[i]);
    }
}

int main() {
    // ---------- if / elif / else: the first matching branch wins, in order ----------
    int score = 72;
    if score >= 90 {
        puts("grade A");
    } elif score >= 80 {
        puts("grade B");
    } elif score >= 70 {
        puts("grade C");
    } else {
        puts("grade F");
    }

    // the condition must be a bool: != yields bool
    if score != 100 {
        puts("not perfect");
    }

    // ---------- while + break / continue ----------
    // while true is legal (true is a bool); continue jumps back to the condition, break exits
    int n = 0;
    int sum = 0;
    while true {
        n += 1;
        if n % 2 == 0 {
            continue;          // skip even numbers
        }
        if n > 10 {
            break;             // up to 11
        }
        sum += n;
    }
    print_int(sum); putchar('\n');   // 1+3+5+7+9 = 25
    print_int(n); putchar('\n');     // 11

    // ---------- while counting down ----------
    int m = 100;
    int steps = 0;
    while m > 1 {
        m /= 2;
        steps += 1;
    }
    print_int(steps); putchar('\n'); // 100->50->25->12->6->3->1: 6 steps
    print_int(m); putchar('\n');     // 1

    // ---------- for i in 0..n (half-open range, n excluded) ----------
    for i in 0..5 {
        print_int(i); putchar(' ');
    }
    putchar('\n');                   // 0 1 2 3 4

    // nested loops: break only leaves the inner loop
    for i in 0..3 {
        for j in 0..10 {
            if j == 2 {
                break;
            }
            print_int(i * 10 + j); putchar(' ');
        }
    }
    putchar('\n');                   // 0 1 10 11 20 21

    // for + continue; range endpoints can be expressions
    for i in 0..3 * 3 {
        if i % 3 == 0 {
            continue;
        }
        print_int(i); putchar(' ');
    }
    putchar('\n');                   // 1 2 4 5 7 8

    return 0;
}
