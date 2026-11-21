// mu standard library: exceptions
//
// The compiler itself only requires that every throwable/catchable class is a
// subclass of this Exception.  The message field is the only built-in state;
// user error classes add their own fields and constructors.

public unit std.exception;

public class Exception {
public:
    string message;

    Exception() {
    }

    Exception(string msg) {
        message = msg;
    }
}


// Small decimal formatter for the cold-path messages. `std.fmt` is still planned; keeping this
// private avoids pulling a formatting unit into an implicitly loaded L3 unit.
private String int_text(isize v) {
    bool neg = v < 0;
    long x = v as long;
    if neg { x = -x; }
    byte[20] tmp;
    int n = 0;
    if x == 0 { tmp[0] = 48 as byte; n = 1; }
    while x > 0 {
        tmp[n] = (48 + (x % 10)) as byte;
        x = x / 10;
        n += 1;
    }
    isize total = n as isize;
    if neg { total += 1; }
    byte[] out = new byte[total];
    isize k = 0;
    if neg { out[0] = 45 as byte; k = 1; }
    int i = n - 1;
    while i >= 0 {
        out[k] = tmp[i];
        k += 1;
        i -= 1;
    }
    return String(out, total);
}

// Thrown by the bounds check that the compiler emits for a managed sequence subscript
// (`T[]`); `--bounds=off` removes the check.
public class BoundsError : Exception {
public:
    isize index;
    isize length;
    isize axis;

    BoundsError(isize idx, isize total, isize ax) {
        index = idx;
        length = total;
        axis = ax;
    }
}

// Cold path of the bounds check: the compiler calls this with the offending index.
public void bounds_fail(isize index, isize length, isize axis) {
    BoundsError e = new BoundsError(index, length, axis);
    e.message = "index " + int_text(index) + " out of bounds for length " + int_text(length) +
                " (axis " + int_text(axis) + ")";
    throw e;
}


// Thrown by the numeric-edge checks the compiler emits when `--bounds` is on (`--bounds=off` drops the
// checks and the operation becomes undefined behavior, SPEC 8.2):
//   kind 0 — a shift whose count is negative or >= the bit width of the left operand;
//   kind 1 — a float->int `as` whose value is NaN or outside the target type's range.
public class NumericEdgeError : Exception {
public:
    int kind;
    double value;
    long width;

    NumericEdgeError(int k, double v, long w) {
        kind = k;
        value = v;
        width = w;
    }
}

// Cold paths of those checks (the compiler calls them; they always throw).
public void shift_fail(long count, int width) {
    NumericEdgeError e = new NumericEdgeError(0, count as double, width as long);
    e.message = "shift count " + int_text(count as isize) + " out of range for a " +
                int_text(width as isize) + "-bit operand";
    throw e;
}

public void cast_fail(double value, int width) {
    NumericEdgeError e = new NumericEdgeError(1, value, width as long);
    e.message = "numeric cast out of range for a " + int_text(width as isize) + "-bit target";
    throw e;
}

// Thrown by the null-reference check the compiler emits before a class handle or an interface value
// is dereferenced (SPEC 8.2): a method call or a field access on `null` has no object to work on.
// `--null-check=off` drops the check and the access becomes undefined behavior.
public class NullReferenceError : Exception {
public:
    NullReferenceError(string what) {
        message = what;
    }
}

// Cold path of the null check: the compiler calls this with a description of the access that failed
// (`method call on `Dog``, `field access on `Dog``).
public void null_fail(const char* what) {
    throw new NullReferenceError(what);
}

// Thrown by the arithmetic checks the compiler emits when `--bounds` is on: integer division or
// remainder by zero (`kind` 0) and `MIN / -1` / `MIN % -1` (`kind` 1). The CPU traps on all of them,
// so with `--bounds=off` they are undefined behavior instead (SPEC 8.2).
public class ArithmeticError : Exception {
public:
    long value;   // the dividend
    int kind;

    ArithmeticError(long v, int k) {
        value = v;
        kind = k;
    }
}

// Cold paths of those checks (the compiler calls them; they always throw).
public void div_fail(long value, int kind) {
    ArithmeticError e = new ArithmeticError(value, kind);
    if kind == 0 { e.message = "integer division by zero"; }
    else { e.message = "integer division overflow (minimum value divided by -1)"; }
    throw e;
}
