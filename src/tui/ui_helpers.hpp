#pragma once
#include <ncurses.h>
#include <string>
#include <vector>
#include <functional>
#include <atomic>

namespace dm::ui {

// Draws `title` plus `items` as a selectable menu inside a freshly-created centered
// window and runs a local input loop (arrows/jk to move, Enter to select, q/ESC to
// cancel). Returns the selected index, or -1 on cancel. Blocks the calling thread.
int runMenu(const std::string& title, const std::vector<std::string>& items, const std::string& footer = "");

// Same as runMenu but Space toggles membership in the returned index set instead of
// selecting immediately; Enter confirms the current selection. Returns an empty
// vector on cancel (as distinct from "confirmed with nothing checked", which also
// returns empty -- callers that need to tell these apart should check a cancelled
// flag via the overload below).
std::vector<int> runChecklist(const std::string& title, const std::vector<std::string>& items,
                               const std::string& footer, bool& cancelled);

// Single-line text input with a visible cursor. Returns "" (and sets cancelled) if
// the user presses ESC.
std::string promptInput(const std::string& prompt, bool& cancelled, const std::string& initial = "");

// Requires the user to type `phrase` exactly (default "yes") before returning true;
// ESC or a mismatched confirmation returns false. Always shows `warning` above the
// prompt so destructive actions can't be confirmed by reflex.
bool promptConfirmPhrase(const std::string& warning, const std::string& phrase = "yes");

// Simple Yes/No menu, defaults to "No" highlighted first so the safe answer is the
// path of least resistance.
bool promptYesNo(const std::string& question);

// Scrollable read-only text viewer; blocks until the user presses q/ESC/Enter.
void showText(const std::string& title, const std::string& body);

// Runs `work` on a background thread while rendering a progress bar driven by the
// thread calling the `update(percent, status)` callback it's given. Pressing 'c'
// sets the atomic<bool>& the worker receives so it can cancel cooperatively. Blocks
// the calling (UI) thread until `work` returns, redrawing at ~10Hz.
void showProgress(const std::string& title,
                   const std::function<void(std::atomic<bool>& cancel,
                                             const std::function<void(double, const std::string&)>& update)>& work);

void showMessage(const std::string& title, const std::string& message);

} // namespace dm::ui
