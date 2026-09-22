#pragma once

#include "pch.h"

// '#' then A-Z.
inline constexpr int k_bucket_count = 27;
inline constexpr int k_bucket_other = 0;

class alphabar_playlist_watcher;

// Assumes the active playlist is sorted by %album artist%.
class alphabar_panel : public uie::container_uie_window_v3 {
public:
    // uie::extension_base
    const GUID& get_extension_guid() const override;
    void get_name(pfc::string_base& out) const override;

    // uie::window
    void get_category(pfc::string_base& out) const override;
    unsigned get_type() const override;

    // uie::container_uie_window_v3
    uie::container_window_v3_config get_window_config() override;
    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) override;

    void schedule_rebuild();
    void set_active_from_item(size_t item_index);

    void on_settings_changed();

private:
    void on_create(HWND wnd);
    void on_destroy(HWND wnd);
    void on_paint(HWND wnd);
    void on_mouse_move(HWND wnd, int x, int y);
    void on_click(HWND wnd, int y);

    void rebuild_map();
    void rebuild_slots();
    void jump_to_bucket(int bucket);

    int bucket_at(int y) const;
    void update_layout(HWND wnd);
    void set_hover(HWND wnd, int bucket);
    void set_active(HWND wnd, int bucket);

    HWND find_playlist_window() const;
    void attach_playlist(HWND list);
    void detach_playlist();
    static LRESULT CALLBACK playlist_subclass_proc(
        HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
    void calibrate_scroll_anchors();
    int bucket_at_scroll_pos(int pos) const;
    void sync_active_to_scroll();

    HWND m_playlist = nullptr;
    // -1: empty or not measured.
    std::array<int, k_bucket_count> m_bucket_scroll{};
    int m_last_scroll_pos = -1;
    bool m_calibrating = false;

    // SIZE_MAX: empty.
    std::array<size_t, k_bucket_count> m_first_index{};
    std::array<size_t, k_bucket_count> m_bucket_items{};
    std::array<int, k_bucket_count> m_slots{};
    int m_slot_count = 0;
    // Slot i spans [m_slot_top[i], m_slot_top[i + 1]).
    std::array<int, k_bucket_count + 1> m_slot_top{};
    // Height of the band the letter is centred in, from the slot top; 0: the whole slot.
    int m_label_height = 0;
    pfc::array_t<uint8_t> m_item_bucket;

    int m_active = -1;
    int m_hover = -1;

    HFONT m_font = nullptr;
    int m_font_height = 0;
    // Letters are centred on their capitals, not on the font's line box.
    int m_cap_height = 0;

    alphabar_playlist_watcher* m_watcher = nullptr;
};
