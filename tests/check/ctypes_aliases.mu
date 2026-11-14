unit ctypes_aliases;

use std.ctypes;

struct Header {
    c_int kind;
    c_uint flags;
}

// C-shaped declarations use the ABI names, not Mu's fixed-width ones: `c_long` is 32-bit on Windows
// and 64-bit on POSIX, so this declaration stays correct on every target.
extern c_long lseek(int fd, c_long offset, c_int whence);
extern c_size fread(void* buf, c_size size, c_size n, void* stream);

c_size header_size() { return sizeof(Header) as c_size; }

int main() {
    Header h = { kind = 1, flags = 2 };
    c_long off = 0;
    c_size n = header_size() + (sizeof(c_long) as c_size);
    c_int signedness = h.kind;
    if off != 0 {
        return 1;
    }
    if n == 0 {
        return 2;
    }
    return signedness == 1 ? 0 : 3;
}
