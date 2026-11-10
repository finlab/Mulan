unit lib_vis_struct;

public struct Config {
    private int secret;
    public int shown;

    public Config(int s, int v) {
        secret = s;
        shown = v;
    }

    public int get_secret() {
        return secret;
    }
}
