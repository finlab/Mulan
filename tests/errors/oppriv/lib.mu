public unit oppriv.lib;
public struct V { public int x; }
private V operator+(V a, V b) { V r; r.x = 999; return r; }
