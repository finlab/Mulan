unit lib_vis_cls;

public class C {
    int hidden;
    public int shown;

    public C() {
        hidden = 0;
        shown = 0;
    }

    int hidden_m() {
        return hidden;
    }

    public int open_m() {
        return hidden;
    }
}
