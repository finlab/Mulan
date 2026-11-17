// `use comptime;` is the short spelling of the unit `std.comptime`; the loader points at the real name
// (SPEC 9.3).
unit use_short_comptime;

use comptime;

int main() { return 0; }
