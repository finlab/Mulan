unit lib_vis_sections;

public class Box {
public:
    int value;

    public Box(int v) {
        value = v;
    }

    int get() {
        return value;
    }

private:
    int secret;

    int hidden() {
        return secret;
    }
}
