#include "gui_app.hpp"
#include <gtkmm/application.h>

int main(int argc, char** argv) {
    auto app = Gtk::Application::create(argc, argv, "org.diskmanager.gui");
    dm::GuiApp window;
    return app->run(window);
}
