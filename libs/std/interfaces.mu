// std.interfaces — the compiler's structural contracts and the built-in type capabilities
// (SPEC 3.15.9 / 6.4.1 / 6.4.2)
//
// A type satisfies one of these **structurally** (no declaration needed: providing the members with
// exactly these signatures is enough), and may also *declare* the interface (`struct Counter :
// IIterator<int>`, `class Cursor : IIterator<int>`) to have the definition site checked.
//
// `IIterator<T>` / `IEnumerable<T>` double as value types: since 0.0.108 an instantiated generic
// interface is an ordinary interface type, so a class cursor can be returned as `IIterator<T>` and
// `for-in` dispatches through the itable. A struct is never boxed, so a struct container returns a
// class cursor (or keeps the `for-in` path structural and returns a struct cursor, which needs no
// interface at all).
//
// This unit is loaded implicitly by the compiler (like `std.string` / `std.exception`), so a user
// unit may also `use std.interfaces;` and write the names explicitly.

public unit std.interfaces;

// ---------------------------------------------------------------- for-in contracts
//
// Cursor form (SPEC 6.4.2): the object returned by `iterator()`. It must be a **struct returned by
// value** — the state lives on the stack, calls are static and inlinable, nothing is allocated.
//
// - `next()` advances and answers "is there an element". It may block (a stream such as an accept
//   loop); inside a task a blocking `next()` is a park point and a cancellation point.
// - `current()` is called only after `next()` returned true and returns the element **by value**.
// - A cursor is consumed by **one** loop at a time (calling `next()` mutates it); thread safety is
//   not required. Errors are reported by throwing, not by a sentinel value.
public interface IIterator<T> {
    bool next();
    T current();
}

// Index form (SPEC 6.4.1): the container half is `usize length()` plus a subscript. The subscript
// half stays structural because an interface cannot declare `operator[]` — either a member
// `T operator[](usize i)` (preferred: the receiver is borrowed) or a free function
// `T operator[](C, usize)`; `for ref` additionally requires the subscript to return `ref T`.
//
// A type that also provides `iterator()` supports the cursor form as well; the cursor form wins
// (SPEC 6.4.3).
public interface IIndexable<T> {
    usize length();
}

// The container half of the cursor form: the element type is the interface's type argument, and the
// cursor is returned **as an interface value** (`IIterator<T>`, two words). That is what makes
// `IEnumerable<T>` usable as a plain generic contract: a `class` implements `IIterator<T>` and this
// interface, and dispatch goes through the itable. A `struct` cannot be handed out this way (that
// would be boxing, 3.15.8), so a struct container returns a **class** cursor; a struct cursor is
// still iterated statically by `for-in` (SPEC 6.4.2).
public interface IEnumerable<T> {
    IIterator<T> iterator();
}

// ---------------------------------------------------------------- equality and ordering
//
// `Self` is the implementor type, so `IOrdered` means "`a < b` is meaningful for this type".
// Built-in operators satisfy an operator member when the type is numeric (equality/ordering) or a
// pointer (ordering); enums take part in comparison but not in arithmetic (SPEC 3.5).
public interface IEquatable {
    bool operator==(Self a, Self b);
    bool operator!=(Self a, Self b);
}

public interface IOrdered : IEquatable {
    bool operator<(Self a, Self b);
    bool operator<=(Self a, Self b);
    bool operator>(Self a, Self b);
    bool operator>=(Self a, Self b);
}

// ---------------------------------------------------------------- arithmetic
//
// Only `+ - * / %` and comparison are recognised as built-in operators by the constraint checker,
// so the arithmetic contracts are split per operator instead of bundling a bitwise group (a
// bitwise contract could not be satisfied by the built-in integer types today).
public interface IAddable {
    Self operator+(Self a, Self b);
}

public interface ISubtractable {
    Self operator-(Self a, Self b);
}

public interface IMultipliable {
    Self operator*(Self a, Self b);
}

public interface IDivisible {
    Self operator/(Self a, Self b);
}

public interface IModulable {
    Self operator%(Self a, Self b);
}

// Unary negation: `-x`.
public interface INegatable {
    Self operator-(Self a);
}

// A full arithmetic scalar (C# generic-math shape): satisfies numeric algorithms over `T: INumber`.
// Float types qualify too (`%` is `frem`, SPEC 5.1), which is why `IModulable` stays separate
// instead of being part of the bundle.
public interface INumber : IAddable, ISubtractable, IMultipliable, IDivisible, INegatable, IOrdered {
}
