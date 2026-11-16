unit enum_duplicate_member;

// Unqualified enum members share the unit's namespace: two enums cannot both declare `X`,
// because an unqualified `X` would silently bind to the first one (#37).
enum A { X = 1 }
enum B { X = 2 }

int main() {
    return 0;
}
