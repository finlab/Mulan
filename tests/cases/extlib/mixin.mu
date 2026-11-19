// A library unit that declares an interface extension (SPEC 3.18): every implementor of `IShape`
// gains `IMixShape`'s default methods in the units that can see this declaration.
public unit extlib.mixin;

public interface IShape {
    int area();
}

public interface IMixShape : IShape {
    int doubled() { return this.area() * 2; }
    int scaled(int k) { return this.area() * k; }
}

public extend IShape with IMixShape;
