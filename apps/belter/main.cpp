#include <format>
#include <iostream>

#include "expanse/game.hpp"

int main() {
    std::cout << std::format("{} — nothing to simulate yet.\n", expanse::game_name());
    return 0;
}
