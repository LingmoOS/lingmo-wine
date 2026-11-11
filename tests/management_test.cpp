// SPDX-License-Identifier: GPL-3.0-or-later
#include "manager.h"
#include "base/event_loop.h"
#include "ui/application.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <poll.h>
#include <cstdio>
#include <stdexcept>
using namespace Lingmo;
namespace fs = std::filesystem;
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
ui::View *find(ui::View *root, const std::string &name) {
    if (root->name() == name) return root;
    for (const auto &child : root->children()) if (auto *v = find(child.get(), name)) return v;
    return nullptr;
}
int main(int argc, char **argv) {
    char temporary[] = "/tmp/lingmo-wine-management-XXXXXX";
    const char *directory = mkdtemp(temporary);
    if (!directory) return 1;
    fs::path root(directory);
    try {
        setenv("XDG_DATA_HOME", root.c_str(), 1);
        ui::Application app(argc, argv, ui::Application::Platform::Headless);
        const std::string id(32, 'a');
        const auto env = root / "lingmo/wine/environments" / id;
        fs::create_directories(env);
        std::ofstream(env / "environment.json") << "{\"id\":\"" << id << "\",\"name\":\"Original\",\"arch\":\"win64\"}";
        const auto executable = root / "Test Program.exe";
        std::ofstream(executable) << "test only";
        Manager manager;
        manager.window().show(); manager.window().grab();
        const auto until = [&](const std::function<bool()> &done) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!done() && std::chrono::steady_clock::now() < end) {
                pollfd fd{app.loop().fd(), POLLIN, 0}; poll(&fd, 1, 10); app.loop().dispatch();
            }
            check(done(), "Management operation timeout");
        };
        const auto complete = [&] { until([&] { return manager.canPerform(Manager::Action::Refresh); }); };
        complete();
        auto *view = manager.window().contentView();
        auto *envName = dynamic_cast<ui::TextField *>(find(view, "environment-name"));
        auto *appName = dynamic_cast<ui::TextField *>(find(view, "application-name"));
        auto *file = dynamic_cast<ui::TextField *>(find(view, "installed-executable"));
        auto *apps = dynamic_cast<ui::ComboBox *>(find(view, "registered-applications"));
        check(envName && appName && file && apps, "Management fields missing");
        check(envName->text() == "Original", "Environment selection not loaded");
        envName->setText("Renamed"); manager.trigger(Manager::Action::RenameEnvironment);
        check(!envName->isEnabled(), "Busy inputs must be frozen");
        complete(); check(envName->text() == "Renamed", "Rename not refreshed");
        appName->setText("Test app"); file->setText(executable.string());
        check(manager.canPerform(Manager::Action::Register), "Valid EXE registration disabled");
        manager.trigger(Manager::Action::Register); complete();
        check(apps->items().size() == 2, "Registered app not listed");
        apps->setCurrentIndex(1); apps->onActivated(1);
        appName->setText("Edited app"); manager.trigger(Manager::Action::SaveApplication); complete();
        check(apps->items()[1] == "Edited app", "Edit not refreshed");
        manager.trigger(Manager::Action::RemoveApplication); complete();
        check(apps->items().size() == 1, "Removed app still listed");
        check(fs::exists(executable) && fs::exists(env), "Removal touched installed files");
        check(manager.canPerform(Manager::Action::RestoreApplication), "Undo unavailable");
        manager.trigger(Manager::Action::RestoreApplication); complete();
        check(apps->items().size() == 2, "Undo did not restore entry");
        check(!manager.canPerform(Manager::Action::RestoreApplication), "Undo stayed enabled");
        // A new environment must become the selection even if an existing
        // registered application was selected before the creation.
        apps->setCurrentIndex(1); apps->onActivated(1);
        envName->setText("Second environment");
        manager.trigger(Manager::Action::Create); complete();
        check(envName->text() == "Second environment", "New environment was not selected");
        check(!manager.canPerform(Manager::Action::SaveApplication), "Old app remained selected in new environment");
        apps->setCurrentIndex(1); apps->onActivated(1);
        check(envName->text() == "Renamed", "Selecting app did not restore its actual environment");
        fs::create_directories(root / "lingmo/wine/logs");
        std::ofstream(root / "lingmo/wine/logs" / (id + ".log")) << "Diagnostic fixture";
        manager.trigger(Manager::Action::ReadLog);
        until([&] { return manager.canPerform(Manager::Action::ReadLog); });
        auto *log = dynamic_cast<ui::TextArea *>(find(view, "environment-log"));
        check(log && log->text() == "Diagnostic fixture", "Environment log did not reach the log page");
        std::puts("Management UI and real helper integration passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what()); fs::remove_all(root); return 1;
    }
    fs::remove_all(root);
}
