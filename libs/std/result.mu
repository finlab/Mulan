// std.result — `Result<T, E>`, an operation's outcome: a value or the reason it failed.
//
// A `variant` (SPEC 10.1.1) like `std.option.Option<T>`; the difference is the failure side, which
// carries data (`E`) instead of being a bare `None`. Use `Result` when the caller must tell *why*
// an operation failed, and `Option` when absence is the whole outcome.
//
// A `Result` is a value: it copies by value, and a reference payload is retained/released with it.
// Presence is the language's test: `r is Ok` / `r is Err`; there is no `is_ok()` helper.
//
// Bridging to `std.option.Option` is a `match` (`Ok(v): Some(v); Err(e): None;`), not a helper: `ok` /
// `err` are exactly the short names a user variable takes, and a helper that shadows one is worse
// than three explicit lines.

public unit std.result;

use std.option;

// Thrown by `expect_ok` / `expect_err`.
public class ResultError : Exception {
    ResultError(string m) : Exception(m) { }
}

public variant Result<T, E> {
    Ok T;
    Err E;
}

// The success value, or `fallback` when the result failed.
public T unwrap_or<T, E>(Result<T, E> r, T fallback) {
    return match r {
        Ok(v): v;
        Err(e): fallback;
    };
}

// The success value; throws `ResultError(msg)` on failure. Use it where a failure is a programming
// error, not an expected outcome.
public T expect_ok<T, E>(Result<T, E> r, string msg) {
    match r {
        Ok(v): { return v; }
        Err(e): { throw new ResultError(msg); }
    }
}

// The failure value; throws `ResultError(msg)` on success.
public E expect_err<T, E>(Result<T, E> r, string msg) {
    match r {
        Ok(v): { throw new ResultError(msg); }
        Err(e): { return e; }
    }
}

// Transform the success value, keeping the failure as it is.
public Result<U, E> map_ok<T, U, E>(Result<T, E> r, (T) => U f) {
    return match r {
        Ok(v): Result<U, E>.Ok(f(v));
        Err(e): Result<U, E>.Err(e);
    };
}

// Transform the failure value, keeping a success as it is.
public Result<T, F> map_err<T, E, F>(Result<T, E> r, (E) => F f) {
    return match r {
        Ok(v): Result<T, F>.Ok(v);
        Err(e): Result<T, F>.Err(f(e));
    };
}
