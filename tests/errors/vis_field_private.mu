unit vis_field_private;
use lib_vis_cls;

int main() {
    lib_vis_cls.C c = new lib_vis_cls.C();
    return c.hidden;
}
