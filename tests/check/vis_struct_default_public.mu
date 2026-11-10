unit vis_struct_default_public;
use lib_vis_structpub;

int main() {
    Point2 p = Point2(1, 2);
    return p.x + p.sum();
}
