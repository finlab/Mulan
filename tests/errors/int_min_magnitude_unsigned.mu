unit int_min_magnitude_unsigned;

// A `2^63` magnitude is only the spelling of a signed minimum: without the directly applied unary
// `-` it is out of range for any 64-bit type.

int main() {
    long a = 9223372036854775808;
    return 0;
}
