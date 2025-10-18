#include <condy.hpp>
#include <iostream>

int main() {
    std::cout << "Condy version: " << CONDY_VERSION_MAJOR << "."
              << CONDY_VERSION_MINOR << "." << CONDY_VERSION_PATCH << std::endl;
    return 0;
}