public unit extlib2.first;

public interface I { int f(); }
public interface JFirst : I { int g() { return 1; } }

public extend I with JFirst;
