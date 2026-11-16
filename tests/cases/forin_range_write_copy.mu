unit forin_range_write_copy;
use std.cstdio;

// The range loop variable is a per-iteration copy, like sequence for-in: writing it must not
// advance or rewind the hidden counter (`i = 4` used to skip i=3 and end the loop early).
int main() {
    int hits = 0;
    int seen = 0;
    for i in 0..5 {
        hits += 1;
        if i == 2 {
            i = 4;
        }
        seen = seen * 10 + i;
    }
    printf("hits=%d seen=%d\n", hits, seen);
    if hits != 5 {
        return 1;
    }
    if seen != 1434 {
        return 2;
    }
    return 0;
}
