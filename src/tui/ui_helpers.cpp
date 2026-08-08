#include "ui_helpers.hpp"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>

namespace dm::ui {

namespace {

WINDOW* makeCentered(int height, int width) {
    height = std::min(height, LINES - 2);
    width = std::min(width, COLS - 2);
    int y = std::max(0, (LINES - height) / 2);
    int x = std::max(0, (COLS - width) / 2);
    WINDOW* w = newwin(height, width, y, x);
    keypad(w, TRUE);
    return w;
}

void drawFrame(WINDOW* w, const std::string& title) {
    box(w, 0, 0);
    if (!title.empty()) {
        mvwprintw(w, 0, 2, " %s ", title.c_str());
    }
}

int measureWidth(const std::vector<std::string>& items, const std::string& title) {
    size_t w = title.size();
    for (auto& s : items) w = std::max(w, s.size());
    return static_cast<int>(w) + 8;
}

} // namespace

int runMenu(const std::string& title, const std::vector<std::string>& items, const std::string& footer) {
    if (items.empty()) return -1;
    int width = std::max(30, measureWidth(items, title));
    int height = static_cast<int>(items.size()) + 4 + (footer.empty() ? 0 : 1);
    WINDOW* w = makeCentered(height, width);
    int sel = 0;
    int top = 0;
    int visibleRows = height - 4 - (footer.empty() ? 0 : 1);
    int result = -1;
    while (true) {
        werase(w);
        drawFrame(w, title);
        if (sel < top) top = sel;
        if (sel >= top + visibleRows) top = sel - visibleRows + 1;
        for (int row = 0; row < visibleRows && top + row < (int)items.size(); ++row) {
            int idx = top + row;
            if (idx == sel) wattron(w, A_REVERSE);
            mvwprintw(w, 2 + row, 2, "%-*.*s", width - 4, width - 4, items[idx].c_str());
            if (idx == sel) wattroff(w, A_REVERSE);
        }
        if (!footer.empty()) mvwprintw(w, height - 2, 2, "%-*.*s", width - 4, width - 4, footer.c_str());
        wrefresh(w);
        int ch = wgetch(w);
        if (ch == KEY_UP || ch == 'k') { sel = (sel - 1 + (int)items.size()) % items.size(); }
        else if (ch == KEY_DOWN || ch == 'j') { sel = (sel + 1) % items.size(); }
        else if (ch == '\n' || ch == KEY_ENTER) { result = sel; break; }
        else if (ch == 'q' || ch == 27) { result = -1; break; }
        // ERR means wgetch had nothing to read and nothing to block on (e.g. stdin
        // isn't a real terminal -- closed pipe, lost SSH session). Without this,
        // a non-interactive stdin makes wgetch return ERR immediately forever,
        // busy-spinning this loop at 100% CPU instead of blocking for a keypress.
        else if (ch == ERR) { result = -1; break; }
    }
    delwin(w);
    touchwin(stdscr);
    refresh();
    return result;
}

std::vector<int> runChecklist(const std::string& title, const std::vector<std::string>& items,
                               const std::string& footer, bool& cancelled) {
    cancelled = false;
    std::vector<int> checked;
    if (items.empty()) { cancelled = true; return checked; }
    std::vector<bool> mark(items.size(), false);
    int width = std::max(30, measureWidth(items, title) + 4);
    int height = static_cast<int>(items.size()) + 4 + (footer.empty() ? 0 : 1);
    WINDOW* w = makeCentered(height, width);
    int sel = 0;
    int top = 0;
    int visibleRows = height - 4 - (footer.empty() ? 0 : 1);
    while (true) {
        werase(w);
        drawFrame(w, title);
        if (sel < top) top = sel;
        if (sel >= top + visibleRows) top = sel - visibleRows + 1;
        for (int row = 0; row < visibleRows && top + row < (int)items.size(); ++row) {
            int idx = top + row;
            if (idx == sel) wattron(w, A_REVERSE);
            mvwprintw(w, 2 + row, 2, "[%c] %-*.*s", mark[idx] ? 'x' : ' ', width - 8, width - 8, items[idx].c_str());
            if (idx == sel) wattroff(w, A_REVERSE);
        }
        if (!footer.empty()) mvwprintw(w, height - 2, 2, "%-*.*s", width - 4, width - 4, footer.c_str());
        wrefresh(w);
        int ch = wgetch(w);
        if (ch == KEY_UP || ch == 'k') { sel = (sel - 1 + (int)items.size()) % items.size(); }
        else if (ch == KEY_DOWN || ch == 'j') { sel = (sel + 1) % items.size(); }
        else if (ch == ' ') { mark[sel] = !mark[sel]; }
        else if (ch == '\n' || ch == KEY_ENTER) { break; }
        else if (ch == 'q' || ch == 27) { cancelled = true; break; }
        else if (ch == ERR) { cancelled = true; break; } // see runMenu's ERR comment
    }
    delwin(w);
    touchwin(stdscr);
    refresh();
    if (!cancelled) {
        for (size_t i = 0; i < mark.size(); ++i) if (mark[i]) checked.push_back((int)i);
    }
    return checked;
}

std::string promptInput(const std::string& prompt, bool& cancelled, const std::string& initial) {
    cancelled = false;
    int width = std::max(50, (int)prompt.size() + 10);
    WINDOW* w = makeCentered(5, width);
    std::string value = initial;
    curs_set(1);
    while (true) {
        werase(w);
        drawFrame(w, "Input");
        mvwprintw(w, 1, 2, "%s", prompt.c_str());
        mvwprintw(w, 2, 2, "> %-*.*s", width - 6, width - 6, value.c_str());
        wmove(w, 2, 4 + (int)value.size());
        wrefresh(w);
        int ch = wgetch(w);
        if (ch == '\n' || ch == KEY_ENTER) break;
        if (ch == 27 || ch == ERR) { cancelled = true; break; } // see runMenu's ERR comment
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { if (!value.empty()) value.pop_back(); continue; }
        if (ch >= 32 && ch < 256 && value.size() < 200) value.push_back((char)ch);
    }
    curs_set(0);
    delwin(w);
    touchwin(stdscr);
    refresh();
    if (cancelled) return "";
    return value;
}

bool promptConfirmPhrase(const std::string& warning, const std::string& phrase) {
    bool cancelled = false;
    std::string typed = promptInput(warning + "\nType \"" + phrase + "\" to confirm:", cancelled);
    return !cancelled && typed == phrase;
}

bool promptYesNo(const std::string& question) {
    int idx = runMenu(question, {"No", "Yes"});
    return idx == 1;
}

void showText(const std::string& title, const std::string& body) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= body.size()) {
        size_t nl = body.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(body.substr(start)); break; }
        lines.push_back(body.substr(start, nl - start));
        start = nl + 1;
    }
    int height = std::min((int)lines.size() + 4, LINES - 2);
    int width = std::min(COLS - 4, std::max(40, 100));
    WINDOW* w = makeCentered(std::max(height, 8), width);
    int visibleRows = std::max(height, 8) - 4;
    int top = 0;
    while (true) {
        werase(w);
        drawFrame(w, title);
        for (int row = 0; row < visibleRows && top + row < (int)lines.size(); ++row) {
            mvwprintw(w, 2 + row, 2, "%-*.*s", width - 4, width - 4, lines[top + row].c_str());
        }
        mvwprintw(w, std::max(height, 8) - 2, 2, "q/Enter: close  arrows: scroll");
        wrefresh(w);
        int ch = wgetch(w);
        if (ch == 'q' || ch == 27 || ch == '\n' || ch == KEY_ENTER || ch == ERR) break; // see runMenu's ERR comment
        if (ch == KEY_DOWN && top + visibleRows < (int)lines.size()) top++;
        if (ch == KEY_UP && top > 0) top--;
    }
    delwin(w);
    touchwin(stdscr);
    refresh();
}

void showMessage(const std::string& title, const std::string& message) {
    showText(title, message);
}

void showProgress(const std::string& title,
                   const std::function<void(std::atomic<bool>& cancel,
                                             const std::function<void(double, const std::string&)>& update)>& work) {
    int width = std::min(COLS - 4, 70);
    WINDOW* w = makeCentered(7, width);
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::mutex m;
    double percent = 0.0;
    std::string status = "starting...";

    auto update = [&](double p, const std::string& s) {
        std::lock_guard<std::mutex> lock(m);
        percent = p;
        status = s;
    };

    std::thread worker([&]() {
        work(cancel, update);
        done = true;
    });

    nodelay(w, TRUE);
    while (!done.load()) {
        double p; std::string s;
        { std::lock_guard<std::mutex> lock(m); p = percent; s = status; }
        werase(w);
        drawFrame(w, title);
        int barWidth = width - 8;
        int filled = barWidth > 0 ? (int)(barWidth * std::clamp(p, 0.0, 100.0) / 100.0) : 0;
        mvwprintw(w, 2, 2, "[");
        for (int i = 0; i < barWidth; ++i) waddch(w, i < filled ? '#' : '-');
        wprintw(w, "] %5.1f%%", p);
        mvwprintw(w, 3, 2, "%-*.*s", width - 4, width - 4, s.c_str());
        mvwprintw(w, 5, 2, "press 'c' to cancel");
        wrefresh(w);
        int ch = wgetch(w);
        if (ch == 'c') cancel = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    worker.join();
    nodelay(w, FALSE);
    delwin(w);
    touchwin(stdscr);
    refresh();
}

} // namespace dm::ui
