// std.mem — element-wise operations on a managed sequence.
//
// These work on the **elements** of `T[]`, not on bytes: an assignment goes through the language's
// element rules, so a reference element is retained/released exactly as `dst[i] = src[i]` would do.
// A raw byte copy is `T[]`'s `.ptr()` handed to `cstd.string.memcpy` and is only valid for a trivial
// element type (SPEC 3.3).
//
// Not provided:
//   - `zero(dst)`: a type parameter has no zero value (`T()` is not defined for every `T`), and
//     `new T[n]` already produces a zeroed array, so filling with zero is `fill(dst, 0)` for a scalar
//     element type;
//   - offset copies (`copy(dst, dst_at, src, src_at, n)`): start from the forms below and add one when
//     a container needs it.

public unit std.mem;

use std.interfaces;

// Thrown by `align_up`.
public class MemError : Exception {
    MemError(string m) : Exception(m) { }
}

// Copy every element of `src` into `dst`, starting at index 0. `dst` must be at least as long; the
// loop's own bounds check reports the first index that is out of range, so a short destination throws
// `BoundsError` instead of writing past its end.
//
// The two ranges must not overlap (`dst` and `src` may be the same array only when they start at the
// same element). Overlapping ranges are copied front to back, which is not memmove.
public void copy<T>(T[] dst, T[] src) {
    isize n = src.length();
    for i in 0..n {
        dst[i] = src[i];
    }
}

// Assign `value` to every element.
public void fill<T>(T[] dst, T value) {
    for i in 0..dst.length() {
        dst[i] = value;
    }
}

// Lexicographic comparison: -1 when `a` sorts before `b`, 1 when after, 0 when the sequences are
// equal. A prefix sorts before the longer sequence (`{1,2}` < `{1,2,3}`).
public isize compare<T: IOrder>(T[] a, T[] b) {
    isize n = a.length();
    if b.length() < n { n = b.length(); }
    for i in 0..n {
        if a[i] < b[i] { return -1; }
        if b[i] < a[i] { return 1; }
    }
    if a.length() < b.length() { return -1; }
    if b.length() < a.length() { return 1; }
    return 0;
}

// Round `n` up to the next multiple of `alignment`. `alignment` must be a positive power of two (the
// same precondition `align(N)` has); anything else throws `MemError` rather than returning a value
// that silently misaligns.
public isize align_up(isize n, isize alignment) {
    if alignment <= 0 or (alignment & (alignment - 1)) != 0 {
        throw new MemError("alignment must be a positive power of two");
    }
    return (n + alignment - 1) & ~(alignment - 1);
}
