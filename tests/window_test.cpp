// SPDX-License-Identifier: GPL-3.0-or-later
#include "manager.h"
#include "base/timer.h"
#include "base/i18n.h"
#include <libintl.h>
#include <clocale>
#include <cstdlib>
#include "base/event_loop.h"
#include "ui/application.h"
#include "ui/header_button.h"
#include "ui/window_header.h"
#include "ui/test_input.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

using namespace Lingmo::ui;
static void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
static View *find(View *root, const std::string &name)
{
    if (root->name() == name) return root;
    for (const auto &child : root->children())
        if (auto *found = find(child.get(), name)) return found;
    return nullptr;
}
int main(int argc, char **argv)
{
    try {
        Application app(argc, argv, Application::Platform::Headless);
        if (argc > 2) {
            setenv("LANGUAGE", "zh_CN", 1);
            std::setlocale(LC_ALL, "zh_CN.UTF-8");
            Lingmo::I18n::setDomain("lingmo-wine");
            bindtextdomain("lingmo-wine", argv[2]);
        }
        Manager manager(false);
        auto &window = manager.window();
        window.show();
        for (bool dark : {false, true}) {
            Theme::setCurrent(dark ? Theme::dark() : Theme::light());
            for (bool glass : {false, true}) {
                TestInput::setGlassAvailable(glass);
                for (double width : {840.0, 960.0, 1100.0}) {
                    for (double scale : {1.0, 1.5}) {
                        window.resize({width, 720});
                        window.setScale(scale);
                        for (int page = 0; page < 4; ++page) {
                            manager.showPage(page);
                            auto image = window.grab();
                            auto *root = window.contentView();
                            auto *sidebar = find(root, "sidebar");
                            auto *content = find(root, "main-content");
                            check(sidebar && content, "Missing split layout");
                            check(sidebar->geometry().x == 0 && sidebar->geometry().width == 224, "Sidebar bounds");
                            check(content->geometry().x == 224, "Content must follow sidebar");
                            for (int i = 0; i < 4; ++i)
                                check(find(root, "page-" + std::to_string(i))->isVisible() == (i == page), "Pages must be exclusive");
                            check(image.pixel(int(5 * scale), int(200 * scale)).a == (glass ? 0 : 255),
                                  "Sidebar must be transparent over glass and opaque in fallback");
                            check(image.pixel(int(226 * scale), int(200 * scale)).a == 255, "Content must be opaque");
                            check(image.pixel(int(226 * scale), int(15 * scale)).a == 255, "Right header must stay opaque");
                            auto material = TestInput::backgroundGlass(window);
                            check(bool(material) == glass, "Glass material request");
                            WindowHeader *header = nullptr;
                            for (const auto &child : root->children())
                                if (auto *value = dynamic_cast<WindowHeader *>(child.get())) header = value;
                            check(header && header->geometry().x == 224, "Window header must be on right content");
                            check(std::abs(header->geometry().width - (width - 224)) < 1, "Window header width");
                            unsigned buttons = 0;
                            for (const auto &child : header->children()) {
                                auto *button = dynamic_cast<HeaderButton *>(child.get());
                                if (!button) continue;
                                ++buttons;
                                const auto rect = button->geometry();
                                check(rect.width > 0 && rect.x >= 0 && rect.x + rect.width <= header->geometry().width,
                                      "Window button outside header");
                            }
                            check(buttons == 3, "Missing window buttons");
                            if (argc > 1 && width == 960) {
                                std::filesystem::create_directories(argv[1]);
                                const std::string path = std::string(argv[1]) + "/page-" + std::to_string(page)
                                    + (dark ? "-dark" : "-light") + (glass ? "-glass" : "-fallback")
                                    + (scale == 1 ? "-100.png" : "-150.png");
                                check(image.savePng(path), "Save capture");
                            }
                        }
                    }
                }
            }
        }
        check(!manager.canPerform(Manager::Action::Run), "Cannot run without environment or Wine");
        check(!manager.canPerform(Manager::Action::Register), "Cannot register without EXE and environment");
        check(!manager.canPerform(Manager::Action::Create), "Empty environment name must disable creation");
        manager.trigger(Manager::Action::Run); // Must be ignored by dispatcher too.
        auto *fallback = find(window.contentView(), "menu-fallback");
        check(fallback && fallback->isVisible(), "No registrar must retain compact menu");
        // Switching to an auxiliary page must not lose in-progress input.
        manager.showPage(2);
        auto *name = dynamic_cast<TextField *>(find(window.contentView(), "environment-name"));
        name->setText("Preserved environment");
        check(manager.canPerform(Manager::Action::Create), "Valid name enables creation");
        name->setText("bad\nname");
        check(!manager.canPerform(Manager::Action::Create), "Invalid name must disable creation");
        name->setText("Preserved environment");
        manager.showPage(3); manager.showPage(2);
        check(name->text() == "Preserved environment", "Page switch lost input");
        WindowHeader *header = nullptr;
        for (const auto &child : window.contentView()->children())
            if (auto *value = dynamic_cast<WindowHeader *>(child.get())) header = value;
        HeaderButton *close = nullptr;
        for (const auto &child : header->children())
            if (auto *button = dynamic_cast<HeaderButton *>(child.get()); button && button->artwork() == "close") close = button;
        check(close, "Missing Close");
        const auto rect = close->geometry();
        app.loop().post([&window, header, rect] {
            TestInput::click(window, {header->geometry().x + rect.x + rect.width / 2, rect.y + rect.height / 2});
        });
        bool timedOut = false;
        Lingmo::base::Timer deadline;
        deadline.setSingleShot(true);
        deadline.onTimeout = [&] { timedOut = true; app.quit(1); };
        deadline.start(std::chrono::milliseconds(100));
        check(app.run() == 0 && !timedOut, "Close must quit idle manager");
        std::puts("Split layout, materials, page state and close tests passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
