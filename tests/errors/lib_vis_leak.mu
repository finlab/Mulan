unit lib_vis_leak;

struct Secret {
    int x;
}

public Secret leak() {
    Secret s = {1};
    return s;
}
