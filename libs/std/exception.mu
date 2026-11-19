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

// Thrown by the bounds check that the compiler emits for a managed sequence subscript
// (`T[]` / `buffer<T>`); `--bounds=off` removes the check.
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
    throw new BoundsError(index, length, axis);
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
    throw new NumericEdgeError(0, count as double, width as long);
}

public void cast_fail(double value, int width) {
    throw new NumericEdgeError(1, value, width as long);
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
