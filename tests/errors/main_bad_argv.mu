unit main_bad_argv;
int main(int argc, char* argv) { return argc + *argv; }
