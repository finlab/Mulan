unit vis_pub_ok;
use lib_vis_pub;

int main() {
    lib_vis_pub.Open o = new lib_vis_pub.Open(1);
    o.value = 2;
    int a = o.get();
    int b = lib_vis_pub.add(1, 2);
    lib_vis_pub.Point p = {3, 4};
    return a + b + p.x;
}
