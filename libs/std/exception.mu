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
