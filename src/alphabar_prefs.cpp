#include "pch.h"
#include "alphabar_settings.h"
#include "resource.h"

#include <foobar2000/SDK/coreDarkMode.h>

namespace {

// {B4E9D27A-1C63-4F58-9A0E-7D5B3F82C146}
const GUID guid_cfg_show_empty_letters
    = {0xb4e9d27a, 0x1c63, 0x4f58, {0x9a, 0x0e, 0x7d, 0x5b, 0x3f, 0x82, 0xc1, 0x46}};

// {E27C4B90-5D13-4A6F-8C21-B9F03D6A7E58}
const GUID guid_cfg_proportional_letters
    = {0xe27c4b90, 0x5d13, 0x4a6f, {0x8c, 0x21, 0xb9, 0xf0, 0x3d, 0x6a, 0x7e, 0x58}};

// {5C1F8A34-96D2-4B07-B3E8-2A4D9C60E7F1}
const GUID guid_preferences_page
    = {0x5c1f8a34, 0x96d2, 0x4b07, {0xb3, 0xe8, 0x2a, 0x4d, 0x9c, 0x60, 0xe7, 0xf1}};

constexpr bool default_show_empty_letters = true;
constexpr bool default_proportional_letters = false;

cfg_bool cfg_show_empty_letters(guid_cfg_show_empty_letters, default_show_empty_letters);
cfg_bool cfg_proportional_letters(guid_cfg_proportional_letters, default_proportional_letters);

// Proportional sizing hides empty letters, so their choice does not apply.
constexpr int empty_letter_controls[]
    = {IDC_EMPTY_LABEL, IDC_EMPTY_SHOW, IDC_EMPTY_SHOW_NOTE, IDC_EMPTY_HIDE, IDC_EMPTY_HIDE_NOTE};

// The dialog holds the pending state until apply().
class alphabar_preferences : public preferences_page_instance {
public:
    explicit alphabar_preferences(preferences_page_callback::ptr callback)
        : m_callback(callback)
    {
    }

    // Not in the constructor: the dialog must not run on a half-built service.
    void create_window(HWND parent)
    {
        CreateDialogParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_PREFERENCES), parent,
            dialog_proc, reinterpret_cast<LPARAM>(this));
    }

    // preferences_page_instance
    fb2k::hwnd_t get_wnd() override { return m_wnd; }

    uint32_t get_state() override
    {
        uint32_t state = preferences_state::resettable | preferences_state::dark_mode_supported;
        if (dialog_show_empty() != cfg_show_empty_letters.get()
            || dialog_proportional() != cfg_proportional_letters.get())
            state |= preferences_state::changed;
        return state;
    }

    void apply() override
    {
        cfg_show_empty_letters = dialog_show_empty();
        cfg_proportional_letters = dialog_proportional();
        alphabar::notify_settings_changed();
        m_callback->on_state_changed();
    }

    void reset() override
    {
        set_dialog_values(default_show_empty_letters, default_proportional_letters);
        m_callback->on_state_changed();
    }

private:
    bool dialog_show_empty() const
    {
        return m_wnd == nullptr || IsDlgButtonChecked(m_wnd, IDC_EMPTY_SHOW) == BST_CHECKED;
    }

    bool dialog_proportional() const
    {
        return m_wnd != nullptr && IsDlgButtonChecked(m_wnd, IDC_PROPORTIONAL) == BST_CHECKED;
    }

    void set_dialog_values(bool show_empty, bool proportional)
    {
        CheckRadioButton(m_wnd, IDC_EMPTY_SHOW, IDC_EMPTY_HIDE,
            show_empty ? IDC_EMPTY_SHOW : IDC_EMPTY_HIDE);
        CheckDlgButton(m_wnd, IDC_PROPORTIONAL, proportional ? BST_CHECKED : BST_UNCHECKED);
        update_enabled();
    }

    void update_enabled()
    {
        const BOOL enable = dialog_proportional() ? FALSE : TRUE;
        for (const int id : empty_letter_controls)
            EnableWindow(GetDlgItem(m_wnd, id), enable);
    }

    static INT_PTR CALLBACK dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<alphabar_preferences*>(GetWindowLongPtr(wnd, DWLP_USER));

        switch (msg) {
        case WM_INITDIALOG:
            self = reinterpret_cast<alphabar_preferences*>(lp);
            SetWindowLongPtr(wnd, DWLP_USER, lp);
            self->m_wnd = wnd;
            self->m_dark.AddDialogWithControls(wnd);
            self->set_dialog_values(cfg_show_empty_letters, cfg_proportional_letters);
            return TRUE;

        case WM_COMMAND:
            if (self != nullptr && HIWORD(wp) == BN_CLICKED
                && (LOWORD(wp) == IDC_EMPTY_SHOW || LOWORD(wp) == IDC_EMPTY_HIDE
                    || LOWORD(wp) == IDC_PROPORTIONAL)) {
                self->update_enabled();
                self->m_callback->on_state_changed();
            }
            return FALSE;

        case WM_NCDESTROY:
            if (self != nullptr)
                self->m_wnd = nullptr;
            SetWindowLongPtr(wnd, DWLP_USER, 0);
            return FALSE;

        default:
            return FALSE;
        }
    }

    const preferences_page_callback::ptr m_callback;
    HWND m_wnd = nullptr;
    fb2k::CCoreDarkModeHooks m_dark;
};

class alphabar_preferences_page : public preferences_page_v3 {
public:
    const char* get_name() override { return "Alphabar"; }
    GUID get_guid() override { return guid_preferences_page; }
    GUID get_parent_guid() override { return guid_display; }

    preferences_page_instance::ptr instantiate(
        fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override
    {
        auto instance = fb2k::service_new<alphabar_preferences>(callback);
        instance->create_window(parent);
        return instance;
    }
};

preferences_page_factory_t<alphabar_preferences_page> g_alphabar_preferences_page_factory;

} // namespace

bool alphabar::show_empty_letters()
{
    return cfg_show_empty_letters;
}

bool alphabar::proportional_letters()
{
    return cfg_proportional_letters;
}
