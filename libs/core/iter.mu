// core.iter — compiler-facing contracts for `for-in` (SPEC 6.4.1 / 6.4.2)
//
// The compiler checks these contracts **structurally** (no vtable, no boxing): a type satisfies a
// contract by providing the methods with exactly these signatures. Declaring `: IIterator<T>` on a
// cursor is optional, but it documents the contract at the definition site and lets the compiler
// check it for you.
//
// This unit is loaded implicitly by the compiler, so a user type may also `use core.iter;` and
// declare the interface explicitly.

public unit core.iter;

// ---------------------------------------------------------------- cursor form (6.4.2)
//
// The object returned by `iterator()`. It must be a **struct returned by value**: the state lives on
// the stack, the calls are static and inlinable, and nothing is allocated.
//
// Contract:
// - `next()` advances and answers "is there an element". It may block (that is how a stream such as
//   an accept loop works); inside a task a blocking `next()` is a park point and a cancellation point.
// - `current()` is called only after `next()` returned true and returns the element **by value**.
//   For a class/interface element the loop variable owns a reference (retain on binding, release at
//   the end of the iteration).
// - A cursor is consumed by **one** loop at a time (calling `next()` mutates it); it is not required
//   to be thread-safe.
// - Errors are reported by throwing, not by a special return value.
public interface IIterator<T> {
    bool next();
    T current();
}

// ---------------------------------------------------------------- index form (6.4.1)
//
// The container side of the index form: `usize length()` plus a subscript. An interface cannot
// declare `operator[]`, so the subscript half stays structural: either a **member**
// `T operator[](usize i)` (preferred: the receiver is borrowed) or a free function
// `T operator[](C, usize)`. `for ref` additionally requires the subscript to return `ref T`.
//
// A type that also provides `iterator()` supports the cursor form as well; the index form wins
// (SPEC 6.4.3).
public interface IIndexable<T> {
    usize length();
}
