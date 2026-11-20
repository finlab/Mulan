// A generic interface and a generic struct implementing it, declared in a library unit.
public unit genx.iface;

public interface IVal<T> {
    T get();
    int twice() { return 2; }
}

public struct Wrap<T> : IVal<T> {
    public T v;
    public Wrap() {}
    public T get() { return v; }
}
