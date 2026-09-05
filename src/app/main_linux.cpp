#include <ember/app/main.h>

#include <cstddef>

int main(int argc, char** argv) { return ember_main({.args = {argv, static_cast<std::size_t>(argc)}}); }
