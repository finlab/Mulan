// std.unicode — Unicode operations on String.
//
// Unicode needs data tables, so this unit is NOT loaded implicitly: `use std.unicode;` both brings
// the helper functions into scope and activates the extension that adds the Unicode method names
// to String (SPEC 3.18). Without the `use`, those methods are a compile error (the core String API
// only has the `_ascii` spellings and the code-point structure helpers).
//
// Scope of this first version: code point properties (alpha/digit/space/upper/lower), simple
// one-to-one case mapping, and whitespace strip. Normalization, grapheme/word/line segmentation and
// locale collation are not implemented.

public unit std.unicode;

use std.string;
use std.utf8;
use std.unicode_data;

// The method surface is an interface extension: String names `IString`, and every unit that uses
// this unit gains these defaults on String. The bodies call the generic helpers below, so they work
// for any IString implementor and materialize statically for a struct.
public interface IUnicode : IString {
    String lower() { return unicode_lower(this); }
    String upper() { return unicode_upper(this); }
    String strip() { return unicode_strip(this); }
    String lstrip() { return unicode_lstrip(this); }
    String rstrip() { return unicode_rstrip(this); }
    bool isalpha() { return unicode_isalpha(this); }
    bool isdigit() { return unicode_isdigit(this); }
    bool isalnum() { return unicode_isalnum(this); }
    bool isspace() { return unicode_isspace(this); }
    bool isupper() { return unicode_isupper(this); }
    bool islower() { return unicode_islower(this); }
}

public extend IString with IUnicode;

// ---------------------------------------------------------------- helpers

public String unicode_lower<T: IString>(T s) {
    isize n = s.length();
    isize total = 0;
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        int cp = utf8_cp(bits);
        int mapped = lower_of(cp);
        if mapped >= 0 {
            cp = mapped;
        }
        total += utf8_cp_len(cp);
        off += utf8_len(bits);
    }
    byte[] buf = new byte[total + 1];
    isize dst = 0;
    off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        int cp = utf8_cp(bits);
        int mapped = lower_of(cp);
        if mapped >= 0 {
            cp = mapped;
        }
        dst += utf8_put(buf, dst, cp);
        off += utf8_len(bits);
    }
    buf[total] = 0;
    return String(buf, total);
}

public String unicode_upper<T: IString>(T s) {
    isize n = s.length();
    isize total = 0;
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        int cp = utf8_cp(bits);
        int mapped = upper_of(cp);
        if mapped >= 0 {
            cp = mapped;
        }
        total += utf8_cp_len(cp);
        off += utf8_len(bits);
    }
    byte[] buf = new byte[total + 1];
    isize dst = 0;
    off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        int cp = utf8_cp(bits);
        int mapped = upper_of(cp);
        if mapped >= 0 {
            cp = mapped;
        }
        dst += utf8_put(buf, dst, cp);
        off += utf8_len(bits);
    }
    buf[total] = 0;
    return String(buf, total);
}

public bool unicode_isalpha<T: IString>(T s) {
    isize n = s.length();
    if n == 0 {
        return false;
    }
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        if not is_alpha_cp(utf8_cp(bits)) {
            return false;
        }
        off += utf8_len(bits);
    }
    return true;
}

public bool unicode_isdigit<T: IString>(T s) {
    isize n = s.length();
    if n == 0 {
        return false;
    }
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        if not is_digit_cp(utf8_cp(bits)) {
            return false;
        }
        off += utf8_len(bits);
    }
    return true;
}

public bool unicode_isalnum<T: IString>(T s) {
    isize n = s.length();
    if n == 0 {
        return false;
    }
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        int cp = utf8_cp(bits);
        if not is_alpha_cp(cp) and not is_digit_cp(cp) {
            return false;
        }
        off += utf8_len(bits);
    }
    return true;
}

public bool unicode_isspace<T: IString>(T s) {
    isize n = s.length();
    if n == 0 {
        return false;
    }
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        if not is_space_cp(utf8_cp(bits)) {
            return false;
        }
        off += utf8_len(bits);
    }
    return true;
}

// Python's rule: at least one cased character and no lowercase character.
public bool unicode_isupper<T: IString>(T s) {
    bool has_cased = false;
    isize n = s.length();
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        int cp = utf8_cp(bits);
        if is_lower_cp(cp) {
            return false;
        }
        if is_upper_cp(cp) {
            has_cased = true;
        }
        off += utf8_len(bits);
    }
    return has_cased;
}

// Python's rule: at least one cased character and no uppercase character.
public bool unicode_islower<T: IString>(T s) {
    bool has_cased = false;
    isize n = s.length();
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        int cp = utf8_cp(bits);
        if is_upper_cp(cp) {
            return false;
        }
        if is_lower_cp(cp) {
            has_cased = true;
        }
        off += utf8_len(bits);
    }
    return has_cased;
}

public String unicode_strip<T: IString>(T s) {
    isize n = s.length();
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        if not is_space_cp(utf8_cp(bits)) {
            break;
        }
        off += utf8_len(bits);
    }
    isize start = off;
    isize end = start;
    while off < n {
        isize bits = utf8_decode(s, off);
        off += utf8_len(bits);
        if not is_space_cp(utf8_cp(bits)) {
            end = off;
        }
    }
    return unicode_copy_range(s, start, end);
}

public String unicode_lstrip<T: IString>(T s) {
    isize n = s.length();
    isize off = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        if not is_space_cp(utf8_cp(bits)) {
            break;
        }
        off += utf8_len(bits);
    }
    return unicode_copy_range(s, off, n);
}

public String unicode_rstrip<T: IString>(T s) {
    isize n = s.length();
    isize off = 0;
    isize end = 0;
    while off < n {
        isize bits = utf8_decode(s, off);
        off += utf8_len(bits);
        if not is_space_cp(utf8_cp(bits)) {
            end = off;
        }
    }
    return unicode_copy_range(s, 0, end);
}

public String unicode_copy_range<T: IString>(T s, isize from, isize to) {
    isize n = s.length();
    // The offsets are byte offsets into `s`; a range outside it used to copy silent NUL padding
    // (at() returns 0 past the end), producing a string whose length and content disagreed (#234).
    if from < 0 or to < from or to > n {
        throw new BoundsError(to, n, 0);
    }
    if to == from {
        String e = {};
        return e;
    }
    isize count = to - from;
    byte[] buf = new byte[count + 1];
    isize i = 0;
    while i < count {
        buf[i] = s.at(from + i);
        i += 1;
    }
    buf[count] = 0;
    return String(buf, count);
}
