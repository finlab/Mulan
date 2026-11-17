unit if_dead_use;
use comptime;

// The arm is false on every platform, so its `use` never joins the dependency graph: a unit that
// does not exist here must not be an error.
#if os == OS.Windows and os == OS.Linux {
    use nosuch.thing;
}

int main() {
    return 0;
}
