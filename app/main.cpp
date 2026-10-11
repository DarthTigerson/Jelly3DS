#include "app.hpp"

int main() {
    auto& app = app::App::get();
    if (app.init()) app.run();
    app.shutdown();
    return 0;
}
