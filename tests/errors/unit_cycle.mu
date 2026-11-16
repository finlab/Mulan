unit unit_cycle;

// A back edge in the `use` graph is a cycle, not a valid partial view of the unit (#38).
use lib_cycle_a;

int main() {
    return cycle_a();
}
