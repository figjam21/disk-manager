#include "tui_app.hpp"
#include <ncurses.h>
#include <clocale>

int main() {
    std::setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) start_color();

    {
        dm::TuiApp app;
        app.run();
    }

    endwin();
    return 0;
}
