// std.option — `Option<T>`, a value that may be absent.
//
// A `variant` (SPEC 10.1.1), not a library struct: the set of cases is exhaustive, the payload's
// lifetime is the type system's job (a reference payload is retained and released with the value),
// and one definition serves every `T`. `None` carries no payload, so no sentinel is ever needed.
//
// Dispatch with `match`; the helpers below cover the common cases. Every helper takes the option by
// value, so a reference payload is copied (one retain/release pair), not borrowed: pass values, not
// containers meant to be observed in place.

public unit std.option;

// Thrown by `expect` when the option is `None`.
public class OptionError : Exception {
    OptionError(string m) : Exception(m) { }
}

public variant Option<T> {
    Some T;
    None;
}

public bool is_some<T>(Option<T> o) {
    return match o {
        Some(v): true;
        None: false;
    };
}

public bool is_none<T>(Option<T> o) {
    return match o {
        Some(v): false;
        None: true;
    };
}

// The payload, or `fallback` when the option is `None`. Both branches have type `T`, so this is the
// only helper that works for every payload type, including one without a default value.
public T unwrap_or<T>(Option<T> o, T fallback) {
    return match o {
        Some(v): v;
        None: fallback;
    };
}

// The payload; throws `OptionError(msg)` when the option is `None`. Use it where a missing value is
// a programming error, not an expected outcome.
public T expect<T>(Option<T> o, string msg) {
    match o {
        Some(v): { return v; }
        None: { throw new OptionError(msg); }
    }
}

// Transform the payload, leaving `None` alone.
public Option<U> map<T, U>(Option<T> o, (T) => U f) {
    return match o {
        Some(v): Option<U>.Some(f(v));
        None: Option<U>.None;
    };
}

// Chain an operation that may itself find nothing: `f` decides the result, so `Some(v)` becomes
// `f(v)` (which is `None` when the chain ends there) and `None` stays `None`.
public Option<U> and_then<T, U>(Option<T> o, (T) => Option<U> f) {
    return match o {
        Some(v): f(v);
        None: Option<U>.None;
    };
}

