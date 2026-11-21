unit if_float_cast_range;
#if (1e300 as long) == 0 {
    #error "should not be reachable"
}
int main() { return 0; }
