unit lib_vis_pub;

public struct Point {
    int x, y;
}

public class Open {
    public int value;

    public Open(int v) {
        value = v;
    }

    public int get() {
        return value;
    }

    int internal() {
        return value;
    }
}

public int add(int a, int b) {
    return a + b;
}
