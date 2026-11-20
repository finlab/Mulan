// mu standard library: string
//
// A pure library type: the compiler knows nothing about its layout. A string literal is a
// NUL-terminated UTF-8 `const char*`, and `string s = cstr;` goes through
// `implicit string(const char*)` (a copy, one allocation).
//
// string is a value-type struct: assignment and argument passing copy the three
// fields (p/n/owner); owner is an RC byte array that keeps the backing buffer alive.
//
// The model is UTF-8 bytes: `length()`, `s[i]`, `sub` and `find` are byte-based. Code point
// operations are explicit (`chars`, `char_count`, `char_at`, `uchars`); Unicode tables live
// in `std.unicode`, which extends `IString` and must be loaded explicitly.

public unit std.string;

use std.exception;
use std.utf8;

// `IString` (the byte-sequence view used by the UTF-8 primitives and the std.unicode extension)
// lives in std.utf8.

// ---------------------------------------------------------------- ASCII predicates

private bool ascii_space(byte c) {
    return c == ' ' or c == '\t' or c == '\n' or c == '\r' or c == '\v' or c == '\f';
}

private bool ascii_digit(byte c) {
    return c >= '0' and c <= '9';
}

private bool ascii_upper(byte c) {
    return c >= 'A' and c <= 'Z';
}

private bool ascii_lower(byte c) {
    return c >= 'a' and c <= 'z';
}

private bool ascii_alpha(byte c) {
    return ascii_upper(c) or ascii_lower(c);
}

// ---------------------------------------------------------------- the String type

public struct String : IString {
private:
    byte* p;
    isize n;
    byte[] owner;

public:
    // An empty string (the zero value). A parameterless constructor is what `string s;` and
    // `new string[n]` construct elements with (SPEC 3.3, agents/value-semantics.md §7).
    String() {}

    // Construct from a NUL-terminated C string (copies it, does not hold c).
    // A null `c` is the empty string: C APIs such as `getenv` return null.
    implicit String(const char* c) {
        if c == null {
            return;
        }
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

    // Copy a byte array and append NUL. Validity is not checked here (`is_valid_utf8` does that).
    String(byte[] bytes) {
        isize count = bytes.length();
        byte[] buf = new byte[count + 1];
        isize i = 0;
        while i < count {
            buf[i] = bytes[i];
            i += 1;
        }
        buf[count] = 0;
        p = &buf[0];
        n = count;
        owner = buf;
    }

    // Take ownership of `buf` without copying. The caller guarantees `buf[len] == 0` and keeps one
    // reference while this String lives. Low-level: used by builders and the Unicode extension.
    String(byte[] buf, isize size) {
        if size <= 0 {
            return;
        }
        p = &buf[0];
        n = size;
        owner = buf;
    }

    // Encode code points as UTF-8. An invalid code point (negative, beyond U+10FFFF, or a surrogate)
    // becomes U+FFFD.
    String(int[] cps) {
        isize total = 0;
        isize i = 0;
        while i < cps.length() {
            total += utf8_cp_len(cps[i]);
            i += 1;
        }
        byte[] buf = new byte[total + 1];
        isize dst = 0;
        i = 0;
        while i < cps.length() {
            dst += utf8_put(buf, dst, cps[i]);
            i += 1;
        }
        buf[total] = 0;
        p = &buf[0];
        n = total;
        owner = buf;
    }

    // ---------------------------------------------------------------- basic info

    isize length() const {
        return n;
    }

    bool is_empty() const {
        return n == 0;
    }

    // Out of range returns 0 (the clamping accessor). `operator[]` below is the checked one.
    byte at(isize i) const {
        if i < 0 or i >= n {
            return 0;
        }
        return p[i];
    }

    // NUL-terminated view of the logical value. The zero value (p == null) is the empty string;
    // every representation ends its buffer with a 0 at n.
    const char* cstr() const {
        if p == null {
            return "";
        }
        return p as const char*;
    }

    // `s[i]`: byte access, bounds-checked. The check is written in the library rather than emitted
    // by the compiler, so `--bounds=off` does not remove it.
    byte operator[](isize i) const {
        if i < 0 or i >= n {
            throw new BoundsError(i, n, 0);
        }
        return p[i];
    }

    // ---------------------------------------------------------------- UTF-8 structure

    // Strict validation: overlong forms, surrogates and values above U+10FFFF are rejected.
    bool is_valid_utf8() const {
        isize off = 0;
        while off < n {
            int b0 = p[off] as int;
            if b0 < 0x80 {
                off += 1;
            } else if (b0 & 0xE0) == 0xC0 {
                if off + 1 >= n {
                    return false;
                }
                int b1 = p[off + 1] as int;
                if (b1 & 0xC0) != 0x80 {
                    return false;
                }
                int cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
                if cp < 0x80 {
                    return false;
                }
                off += 2;
            } else if (b0 & 0xF0) == 0xE0 {
                if off + 2 >= n {
                    return false;
                }
                int b1 = p[off + 1] as int;
                int b2 = p[off + 2] as int;
                if (b1 & 0xC0) != 0x80 or (b2 & 0xC0) != 0x80 {
                    return false;
                }
                int cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
                if cp < 0x800 or (cp >= 0xD800 and cp <= 0xDFFF) {
                    return false;
                }
                off += 3;
            } else if (b0 & 0xF8) == 0xF0 {
                if off + 3 >= n {
                    return false;
                }
                int b1 = p[off + 1] as int;
                int b2 = p[off + 2] as int;
                int b3 = p[off + 3] as int;
                if (b1 & 0xC0) != 0x80 or (b2 & 0xC0) != 0x80 or (b3 & 0xC0) != 0x80 {
                    return false;
                }
                int cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                if cp < 0x10000 or cp > 0x10FFFF {
                    return false;
                }
                off += 4;
            } else {
                return false;
            }
        }
        return true;
    }

    // Number of code points; each invalid byte counts as one U+FFFD (the lossy policy).
    isize char_count() const {
        isize count = 0;
        isize off = 0;
        while off < n {
            isize bits = utf8_decode(this, off);
            off += utf8_len(bits);
            count += 1;
        }
        return count;
    }

    // The code point at a code point index (O(n)). Throws BoundsError when out of range.
    int char_at(isize index) const {
        if index < 0 {
            throw new BoundsError(index, char_count(), 0);
        }
        isize off = 0;
        isize k = 0;
        while off < n {
            isize bits = utf8_decode(this, off);
            if k == index {
                return utf8_cp(bits);
            }
            off += utf8_len(bits);
            k += 1;
        }
        throw new BoundsError(index, k, 0);
    }

    bool is_char_boundary(isize off) const {
        if off < 0 or off > n {
            return false;
        }
        if off == 0 or off == n {
            return true;
        }
        return ((p[off] as int) & 0xC0) != 0x80;
    }

    // Byte offset of a code point index (O(n)). Throws BoundsError when out of range.
    isize byte_offset(isize char_index) const {
        if char_index < 0 {
            throw new BoundsError(char_index, char_count(), 0);
        }
        isize off = 0;
        isize k = 0;
        while off < n {
            if k == char_index {
                return off;
            }
            isize bits = utf8_decode(this, off);
            off += utf8_len(bits);
            k += 1;
        }
        if k == char_index {
            return n;
        }
        throw new BoundsError(char_index, k, 0);
    }

    // Code point index of a byte offset; the offset must be a boundary.
    isize char_index(isize off) const {
        if not is_char_boundary(off) {
            throw new BoundsError(off, n, 0);
        }
        isize k = 0;
        isize at_off = 0;
        while at_off < off {
            isize bits = utf8_decode(this, at_off);
            at_off += utf8_len(bits);
            k += 1;
        }
        return k;
    }

    // All code points, lossy on invalid input.
    int[] uchars() const {
        isize count = char_count();
        int[] out = new int[count];
        isize off = 0;
        isize k = 0;
        while off < n {
            isize bits = utf8_decode(this, off);
            out[k] = utf8_cp(bits);
            k += 1;
            off += utf8_len(bits);
        }
        return out;
    }

    CharCursor chars() const {
        return CharCursor(this);
    }

    ByteCursor bytes() const {
        return ByteCursor(this);
    }

    // ---------------------------------------------------------------- comparison and concatenation

    bool operator==(String o) const {
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

    // Compare with a NUL-terminated C string (literals): `s == "abc"`.
    bool operator==(const char* c) const {
        isize i = 0;
        while i < n {
            if c[i] == 0 or p[i] != (c[i] as byte) {
                return false;
            }
            i += 1;
        }
        return c[i] == 0;
    }

    String operator+(const char* c) const {
        return concat(String(c));
    }

    String operator+(String o) const {
        return concat(o);
    }

    String concat(String other) const {
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
        return String(buf, total);
    }

    // ---------------------------------------------------------------- slicing

    // A byte slice; it copies into its own NUL-terminated buffer. Out-of-range arguments are
    // clamped. The offsets are NOT required to be character boundaries: `sub` is a byte operation,
    // so slicing in the middle of a code point produces an invalid-UTF-8 String (use `char_slice`
    // for code point semantics).
    String sub(isize start, isize count) const {
        if start < 0 {
            start = 0;
        }
        if count < 0 {
            count = 0;
        }
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
        return String(buf, count);
    }

    // Code point slice: `start` and `count` are code point indices. O(n), and the result is always
    // on code point boundaries (so it stays valid when the source is valid).
    String char_slice(isize start, isize count) const {
        isize total = char_count();
        if start < 0 {
            start = 0;
        }
        if count < 0 {
            count = 0;
        }
        if start > total {
            start = total;
        }
        if count > total - start {
            count = total - start;
        }
        isize from = byte_offset(start);
        isize to = byte_offset(start + count);
        return sub(from, to - from);
    }

    String repeat(isize times) const {
        if times <= 0 or n == 0 {
            String e = {};
            return e;
        }
        isize total = n * times;
        byte[] buf = new byte[total + 1];
        isize dst = 0;
        isize t = 0;
        while t < times {
            isize i = 0;
            while i < n {
                buf[dst] = p[i];
                dst += 1;
                i += 1;
            }
            t += 1;
        }
        buf[total] = 0;
        return String(buf, total);
    }

    String remove_prefix(String prefix) const {
        if startswith(prefix) {
            return sub(prefix.n, n - prefix.n);
        }
        return sub(0, n);
    }

    String remove_suffix(String suffix) const {
        if endswith(suffix) {
            return sub(0, n - suffix.n);
        }
        return sub(0, n);
    }

    // ---------------------------------------------------------------- search

    // Byte offset of the first occurrence, or -1. Empty needle returns 0 (Python `find`).
    isize find(String needle) const {
        return find_from(needle, 0);
    }

    // Byte offset of the last occurrence, or -1. Empty needle returns n.
    isize rfind(String needle) const {
        if needle.n == 0 {
            return n;
        }
        if needle.n > n {
            return -1;
        }
        isize i = n - needle.n;
        while i >= 0 {
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
            i -= 1;
        }
        return -1;
    }

    // Non-overlapping occurrences. Empty needle returns 0.
    isize count(String needle) const {
        if needle.n == 0 {
            return 0;
        }
        isize c = 0;
        isize start = 0;
        while true {
            isize pos = find_from(needle, start);
            if pos < 0 {
                break;
            }
            c += 1;
            start = pos + needle.n;
        }
        return c;
    }

    bool contains(String needle) const {
        return find(needle) >= 0;
    }

    bool startswith(String prefix) const {
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

    bool endswith(String suffix) const {
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

    // ---------------------------------------------------------------- replace / split / join

    // Replace every non-overlapping occurrence. An empty `old` returns a copy (Python inserts at
    // every position; that spelling is not provided in the byte API).
    String replace(String old, String neu) const {
        if old.n == 0 {
            return sub(0, n);
        }
        isize cnt = 0;
        isize start = 0;
        while true {
            isize pos = find_from(old, start);
            if pos < 0 {
                break;
            }
            cnt += 1;
            start = pos + old.n;
        }
        if cnt == 0 {
            return sub(0, n);
        }
        isize total = n - cnt * old.n + cnt * neu.n;
        byte[] buf = new byte[total + 1];
        isize src = 0;
        isize dst = 0;
        start = 0;
        while true {
            isize pos = find_from(old, start);
            if pos < 0 {
                break;
            }
            while src < pos {
                buf[dst] = p[src];
                dst += 1;
                src += 1;
            }
            isize k = 0;
            while k < neu.n {
                buf[dst] = neu.p[k];
                dst += 1;
                k += 1;
            }
            src = pos + old.n;
            start = src;
        }
        while src < n {
            buf[dst] = p[src];
            dst += 1;
            src += 1;
        }
        buf[total] = 0;
        return String(buf, total);
    }

    // Split on an exact separator; trailing empty pieces are kept (Python `split(sep)`). An empty
    // separator is an error (Python raises ValueError too).
    String[] split(String sep) const {
        if sep.n == 0 {
            throw new Exception("split: empty separator");
        }
        isize cnt = 0;
        isize start = 0;
        while true {
            isize pos = find_from(sep, start);
            if pos < 0 {
                break;
            }
            cnt += 1;
            start = pos + sep.n;
        }
        String[] parts = new String[cnt + 1];
        start = 0;
        isize k = 0;
        while true {
            isize pos = find_from(sep, start);
            if pos < 0 {
                break;
            }
            parts[k] = sub(start, pos - start);
            k += 1;
            start = pos + sep.n;
        }
        parts[k] = sub(start, n - start);
        return parts;
    }

    // Whitespace split with ASCII whitespace only (Python's no-argument `split` is Unicode-aware;
    // it lives in std.unicode). Runs of whitespace are one separator; no empty pieces.
    String[] split_ascii_whitespace() const {
        isize cnt = 0;
        isize i = 0;
        while i < n {
            while i < n and ascii_space(p[i]) {
                i += 1;
            }
            if i >= n {
                break;
            }
            cnt += 1;
            while i < n and not ascii_space(p[i]) {
                i += 1;
            }
        }
        String[] parts = new String[cnt];
        isize k = 0;
        i = 0;
        while i < n {
            while i < n and ascii_space(p[i]) {
                i += 1;
            }
            if i >= n {
                break;
            }
            isize start = i;
            while i < n and not ascii_space(p[i]) {
                i += 1;
            }
            parts[k] = sub(start, i - start);
            k += 1;
        }
        return parts;
    }

    // Split on ASCII line endings (\n, \r, \r\n). A trailing line break does not add an empty line,
    // matching Python `splitlines` (Unicode line separators live in std.unicode).
    String[] splitlines_ascii() const {
        isize cnt = 0;
        isize i = 0;
        isize line_start = 0;
        while i < n {
            byte c = p[i];
            if c == '\n' or c == '\r' {
                cnt += 1;
                if c == '\r' and i + 1 < n and p[i + 1] == '\n' {
                    i += 2;
                } else {
                    i += 1;
                }
                line_start = i;
            } else {
                i += 1;
            }
        }
        if line_start < n {
            cnt += 1;
        }
        String[] parts = new String[cnt];
        isize k = 0;
        i = 0;
        line_start = 0;
        while i < n {
            byte c = p[i];
            if c == '\n' or c == '\r' {
                parts[k] = sub(line_start, i - line_start);
                k += 1;
                if c == '\r' and i + 1 < n and p[i + 1] == '\n' {
                    i += 2;
                } else {
                    i += 1;
                }
                line_start = i;
            } else {
                i += 1;
            }
        }
        if line_start < n {
            parts[k] = sub(line_start, n - line_start);
        }
        return parts;
    }

    // Join parts with this String as the separator (Python `sep.join(parts)`).
    String join(String[] parts) const {
        isize count = parts.length();
        isize total = 0;
        isize i = 0;
        while i < count {
            total += parts[i].n;
            i += 1;
        }
        if count > 1 {
            total += n * (count - 1);
        }
        byte[] buf = new byte[total + 1];
        isize dst = 0;
        i = 0;
        while i < count {
            if i > 0 {
                isize k = 0;
                while k < n {
                    buf[dst] = p[k];
                    dst += 1;
                    k += 1;
                }
            }
            String part = parts[i];
            isize k = 0;
            while k < part.n {
                buf[dst] = part.p[k];
                dst += 1;
                k += 1;
            }
            i += 1;
        }
        buf[total] = 0;
        return String(buf, total);
    }

    // ---------------------------------------------------------------- ASCII transformations

    String strip_ascii() const {
        isize start = 0;
        while start < n and ascii_space(p[start]) {
            start += 1;
        }
        isize stop = n;
        while stop > start and ascii_space(p[stop - 1]) {
            stop -= 1;
        }
        return sub(start, stop - start);
    }

    String lstrip_ascii() const {
        isize start = 0;
        while start < n and ascii_space(p[start]) {
            start += 1;
        }
        return sub(start, n - start);
    }

    String rstrip_ascii() const {
        isize stop = n;
        while stop > 0 and ascii_space(p[stop - 1]) {
            stop -= 1;
        }
        return sub(0, stop);
    }

    String lower_ascii() const {
        byte[] buf = new byte[n + 1];
        isize i = 0;
        while i < n {
            byte c = p[i];
            if ascii_upper(c) {
                buf[i] = ((c as int) + 32) as byte;
            } else {
                buf[i] = c;
            }
            i += 1;
        }
        buf[n] = 0;
        return String(buf, n);
    }

    String upper_ascii() const {
        byte[] buf = new byte[n + 1];
        isize i = 0;
        while i < n {
            byte c = p[i];
            if ascii_lower(c) {
                buf[i] = ((c as int) - 32) as byte;
            } else {
                buf[i] = c;
            }
            i += 1;
        }
        buf[n] = 0;
        return String(buf, n);
    }

    bool is_ascii() const {
        isize i = 0;
        while i < n {
            if (p[i] as int) >= 0x80 {
                return false;
            }
            i += 1;
        }
        return true;
    }

    // Python's `isalpha` etc. are false for the empty string; the `_ascii` versions keep that rule
    // and use the ASCII definitions only.
    bool is_alpha_ascii() const {
        if n == 0 {
            return false;
        }
        isize i = 0;
        while i < n {
            if not ascii_alpha(p[i]) {
                return false;
            }
            i += 1;
        }
        return true;
    }

    bool is_digit_ascii() const {
        if n == 0 {
            return false;
        }
        isize i = 0;
        while i < n {
            if not ascii_digit(p[i]) {
                return false;
            }
            i += 1;
        }
        return true;
    }

    bool is_alnum_ascii() const {
        if n == 0 {
            return false;
        }
        isize i = 0;
        while i < n {
            if not ascii_alpha(p[i]) and not ascii_digit(p[i]) {
                return false;
            }
            i += 1;
        }
        return true;
    }

    bool is_space_ascii() const {
        if n == 0 {
            return false;
        }
        isize i = 0;
        while i < n {
            if not ascii_space(p[i]) {
                return false;
            }
            i += 1;
        }
        return true;
    }

    bool is_upper_ascii() const {
        bool has_cased = false;
        isize i = 0;
        while i < n {
            byte c = p[i];
            if ascii_lower(c) {
                return false;
            }
            if ascii_upper(c) {
                has_cased = true;
            }
            i += 1;
        }
        return has_cased;
    }

    bool is_lower_ascii() const {
        bool has_cased = false;
        isize i = 0;
        while i < n {
            byte c = p[i];
            if ascii_upper(c) {
                return false;
            }
            if ascii_lower(c) {
                has_cased = true;
            }
            i += 1;
        }
        return has_cased;
    }

private:
    isize find_from(String needle, isize start) const {
        if needle.n == 0 {
            return start;
        }
        if start < 0 {
            start = 0;
        }
        if needle.n > n or start > n - needle.n {
            return -1;
        }
        isize i = start;
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
}

// ---------------------------------------------------------------- cursors

// Code point cursor (lossy: an invalid byte yields U+FFFD and advances one byte).
public struct CharCursor {
private:
    String s;
    isize i;
    int cp;
    isize adv;

public:
    CharCursor(String str) {
        s = str;
        i = 0;
        cp = 0;
        adv = 1;
    }

    bool next() {
        if i >= s.length() {
            return false;
        }
        isize bits = utf8_decode(s, i);
        cp = utf8_cp(bits);
        adv = utf8_len(bits);
        i += adv;
        return true;
    }

    int current() const {
        return cp;
    }

    CharCursor iterator() {
        return this;
    }
}

public struct ByteCursor {
private:
    String s;
    isize i;

public:
    ByteCursor(String str) {
        s = str;
        i = 0;
    }

    bool next() {
        if i >= s.length() {
            return false;
        }
        i += 1;
        return true;
    }

    byte current() const {
        return s.at(i - 1);
    }

    ByteCursor iterator() {
        return this;
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
