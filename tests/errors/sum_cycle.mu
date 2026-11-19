// Two sums storing each other by value: neither has a finite layout. The direct self-cycle is
// caught when the payload is resolved; a cycle through another sum only shows up while the layout
// is computed, so `check_layouts` validates every sum and reports the cycle.
unit sum_cycle;

variant A {
    Wrap B;
    None;
}
variant B {
    Wrap A;
    None;
}

int main() { return 0; }
