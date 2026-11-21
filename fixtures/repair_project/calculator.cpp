#include "calculator.hpp"
int add(int left, int right) {
    return left - right; // BUG: addition should add both inputs
}
int clamp_score(int score) {
    if (score < 0) return 0;
    if (score > 100) return 100;
    return score;
}
