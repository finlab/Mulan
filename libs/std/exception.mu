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
