// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "base/process.h"
#include "base/json.h"
#include "ui/window.h"
#include "ui/file_chooser.h"
#include "ui/combo_box.h"
#include "ui/label.h"
#include "ui/text_field.h"
#include "ui/text_area.h"
#include <functional>

class Manager
{
public:
    explicit Manager(bool load = true);
    void openFile(const std::string &path);
    void launch(const std::string &id);
    Lingmo::ui::Window &window() { return m_window; }
private:
    void refresh(const std::string &preferred = {});
    void perform(std::vector<std::string> arguments, std::function<void()> done = {});
    void runFile(const std::string &operation);
    void choose();
    std::string environment() const;
    void append(std::string_view text);
    Lingmo::ui::Window m_window;
    Lingmo::ui::ComboBox *m_environments;
    Lingmo::ui::ComboBox *m_apps;
    Lingmo::ui::TextField *m_name;
    Lingmo::ui::TextField *m_file;
    Lingmo::ui::Label *m_status;
    Lingmo::ui::TextArea *m_log;
    std::vector<std::string> m_ids;
    std::vector<std::string> m_appIds;
    std::vector<std::string> m_appEnvironments;
    std::string m_runningEnvironment;
    Lingmo::base::Process m_task;
    Lingmo::base::Process m_stop;
    Lingmo::base::Process m_read;
    std::unique_ptr<Lingmo::ui::FileChooser> m_chooser;
};
