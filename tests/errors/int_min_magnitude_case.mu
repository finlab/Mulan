unit int_min_magnitude_case;

// A `2^63` magnitude is only valid directly after unary `-`; as a switch case it is out of range
// (it must not silently become the value 0).

int f(long x) {
    switch (x) {
        9223372036854775808: { return 1; }
        else: { return 0; }
    }
}

int main() { return f(0); }
