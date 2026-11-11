// SPDX-License-Identifier: GPL-3.0-or-later
#include "manager.h"
#include "ui/application.h"
#include "base/i18n.h"
#include <cstdio>
#include <unistd.h>
#include "base/event_loop.h"
#include "base/process.h"
int main(int argc, char **argv)
{
    std::string file, application, capture;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::puts("Usage: lingmo-wine [--open FILE | --launch ID]"); return 0;
        }
        if (arg == "--version") { std::puts("lingmo-wine 1.0.2"); return 0; }
        if ((arg == "--open" || arg == "--launch" || arg == "--capture") && i + 1 < argc) {
            (arg == "--open" ? file : arg == "--launch" ? application : capture) = argv[++i];
        } else if (!arg.starts_with("-") && file.empty()) {
            file = arg;
        } else { std::fprintf(stderr, "Unknown or incomplete option: %s\n", arg.c_str()); return 1; }
    }
    // Launcher entries stay in AppRuntime's process tree. No manager window
    // and no detached process is needed to run an already registered app.
    if (!application.empty() && capture.empty()) {
        execl("/usr/bin/python3", "python3", WINE_HELPER, "launch", application.c_str(), nullptr);
        std::perror("Wine helper");
        return 1;
    }
    std::string failure;
    if (!file.empty() && capture.empty()) {
        // No manager or environment-selection window on the double-click path.
        // Wait for the helper so launch failures can be shown, not lost on stderr.
        Lingmo::base::EventLoop loop;
        Lingmo::base::Process process(&loop);
        int result = 1;
        process.onFinished = [&](int code, bool crashed) { result = crashed ? 1 : code; loop.quit(); };
        if (process.start("/usr/bin/python3", {WINE_HELPER, "open", file})) {
            loop.run();
            if (result == 0) return 0;
            failure = process.standardOutput() + process.standardError();
        } else {
            failure = "Could not start the Wine helper.";
        }
    }
    Lingmo::ui::Application app(argc, argv, capture.empty() ? Lingmo::ui::Application::Platform::Wayland
                                                          : Lingmo::ui::Application::Platform::Headless);
    app.setApplicationId("lingmo-wine");
    Lingmo::i18n::setDomain("lingmo-wine");
    Manager manager(capture.empty() && failure.empty());
    if (!file.empty()) manager.openFile(file);
    if (!failure.empty()) manager.showFailure(failure);
    manager.window().show();
    if (!capture.empty()) return manager.window().grab().savePng(capture) ? 0 : 1;
    if (!application.empty()) manager.launch(application);
    return app.run();
}
