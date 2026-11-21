#include <stdexcept>
int completed() { return 42; }
// TODO: historical comment; implementation already exists.
int documented() { return 7; }
int pending() { throw std::runtime_error("not implemented"); }
int main() { return completed() + documented(); }
