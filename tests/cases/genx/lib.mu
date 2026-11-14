// A generic type declared here, instantiated with types from the client unit (and with its own).
public unit genx.lib;

public struct Item {
    public int v;
}

public struct Box<T> {
    public T item;
}

public int lib_boxed(Box<Item> b) {
    return b.item.v;
}

public Box<Item> lib_make(int v) {
    Box<Item> b = { item = { v = v } };
    return b;
}
