unit if_cross_enum;
use comptime;
// `#if` follows the same rule as ordinary code: enum values compare only with the same enum type.
int main() {
    #if os == Endian.Little {
        return 1;
    }
    return 0;
}
