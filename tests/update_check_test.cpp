#include "app/update.h"
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    wardrobe::update::Updater updater(true);
    for (int i = 0; i < 200; ++i) {
        const auto result = updater.Current();
        using wardrobe::update::State;
        if (result.state == State::Current || result.state == State::Available || result.state == State::Failed) {
            std::cout << "state=" << int(result.state) << " version=" << result.version << '\n';
            if (int(result.state) != std::stoi(argv[1])) return 1;
            if (result.state == State::Failed && (result.error.empty() || !result.url.empty())) return 1;
            return 0;
        }
        Sleep(25);
    }
    return 1;
}
