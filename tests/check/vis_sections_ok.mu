unit vis_sections_ok;
use lib_vis_sections;

int main() {
    Box b = new Box(3);
    return b.value + b.get();
}
