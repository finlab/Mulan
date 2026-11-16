// std.target — the build constants a `#if` condition may read.
//
// The declarations exist so ordinary code can use the same names (`switch os { ... }` gets the
// exhaustive-coverage check); the **values are filled by the compiler** from `--platform` /
// `--target` / `-g` / `--bounds` / `--single-thread`. The initializers below are placeholders: the
// compiler replaces them during semantic analysis (Sema::seed_target_unit) and checks that these
// enums match its own table. Do not edit the member order — the value is the declaration index.
//
// See docs/SPEC.md §9.2 for the directive rules.
public unit std.target;

public enum OS { Windows, Linux, MacOS }
public enum Arch { X86_64, AArch64 }
public enum Abi { Msvc, Ucrt, Gnu, None }
public enum Endian { Little, Big }

public const OS os = OS.Windows;
public const Arch arch = Arch.X86_64;
public const Abi abi = Abi.None;
public const Endian endian = Endian.Little;

public const int ptr_bits = 64;
public const bool debug = false;
public const bool bounds = true;
public const bool single_thread = false;
