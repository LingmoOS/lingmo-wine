// SPDX-License-Identifier: GPL-3.0-or-later
#include "manager.h"
#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/paths.h"
#include "ui/application.h"
#include "ui/button.h"
#include "ui/layout.h"
#include "ui/window_header.h"
#include <filesystem>
#include <algorithm>
#include <cstdlib>

using namespace Lingmo;
using namespace Lingmo::ui;
using Lingmo::i18n::tr;

Manager::Manager(bool load)
{
    m_window.setTitle(tr("Windows Applications"));
    m_window.resize({760, 690});
    m_window.setMinimumSize({700, 640});
    auto root = std::make_unique<Column>();
    root->addChild<WindowHeader>();
    auto *body = root->addChild<Column>();
    body->setMargins({24, 16, 24, 24});
    body->setSpacing(12);
    body->setLayoutHints({.fillWidth = true, .fillHeight = true});
    auto *title = body->addChild<Label>(tr("Windows Applications"));
    title->setFontSize(24);
    title->setWeight(600);
    auto *intro = body->addChild<Label>(tr("Create an environment for each application, or reuse one for related programs."));
    intro->setWrap(true);
    intro->setLayoutHints({.fillWidth = true});
    body->addChild<Label>(tr("Environment"));
    m_environments = body->addChild<ComboBox>();
    m_environments->setLayoutHints({.fillWidth = true});
    auto *newRow = body->addChild<Row>();
    newRow->setSpacing(12); newRow->setLayoutHints({.fillWidth = true});
    m_name = newRow->addChild<TextField>();
    m_name->setPlaceholder(tr("Environment or application name"));
    m_name->setLayoutHints({.fillWidth = true});
    auto *create = newRow->addChild<Button>(tr("Create environment"));
    create->onClicked = [this] { perform({"create", m_name->text()}, [this] {
        const auto created = base::json::Value::parse(m_task.standardOutput());
        refresh(created ? (*created)["id"].toString() : std::string());
    }); };
    auto *files = body->addChild<Row>();
    files->setSpacing(12); files->setLayoutHints({.fillWidth = true});
    m_file = files->addChild<TextField>();
    m_file->setPlaceholder(tr("Select an EXE or MSI file"));
    m_file->setLayoutHints({.fillWidth = true});
    auto *browse = files->addChild<Button>(tr("Browse…"));
    browse->onClicked = [this] { choose(); };
    auto *actions = body->addChild<Row>(); actions->setSpacing(10);
    auto *run = actions->addChild<Button>(tr("Install / Run"));
    run->onClicked = [this] { runFile("run"); };
    auto *shortcut = actions->addChild<Button>(tr("Add to launcher"));
    shortcut->onClicked = [this] {
        if (environment().empty()) { m_status->setText(tr("Create or select an environment first.")); return; }
        perform({"register", environment(), m_name->text(), m_file->text()}, [this] { refresh(); });
    };
    auto *advanced = body->addChild<Row>(); advanced->setSpacing(10);
    for (const auto &[label, command] : std::vector<std::pair<std::string, std::string>>{
             {tr("Wine settings"), "configure"}, {tr("Uninstall programs"), "uninstall"}}) {
        auto *button = advanced->addChild<Button>(label);
        button->onClicked = [this, command] { runFile(command); };
    }
    auto *stop = advanced->addChild<Button>(tr("Stop environment"));
    stop->onClicked = [this] {
        const std::string id = m_runningEnvironment.empty() ? environment() : m_runningEnvironment;
        if (id.empty() || m_stop.isRunning()) return;
        m_stop.onFinished = [this](int code, bool) {
            m_status->setText(code ? tr("Could not stop the environment.") : tr("Stop requested."));
        };
        if (!m_stop.start("/usr/bin/python3", {WINE_HELPER, "stop", id}))
            m_status->setText(tr("Could not start the Wine helper."));
    };
    body->addChild<Label>(tr("Registered applications"));
    auto *apps = body->addChild<Row>(); apps->setSpacing(12); apps->setLayoutHints({.fillWidth = true});
    m_apps = apps->addChild<ComboBox>(); m_apps->setLayoutHints({.fillWidth = true});
    auto *launchButton = apps->addChild<Button>(tr("Launch"));
    launchButton->onClicked = [this] {
        int index = m_apps->currentIndex();
        if (index >= 0 && size_t(index) < m_appIds.size()) launch(m_appIds[index]);
    };
    m_status = body->addChild<Label>(tr("Ready")); m_status->setWrap(true); m_status->setLayoutHints({.fillWidth = true});
    m_log = body->addChild<TextArea>(); m_log->setReadOnly(true);
    m_log->setLayoutHints({.fillWidth = true, .fillHeight = true});
    auto *logs = body->addChild<Button>(tr("Open logs"));
    logs->onClicked = [] {
        const char *xdg = std::getenv("XDG_DATA_HOME");
        const std::string data = xdg && *xdg ? xdg : base::paths::dataHome();
        base::Process::startDetached("xdg-open", {data + "/lingmo/wine/logs"});
    };
    m_window.setContentView(std::move(root));
    m_window.onCloseRequested = [this] {
        if (m_task.isRunning() || m_stop.isRunning()) {
            m_status->setText(tr("Close Windows applications or stop the environment before closing this window."));
        } else { ui::Application::instance()->quit(); }
    };
    if (load) refresh();
}

void Manager::append(std::string_view text)
{
    std::string content = m_log->text() + std::string(text);
    if (content.size() > 12000) content.erase(0, content.size() - 12000);
    m_log->setText(content);
}

void Manager::refresh(const std::string &preferred)
{
    if (m_read.isRunning()) return;
    const std::string selected = preferred.empty() ? environment() : preferred;
    m_read.onFinished = [this, selected](int code, bool) {
        auto data = base::json::Value::parse(m_read.standardOutput());
        if (code || !data) { m_status->setText(tr("Could not read Wine environments.")); append(m_read.standardError()); return; }
        m_ids.clear(); std::vector<std::string> names;
        int index = 0;
        for (const auto &item : (*data)["environments"].toArray()) {
            if (item["id"].toString() == selected) index = int(names.size());
            m_ids.push_back(item["id"].toString()); names.push_back(item["name"].toString());
        }
        m_environments->setItems(names); m_environments->setCurrentIndex(names.empty() ? -1 : index);
        m_appIds.clear(); m_appEnvironments.clear(); names.clear();
        for (const auto &item : (*data)["applications"].toArray()) {
            m_appIds.push_back(item["id"].toString());
            m_appEnvironments.push_back(item["environment"].toString());
            names.push_back(item["name"].toString());
        }
        m_apps->setItems(names); m_apps->setCurrentIndex(names.empty() ? -1 : 0);
        if (!(*data)["wineAvailable"].toBool())
            m_status->setText(tr("Wine is not installed. Install the Wine runtime packages to continue."));
    };
    if (!m_read.start("/usr/bin/python3", {WINE_HELPER, "list"}))
        m_status->setText(tr("Could not start the Wine helper."));
}
std::string Manager::environment() const
{
    int index = m_environments->currentIndex();
    return index >= 0 && size_t(index) < m_ids.size() ? m_ids[index] : std::string();
}
void Manager::perform(std::vector<std::string> arguments, std::function<void()> done)
{
    if (m_task.isRunning()) { m_status->setText(tr("An operation is already running.")); return; }
    m_log->setText({}); m_status->setText(tr("Working…"));
    m_task.onStandardOutput = [this](std::string_view s) { append(s); };
    m_task.onStandardError = [this](std::string_view s) { append(s); };
    m_task.onFinished = [this, done](int code, bool crashed) {
        m_runningEnvironment.clear();
        m_status->setText(code || crashed ? tr("Operation failed. See the details and logs.") : tr("Finished"));
        // Follow-ups only refresh the separate metadata process.
        if (!code && !crashed && done) done();
    };
    arguments.insert(arguments.begin(), WINE_HELPER);
    if (!m_task.start("/usr/bin/python3", arguments)) {
        m_runningEnvironment.clear(); m_status->setText(tr("Could not start the Wine helper."));
    }
}
void Manager::runFile(const std::string &operation)
{
    if (m_task.isRunning()) return;
    if (environment().empty()) { m_status->setText(tr("Create or select an environment first.")); return; }
    m_runningEnvironment = environment();
    std::vector<std::string> args{operation, environment()};
    if (operation == "run") args.push_back(m_file->text());
    perform(args);
}
void Manager::choose()
{
    m_chooser = std::make_unique<FileChooser>(&m_window);
    m_chooser->setFilters({{tr("Windows applications"), {"*.exe", "*.msi", "*.EXE", "*.MSI"}, {}}});
    m_chooser->onAccepted = [this](const auto &paths, const auto &) { if (!paths.empty()) openFile(paths.front()); };
    m_chooser->open();
}
void Manager::openFile(const std::string &path)
{
    m_file->setText(path);
    m_name->setText(std::filesystem::path(path).stem().string());
    m_status->setText(tr("Select an environment, then choose Install / Run."));
}
void Manager::launch(const std::string &id)
{
    if (m_task.isRunning()) return;
    const auto it = std::find(m_appIds.begin(), m_appIds.end(), id);
    if (it == m_appIds.end()) return;
    m_runningEnvironment = m_appEnvironments[size_t(it - m_appIds.begin())];
    perform({"launch", id});
}
