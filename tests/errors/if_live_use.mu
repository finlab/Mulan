unit if_live_use;
use comptime;

#if ptr_bits > 0 {
    use nosuch.thing;
}

int main() { return 0; }
