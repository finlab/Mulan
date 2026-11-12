// mu standard library: string
//
// This is a pure mu implementation; the compiler only handles fast fallbacks for
// literals, s[i], len, ==/!=, for-in and other canonical layouts.
//
// string is a value-type struct: assignment and argument passing copy the three
// fields (p/n/owner); owner is an RC byte array that keeps the backing buffer alive.

public unit std.string;

public struct string {
private:
    byte* p;
    usize n;
    byte[] owner;

public:
    // construct from a NUL-terminated C string (copies it, does not hold c)
    string(const char* c) {
        usize count = 0;
        while c[count] != 0 {
            count += 1;
        }
        byte[] buf = new byte[count + 1];
        usize i = 0;
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

    usize length() {
        return n;
    }

    bool empty() {
        return n == 0;
    }

    bool is_empty() {
        return n == 0;
    }

    // out of range returns 0 (can be changed to panic in debug builds)
    byte at(usize i) {
        if i >= n {
            return 0;
        }
        return p[i];
    }

    // NUL-terminated view; both literals and normal strings guarantee a trailing 0
    const char* cstr() {
        return p as const char*;
    }

    // ---------------------------------------------------------------- slicing and concatenation

    string sub(usize start, usize count) {
        string r = {};
        r.p = p + start;
        r.n = count;
        r.owner = owner;
        return r;
    }

    string concat(string other) {
        usize total = n + other.n;
        byte[] buf = new byte[total + 1];
        usize i = 0;
        while i < n {
            buf[i] = p[i];
            i += 1;
        }
        usize j = 0;
        while j < other.n {
            buf[n + j] = other.p[j];
            j += 1;
        }
        buf[total] = 0;
        string r = {};
        r.p = &buf[0];
        r.n = total;
        r.owner = buf;
        return r;
    }

    // ---------------------------------------------------------------- search and match

    // returns the byte offset of the first occurrence, or -1 if not found
    int find(string needle) {
        if needle.n == 0 {
            return 0;
        }
        if needle.n > n {
            return -1;
        }
        usize i = 0;
        while i + needle.n <= n {
            usize j = 0;
            bool same = true;
            while j < needle.n {
                if p[i + j] != needle.p[j] {
                    same = false;
                    break;
                }
                j += 1;
            }
            if same {
                return i as int;
            }
            i += 1;
        }
        return -1;
    }

    bool starts_with(string prefix) {
        if prefix.n > n {
            return false;
        }
        usize i = 0;
        while i < prefix.n {
            if p[i] != prefix.p[i] {
                return false;
            }
            i += 1;
        }
        return true;
    }

    bool ends_with(string suffix) {
        if suffix.n > n {
            return false;
        }
        usize off = n - suffix.n;
        usize i = 0;
        while i < suffix.n {
            if p[off + i] != suffix.p[i] {
                return false;
            }
            i += 1;
        }
        return true;
    }

    // ---------------------------------------------------------------- trimming

    string trim() {
        usize start = 0;
        while start < n and is_space_byte(p[start]) {
            start += 1;
        }
        usize stop = n;
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

// operators can only be free functions (language limitation); thin wrappers over the member methods.
public string operator+(string a, string b) {
    return a.concat(b);
}
