unit vis_multi_seg;
use lib_visopen.app;

int main() {
    lib_visopen.app.X x = {7};
    return lib_visopen.app.f() + x.v;
}
