unit vis_struct_field_private;
use lib_vis_struct;

int main() {
    Config c = Config(1, 2);
    return c.secret;
}
