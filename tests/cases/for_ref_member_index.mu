unit for_ref_member_index;

extern int puts(const char* s);

// `for ref` through a member `operator[]` that returns `ref T` (issue #27): the borrow is the
// element itself, so writing through the loop variable mutates the container.
struct Vec {
    int[3] data;
    int count;
    usize length() { return count as usize; }
    ref int operator[](usize i) { return data[i]; }
}

int main() {
    Vec v = {{1, 2, 3}, 3};
    for ref x in v { x += 10; }
    if v.data[0] != 11 or v.data[1] != 12 or v.data[2] != 13 {
        puts("struct member ref failed");
        return 1;
    }

    // index + ref binding over the member overload
    int sum = 0;
    for i, ref x in v { x += i as int; sum += x; }
    if v.data[0] != 11 or v.data[1] != 13 or v.data[2] != 15 or sum != 39 {
        puts("struct member ref index failed");
        return 1;
    }

    // an empty sequence runs the loop body zero times
    Vec e = {{0, 0, 0}, 0};
    int runs = 0;
    for ref z in e { runs += 1; }
    if runs != 0 or e.data[0] != 0 {
        puts("empty member ref failed");
        return 1;
    }

    puts("for ref member index ok");
    return 0;
}
