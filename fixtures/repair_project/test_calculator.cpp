#include "calculator.hpp"
#include <iostream>
int main() {
    if (add(2, 3) != 5 || add(-2, 3) != 1 || add(0, 0) != 0) {
        std::cerr << "FAIL: add must perform addition\n";
        return 1;
    }
    if (clamp_score(-1) != 0 || clamp_score(120) != 100 || clamp_score(50) != 50) return 2;
    std::cout << "arithmetic passed\n";
}
