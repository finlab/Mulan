unit lib_vis_structpub;

public struct Point2 {
    int x, y;              // struct 默认 public

    public Point2(int a, int b) {
        x = a;
        y = b;
    }

    public int sum() {
        return x + y;
    }
}
