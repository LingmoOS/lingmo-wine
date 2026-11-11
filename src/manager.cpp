// SPDX-License-Identifier: GPL-3.0-or-later
#include "manager.h"
#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/paths.h"
#include "ui/application.h"
#include "ui/button.h"
#include "ui/layout.h"
#include "ui/window_header.h"
#include "ui/painter.h"
#include "ui/popup_glass.h"
#include "ui/scroll_view.h"
#include "ui/theme.h"
#include "ui/keys.h"
#include <cctype>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <cmath>

using namespace Lingmo;
using namespace Lingmo::ui;
using Lingmo::i18n::tr;

namespace {

constexpr double sidebarWidth = 224;

// The only translucent region is the left sidebar. The right-hand surface
// paints opaque pixels even beneath the window controls.
class SplitRoot final : public View
{
public:
    Column *sidebar = addChild<Column>();
    Column *content = addChild<Column>();
    WindowHeader *header = addChild<WindowHeader>();
    std::function<void()> afterLayout;

    SplitRoot()
    {
        setName("split-root");
        sidebar->setName("sidebar");
        content->setName("main-content");
        sidebar->setMargins({20, 16, 20, 20});
        sidebar->setSpacing(12);
        content->setMargins({28, 12, 28, 20});
        content->setSpacing(16);
    }

protected:
    void layout() override
    {
        const double scale = window() ? window()->scale() : 1;
        const double split = std::round(sidebarWidth * scale) / scale;
        sidebar->setGeometry({0, 0, split, geometry().height});
        header->setGeometry({split, 0, geometry().width - split, 40});
        content->setGeometry({split, 40, geometry().width - split, geometry().height - 40});
        if (window()) {
            window()->setBackgroundColor(Color{0, 0, 0, 0});
            window()->setTranslucentSubpixelText(false);
            auto glass = popupGlass(Theme::current().darkMode);
            glass.blurSigma = 20;
            window()->setBackgroundGlass(window()->backgroundGlassAvailable()
                                            ? std::optional<GlassMaterial>(glass) : std::nullopt);
            window()->setBackgroundBlur({0, 0, split, geometry().height}, Theme::current().windowRadius);
        }
        if (afterLayout) afterLayout();
    }

    void paint(Painter &painter) override
    {
        const auto &theme = Theme::current();
        if (!window() || !window()->backgroundGlassAvailable())
            painter.fillRect({0, 0, sidebarWidth, geometry().height}, theme.secondBackground);
        painter.fillRect({sidebarWidth, 0, geometry().width - sidebarWidth, geometry().height}, theme.background);
    }
};

class SidebarHint final : public Label
{
public:
    using Label::Label;
protected:
    void layout() override
    {
        setColor(Theme::current().disabledText);
        Label::layout();
    }
};

class NavigationButton final : public Button
{
public:
    explicit NavigationButton(std::string title) : Button(std::move(title)) {}
    bool current = false;
protected:
    void paint(Painter &painter) override
    {
        Button::paint(painter);
        if (current)
            painter.fillRoundedRect({0, 8, 3, geometry().height - 16}, 1.5, Theme::current().highlight);
    }
};

} // namespace

Manager::Manager(bool load)
{
    m_window.setTitle(tr("Windows Applications"));
    m_window.resize({960, 720});
    m_window.setMinimumSize({840, 640});
    auto root = std::make_unique<SplitRoot>();
    auto *bar = root->header->setContent<Row>();
    bar->setMargins({12, 2, 0, 2});
    m_menuButton = bar->addChild<Button>("⋯");
    m_menuButton->setName("menu-fallback");
    m_menuButton->setToolTip(tr("Menu"));
    m_menuButton->onClicked = [this] { openMenu(); };
    auto *sidebar = root->sidebar;
    auto *title = sidebar->addChild<Label>(tr("Windows Applications"));
    title->setName("sidebar-title");
    title->setFontSize(19);
    title->setWeight(600);
    title->setWrap(true);
    title->setLayoutHints({.fillWidth = true, .preferredHeight = 60});
    sidebar->addChild<Label>(tr("Environment"));
    m_environments = sidebar->addChild<ComboBox>();
    m_environments->setLayoutHints({.fillWidth = true});
    auto *body = root->content;
    m_pageTitle = body->addChild<Label>();
    m_pageTitle->setFontSize(23);
    m_pageTitle->setWeight(600);
    m_pageTitle->setLayoutHints({.fillWidth = true});
    m_pageHost = body->addChild<View>();
    m_pageHost->setName("page-host");
    m_pageHost->setClipsChildren(true);
    m_pageHost->setLayoutHints({.fillWidth = true, .fillHeight = true});
    root->afterLayout = [this] {
        for (auto *page : m_pages)
            page->setGeometry(m_pageHost->localRect());
    };
    const std::vector<std::string> titles{tr("Install / Run"), tr("Registered applications"),
                                           tr("Environments"), tr("Logs")};
    std::vector<Column *> columns;
    for (size_t i = 0; i < titles.size(); ++i) {
        auto *nav = sidebar->addChild<NavigationButton>(titles[i]);
        nav->setName("navigation-" + std::to_string(i));
        nav->setLayoutHints({.fillWidth = true, .preferredHeight = 38});
        m_navigation.push_back(nav);
        nav->onClicked = [this, i] { showPage(int(i)); };
        auto *scroll = m_pageHost->addChild<ScrollView>();
        scroll->setName("page-" + std::to_string(i));
        m_pages.push_back(scroll);
        auto *column = scroll->setContent<Column>();
        column->setMargins({0, 0, 8, 12});
        column->setSpacing(16);
        columns.push_back(column);
    }
    sidebar->addStretch();
    auto *hint = sidebar->addChild<SidebarHint>(tr("Each environment keeps its own Windows settings and installed programs."));
    hint->setWrap(true); hint->setLayoutHints({.fillWidth = true});
    hint->setColor(Theme::current().disabledText);
    m_status = body->addChild<Label>(tr("Ready"));
    m_status->setName("operation-status");
    m_status->setWrap(true); m_status->setLayoutHints({.fillWidth = true});

    auto *install = columns[0];
    auto *intro = install->addChild<Label>(tr("Select an EXE or MSI file and the environment to use."));
    intro->setWrap(true); intro->setLayoutHints({.fillWidth = true});
    auto *files = install->addChild<Row>();
    files->setSpacing(12); files->setLayoutHints({.fillWidth = true});
    m_file = files->addChild<TextField>();
    m_file->setPlaceholder(tr("Select an EXE or MSI file"));
    m_file->setLayoutHints({.fillWidth = true});
    auto *browse = files->addChild<Button>(tr("Browse…"));
    m_buttons[Action::Browse] = browse;
    auto *run = install->addChild<Button>(tr("Install / Run"));
    m_buttons[Action::Run] = run;

    auto *applications = columns[1];
    m_apps = applications->addChild<ComboBox>();
    m_apps->setLayoutHints({.fillWidth = true});
    auto *launchButton = applications->addChild<Button>(tr("Launch"));
    m_buttons[Action::Launch] = launchButton;
    auto *shortcutHint = applications->addChild<Label>(tr("After installation, select the installed EXE on the Install / Run page, then name it here."));
    shortcutHint->setWrap(true); shortcutHint->setLayoutHints({.fillWidth = true});
    m_name = applications->addChild<TextField>();
    m_name->setPlaceholder(tr("Application name"));
    m_name->setLayoutHints({.fillWidth = true});
    auto *shortcut = applications->addChild<Button>(tr("Add to launcher"));
    m_buttons[Action::Register] = shortcut;

    auto *environments = columns[2];
    auto *environmentHint = environments->addChild<Label>(tr("Create an environment for each application, or reuse one for related programs."));
    environmentHint->setWrap(true); environmentHint->setLayoutHints({.fillWidth = true});
    auto *environmentName = environments->addChild<TextField>();
    m_environmentName = environmentName;
    environmentName->setName("environment-name");
    environmentName->setPlaceholder(tr("Environment name"));
    environmentName->setLayoutHints({.fillWidth = true});
    auto *create = environments->addChild<Button>(tr("Create environment"));
    m_buttons[Action::Create] = create;
    for (const auto &[label, command] : std::vector<std::pair<std::string, std::string>>{
             {tr("Wine settings"), "configure"}, {tr("Uninstall programs"), "uninstall"}}) {
        auto *button = environments->addChild<Button>(label);
        m_buttons[command == "configure" ? Action::Configure : Action::Uninstall] = button;
    }
    auto *stop = environments->addChild<Button>(tr("Stop environment"));
    m_buttons[Action::Stop] = stop;

    // Logs occupy their own page; changing pages never rebuilds the task or
    // its text buffer. The page viewport sizes this area on every layout.
    m_log = columns[3]->addChild<TextArea>();
    m_log->setReadOnly(true);
    m_log->setLayoutHints({.fillWidth = true, .preferredHeight = 380});
    auto *logs = columns[3]->addChild<Button>(tr("Open logs"));
    m_buttons[Action::Logs] = logs;
    m_window.setContentView(std::move(root));
    m_window.onCloseRequested = [this] {
        if (!canPerform(Action::Quit))
            m_status->setText(tr("Close Windows applications or stop the environment before closing this window."));
        else trigger(Action::Quit);
    };
    for (const auto &[action, button] : m_buttons)
        button->onClicked = [this, action] { trigger(action); };
    for (auto *field : {m_file, m_name, m_environmentName})
        field->onTextChanged = [this] { updateActions(); };
    m_file->setName("application-path");
    m_name->setName("application-name");
    m_environments->onActivated = [this](int) { updateActions(); };
    m_apps->onActivated = [this](int) { updateActions(); };
    m_window.addShortcut(ControlModifier, 'o', [this] { trigger(Action::Browse); });
    m_window.addShortcut(ControlModifier, 'q', [this] { trigger(Action::Quit); });
    m_window.addShortcut(ControlModifier, 'l', [this] { showPage(3); });
    m_globalMenu = std::make_unique<Wine::GlobalMenu>(Application::instance()->sessionBus(),
        [this] { return menuEntries(); }, [this] { updateActions(); });
    m_menuAvailability = m_globalMenu->availabilityChanged.connect([this](bool available) {
        if (available && m_menuButton->hasFocus()) m_environments->setFocus();
        if (available && m_localMenu) m_localMenu->close();
        m_menuButton->setVisible(!available);
    });
    m_menuButton->setVisible(!m_globalMenu->isAvailable());
    showPage(0);
    if (load) refresh();
}

void Manager::showPage(int page)
{
    if (page < 0 || size_t(page) >= m_pages.size()) return;
    const std::vector<std::string> titles{tr("Install / Run"), tr("Registered applications"),
                                           tr("Environments"), tr("Logs")};
    for (size_t i = 0; i < m_pages.size(); ++i) {
        m_pages[i]->setVisible(int(i) == page);
        auto *nav = static_cast<NavigationButton *>(m_navigation[i]);
        nav->current = int(i) == page;
        nav->requestPaint();
    }
    m_page = page;
    m_pageTitle->setText(titles[page]);
    updateActions();
    m_pageHost->requestLayout();
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
    m_metadataLoading = true;
    updateActions();
    const std::string selected = preferred.empty() ? environment() : preferred;
    m_read.onFinished = [this, selected](int code, bool) {
        m_metadataLoading = false;
        auto data = base::json::Value::parse(m_read.standardOutput());
        if (code || !data) {
            m_status->setText(tr("Could not read Wine environments."));
            append(m_read.standardError()); updateActions(); return;
        }
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
        m_wineAvailable = (*data)["wineAvailable"].toBool();
        updateActions();
        if (!m_wineAvailable)
            m_status->setText(tr("Wine is not installed. Install the Wine runtime packages to continue."));
    };
    if (!m_read.start("/usr/bin/python3", {WINE_HELPER, "list"})) {
        m_metadataLoading = false; updateActions();
        m_status->setText(tr("Could not start the Wine helper."));
    }
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
        updateActions();
        m_status->setText(code || crashed ? tr("Operation failed. See the details and logs.") : tr("Finished"));
        // Follow-ups only refresh the separate metadata process.
        if (!code && !crashed && done) done();
    };
    arguments.insert(arguments.begin(), WINE_HELPER);
    if (!m_task.start("/usr/bin/python3", arguments)) {
        m_runningEnvironment.clear(); m_status->setText(tr("Could not start the Wine helper."));
    }
    updateActions();
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
    showPage(0);
}
void Manager::launch(const std::string &id)
{
    if (m_task.isRunning()) return;
    const auto it = std::find(m_appIds.begin(), m_appIds.end(), id);
    if (it == m_appIds.end()) return;
    m_runningEnvironment = m_appEnvironments[size_t(it - m_appIds.begin())];
    perform({"launch", id});
}

Manager::~Manager()
{
    // Cancel callbacks while every widget still exists. Process destructors
    // otherwise notify a manager after later-declared fields were destroyed.
    m_menuAvailability.reset();
    m_globalMenu.reset();
    m_localMenu.reset();
    for (auto *process : {&m_task, &m_read, &m_stop}) {
        process->onFinished = {};
        process->onStandardOutput = {};
        process->onStandardError = {};
    }
}

bool Manager::canPerform(Action action) const
{
    const bool idle = !m_task.isRunning() && !m_stop.isRunning() && !m_metadataLoading;
    const auto validName = [](const std::string &name) {
        return !name.empty() && name.size() <= 160
            && name.find_first_not_of(" \t\r\n") != std::string::npos
            && std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; });
    };
    // Don't stat a user-supplied path on every keypress on the UI thread.
    // The helper validates actual file existence before running Wine.
    auto suffix = std::filesystem::path(m_file->text()).extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const bool selected = !environment().empty();
    switch (action) {
    case Action::Browse: return idle;
    case Action::Run: return idle && m_wineAvailable && selected && (suffix == ".exe" || suffix == ".msi");
    case Action::Register: return idle && selected && suffix == ".exe" && validName(m_name->text());
    case Action::Create: return idle && validName(m_environmentName->text());
    case Action::Configure:
    case Action::Uninstall: return idle && m_wineAvailable && selected;
    case Action::Stop: return !m_stop.isRunning() && m_wineAvailable && (!m_runningEnvironment.empty() || selected);
    case Action::Launch: return idle && m_wineAvailable && m_apps->currentIndex() >= 0
            && size_t(m_apps->currentIndex()) < m_appIds.size();
    case Action::Logs: return true;
    case Action::Quit: return !m_task.isRunning() && !m_stop.isRunning();
    }
    return false;
}

void Manager::trigger(Action action)
{
    // Menus and shortcuts recheck the same state as buttons, including when
    // a menu event was queued before a task started.
    if (!canPerform(action)) return;
    switch (action) {
    case Action::Browse: choose(); break;
    case Action::Run: runFile("run"); break;
    case Action::Configure: runFile("configure"); break;
    case Action::Uninstall: runFile("uninstall"); break;
    case Action::Register:
        perform({"register", environment(), m_name->text(), m_file->text()}, [this] { refresh(); }); break;
    case Action::Create:
        perform({"create", m_environmentName->text()}, [this] {
            const auto created = base::json::Value::parse(m_task.standardOutput());
            refresh(created ? (*created)["id"].toString() : std::string());
        }); break;
    case Action::Launch: launch(m_appIds[m_apps->currentIndex()]); break;
    case Action::Stop: {
        const std::string id = m_runningEnvironment.empty() ? environment() : m_runningEnvironment;
        m_stop.onFinished = [this](int code, bool) {
            updateActions();
            m_status->setText(code ? tr("Could not stop the environment.") : tr("Stop requested."));
        };
        if (!m_stop.start("/usr/bin/python3", {WINE_HELPER, "stop", id}))
            m_status->setText(tr("Could not start the Wine helper."));
        updateActions(); break;
    }
    case Action::Logs: {
        const char *xdg = std::getenv("XDG_DATA_HOME");
        const std::string data = xdg && *xdg ? xdg : base::paths::dataHome();
        base::Process::startDetached("xdg-open", {data + "/lingmo/wine/logs"}); break;
    }
    case Action::Quit: Application::instance()->quit(); break;
    }
}

void Manager::updateActions()
{
    if (m_updatingActions) return;
    m_updatingActions = true;
    for (const auto &[action, button] : m_buttons) button->setEnabled(canPerform(action));
    if (m_globalMenu) m_globalMenu->update();
    m_updatingActions = false;
}

std::vector<Wine::GlobalMenu::Entry> Manager::menuEntries()
{
    using Entry = Wine::GlobalMenu::Entry;
    const auto item = [this](const std::string &label, Action action, std::vector<std::string> shortcut = {}) {
        Entry entry; entry.label = label; entry.enabled = canPerform(action);
        entry.action = [this, action] { trigger(action); }; entry.shortcut = std::move(shortcut);
        return entry;
    };
    Entry file; file.label = tr("File");
    file.children = {item(tr("Browse…"), Action::Browse, {"Control", "o"}),
                     item(tr("Install / Run"), Action::Run), item(tr("Add to launcher"), Action::Register),
                     item(tr("Quit"), Action::Quit, {"Control", "q"})};
    Entry environmentMenu; environmentMenu.label = tr("Environment");
    environmentMenu.children = {item(tr("Create environment"), Action::Create), item(tr("Wine settings"), Action::Configure),
        item(tr("Uninstall programs"), Action::Uninstall), item(tr("Stop environment"), Action::Stop)};
    Entry view; view.label = tr("View");
    const std::vector<std::string> titles{tr("Install / Run"), tr("Registered applications"), tr("Environments"), tr("Logs")};
    for (int i = 0; i < 4; ++i) {
        Entry page; page.label = titles[i]; page.checkable = true; page.checked = m_page == i;
        page.action = [this, i] { showPage(i); }; view.children.push_back(std::move(page));
    }
    Entry application; application.label = tr("Applications");
    application.children = {item(tr("Launch"), Action::Launch), item(tr("Open logs"), Action::Logs)};
    return {file, environmentMenu, view, application};
}

void Manager::openMenu()
{
    if (m_globalMenu && m_globalMenu->isAvailable()) return;
    if (m_localMenu && m_localMenu->isOpen()) return;
    m_localMenu = std::make_unique<Menu>();
    for (const auto &top : menuEntries()) {
        auto *menu = m_localMenu->addSubmenu(top.label);
        for (const auto &entry : top.children) {
            const size_t row = menu->addItem(entry.label, entry.action);
            menu->setItemEnabled(row, entry.enabled);
            if (entry.checkable) menu->setItemChecked(row, entry.checked);
        }
    }
    m_localMenu->popupFor(m_menuButton);
}
