// mu standard library: string
//
// A pure library type: the compiler knows nothing about its layout. A string literal is a
// NUL-terminated UTF-8 `const char*`, and `string s = cstr;` goes through
// `implicit string(const char*)` (a copy, one allocation).
//
// string is a value-type struct: assignment and argument passing copy the three
// fields (p/n/owner); owner is an RC byte array that keeps the backing buffer alive.

public unit std.string;

public struct String {
private:
    byte* p;
    isize n;
    byte[] owner;

public:
    // construct from a NUL-terminated C string (copies it, does not hold c).
    // `implicit`: `string s = "literal";` and `string s = cstr;` (3.6)
    implicit String(const char* c) {
        isize count = 0;
        while c[count] != 0 {
            count += 1;
        }
        byte[] buf = new byte[count + 1];
        isize i = 0;
        while i < count {
            buf[i] = c[i] as byte;
            i += 1;
        }
        buf[count] = 0;
        p = &buf[0];
        n = count;
        owner = buf;
    }

    // ---------------------------------------------------------------- basic info

    isize length() {
        return n;
    }

    bool empty() {
        return n == 0;
    }

    bool is_empty() {
        return n == 0;
    }

    // Out of range returns 0. `at` is the clamping accessor; the subscript below stays unchecked
    // (C-shaped raw access), so this one has to reject negatives explicitly now that the index is signed.
    byte at(isize i) {
        if i < 0 or i >= n {
            return 0;
        }
        return p[i];
    }

    // NUL-terminated view of the logical value. The zero value (p == null) is the empty string;
    // every other representation ends its buffer with a 0 at n: the constructor, `concat` and `sub`
    // each allocate their own buffer, so the view cannot expose bytes beyond the value (#32).
    const char* cstr() {
        if p == null {
            return "";
        }
        return p as const char*;
    }

    // ---------------------------------------------------------------- element access and comparison

    // `s[i]`: byte access (bounds are a user responsibility until the bounds policy is settled, 8.2)
    byte operator[](isize i) {
        return p[i];
    }

    bool operator==(String o) {
        if n != o.n {
            return false;
        }
        isize i = 0;
        while i < n {
            if p[i] != o.p[i] {
                return false;
            }
            i += 1;
        }
        return true;
    }

    // compare with a NUL-terminated C string (literals): `s == "abc"`
    bool operator==(const char* c) {
        isize i = 0;
        while i < n {
            if c[i] == 0 or p[i] != (c[i] as byte) {
                return false;
            }
            i += 1;
        }
        return c[i] == 0;
    }

    // `s + "literal"`: the literal is a `const char*` (3.6 implicit conversion is not used inside
    // expressions, so the overload is explicit here)
    String operator+(const char* c) {
        return concat(String(c));
    }

    String operator+(String o) {
        return concat(o);
    }

    // ---------------------------------------------------------------- slicing and concatenation

    // A substring copies into its own NUL-terminated buffer. A view into the source would share
    // the trailing bytes after the range, so `cstr()` could expose them (#32); out-of-range
    // arguments are clamped to the source length instead of producing a read past the buffer (#33).
    String sub(isize start, isize count) {
        if start > n {
            start = n;
        }
        if count > n - start {
            count = n - start;
        }
        byte[] buf = new byte[count + 1];
        isize i = 0;
        while i < count {
            buf[i] = p[i + start];
            i += 1;
        }
        buf[count] = 0;
        String r = {};
        r.p = &buf[0];
        r.n = count;
        r.owner = buf;
        return r;
    }

    String concat(String other) {
        isize total = n + other.n;
        byte[] buf = new byte[total + 1];
        isize i = 0;
        while i < n {
            buf[i] = p[i];
            i += 1;
        }
        isize j = 0;
        while j < other.n {
            buf[n + j] = other.p[j];
            j += 1;
        }
        buf[total] = 0;
        String r = {};
        r.p = &buf[0];
        r.n = total;
        r.owner = buf;
        return r;
    }

    // ---------------------------------------------------------------- search and match

    // returns the byte offset of the first occurrence, or -1 if not found.
    // `long` represents every offset on the supported 64-bit targets; `int` could turn a match
    // beyond 2^31-1 into a negative value indistinguishable from "not found" (#34).
    isize find(String needle) {
        if needle.n == 0 {
            return 0;
        }
        if needle.n > n {
            return -1;
        }
        isize i = 0;
        while i + needle.n <= n {
            isize j = 0;
            bool same = true;
            while j < needle.n {
                if p[i + j] != needle.p[j] {
                    same = false;
                    break;
                }
                j += 1;
            }
            if same {
                return i;
            }
            i += 1;
        }
        return -1;
    }

    bool starts_with(String prefix) {
        if prefix.n > n {
            return false;
        }
        isize i = 0;
        while i < prefix.n {
            if p[i] != prefix.p[i] {
                return false;
            }
            i += 1;
        }
        return true;
    }

    bool ends_with(String suffix) {
        if suffix.n > n {
            return false;
        }
        isize off = n - suffix.n;
        isize i = 0;
        while i < suffix.n {
            if p[off + i] != suffix.p[i] {
                return false;
            }
            i += 1;
        }
        return true;
    }

    // ---------------------------------------------------------------- trimming

    String trim() {
        isize start = 0;
        while start < n and is_space_byte(p[start]) {
            start += 1;
        }
        isize stop = n;
        while stop > start and is_space_byte(p[stop - 1]) {
            stop -= 1;
        }
        return sub(start, stop - start);
    }

private:
    bool is_space_byte(byte c) {
        return c == ' ' or c == '\t' or c == '\n' or c == '\r';
    }
}

// Lowercase alias kept for input convenience (agents/stdlib/style.md: aliases may be lowercase).
public type string = String;

// A C string on the left (`"abc" == s`): a free function because a member needs the receiver on the left.
public bool operator==(const char* c, String s) {
    return s == c;
}

public String operator+(const char* c, String s) {
    return String(c) + s;
}

// Byte length of a NUL-terminated C string: the compiler has no builtin `len` for pointers,
// so a literal's length is `strlen(literal)` (or convert to `string` and use `length()`).
public isize strlen(const char* c) {
    isize i = 0;
    while c[i] != 0 {
        i += 1;
    }
    return i;
}
