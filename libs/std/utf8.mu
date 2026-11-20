// std.utf8 — low-level UTF-8 codec helpers shared by std.string and std.unicode.
//
// Not a user-facing string API: these are the byte-level decode/encode primitives. They are public
// because `private` is unit-scoped, and both std.string and std.unicode need them. The packed
// decode form keeps the code point in the low 21 bits and the sequence length in bits 24-26;
// invalid input yields U+FFFD with length 1 (each bad byte is one replacement).

public unit std.utf8;

// The byte-sequence view: the UTF-8 primitives read through it, and std.unicode's extension adds
// methods to every implementor. String is the only implementor today.
public interface IString {
    isize length() const;
    byte at(isize i) const;
}

public isize utf8_len(isize bits) {
    return (bits >> 24) & 7;
}

public int utf8_cp(isize bits) {
    return (bits & 0x1FFFFF) as int;
}

public isize utf8_decode<T: IString>(T s, isize off) {
    isize n = s.length();
    int b0 = s.at(off) as int;
    if b0 < 0x80 {
        return (1 << 24) | b0;
    }
    if (b0 & 0xE0) == 0xC0 {
        if off + 1 >= n {
            return (1 << 24) | 0xFFFD;
        }
        int b1 = s.at(off + 1) as int;
        if (b1 & 0xC0) != 0x80 {
            return (1 << 24) | 0xFFFD;
        }
        int cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
        if cp < 0x80 {
            return (1 << 24) | 0xFFFD;
        }
        return (2 << 24) | cp;
    }
    if (b0 & 0xF0) == 0xE0 {
        if off + 2 >= n {
            return (1 << 24) | 0xFFFD;
        }
        int b1 = s.at(off + 1) as int;
        int b2 = s.at(off + 2) as int;
        if (b1 & 0xC0) != 0x80 or (b2 & 0xC0) != 0x80 {
            return (1 << 24) | 0xFFFD;
        }
        int cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        if cp < 0x800 or (cp >= 0xD800 and cp <= 0xDFFF) {
            return (1 << 24) | 0xFFFD;
        }
        return (3 << 24) | cp;
    }
    if (b0 & 0xF8) == 0xF0 {
        if off + 3 >= n {
            return (1 << 24) | 0xFFFD;
        }
        int b1 = s.at(off + 1) as int;
        int b2 = s.at(off + 2) as int;
        int b3 = s.at(off + 3) as int;
        if (b1 & 0xC0) != 0x80 or (b2 & 0xC0) != 0x80 or (b3 & 0xC0) != 0x80 {
            return (1 << 24) | 0xFFFD;
        }
        int cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        if cp < 0x10000 or cp > 0x10FFFF {
            return (1 << 24) | 0xFFFD;
        }
        return (4 << 24) | cp;
    }
    return (1 << 24) | 0xFFFD;
}

public isize utf8_cp_len(int cp) {
    if cp < 0 {
        return 3;
    }
    if cp < 0x80 {
        return 1;
    }
    if cp < 0x800 {
        return 2;
    }
    if cp < 0x10000 {
        return 3;
    }
    if cp <= 0x10FFFF {
        return 4;
    }
    return 3;
}

public isize utf8_put(byte[] buf, isize at, int cp) {
    if cp < 0 or cp > 0x10FFFF or (cp >= 0xD800 and cp <= 0xDFFF) {
        cp = 0xFFFD;
    }
    if cp < 0x80 {
        buf[at] = cp as byte;
        return 1;
    }
    if cp < 0x800 {
        buf[at] = (0xC0 | (cp >> 6)) as byte;
        buf[at + 1] = (0x80 | (cp & 0x3F)) as byte;
        return 2;
    }
    if cp < 0x10000 {
        buf[at] = (0xE0 | (cp >> 12)) as byte;
        buf[at + 1] = (0x80 | ((cp >> 6) & 0x3F)) as byte;
        buf[at + 2] = (0x80 | (cp & 0x3F)) as byte;
        return 3;
    }
    buf[at] = (0xF0 | (cp >> 18)) as byte;
    buf[at + 1] = (0x80 | ((cp >> 12) & 0x3F)) as byte;
    buf[at + 2] = (0x80 | ((cp >> 6) & 0x3F)) as byte;
    buf[at + 3] = (0x80 | (cp & 0x3F)) as byte;
    return 4;
}
