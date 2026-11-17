// std.comptime — the compiler-provided compile-time constants (SPEC 9.3).
//
// This is an ordinary unit supplied by the toolchain: `use std.comptime;` makes the names visible. The
// `builtin` declarations have no initializer and no body; the compiler binds each name to its own
// value. A builtin constant is a compile-time value: it has no storage and cannot have its address
// taken. Runtime platform symbols belong to a separate platform unit, not here.
public unit std.comptime;

public enum OS { Windows, Linux, MacOS }
public enum Arch { X86_64, AArch64 }
public enum Abi { Msvc, Ucrt, Gnu, None }
public enum Endian { Little, Big }

public builtin const OS     os;
public builtin const Arch   arch;
public builtin const Abi    abi;
public builtin const int    ptr_bits;
public builtin const Endian endian;
public builtin const bool   debug;
public builtin const bool   bounds;
public builtin const bool   single_thread;

public builtin bool is_windows();
public builtin bool is_linux();
public builtin bool is_macos();
