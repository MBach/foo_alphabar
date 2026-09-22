#include "pch.h"
#include "alphabar_panel.h"
#include "alphabar_settings.h"

namespace {

// {859BBFC0-1E8A-433B-9F25-09CC70C1AAB3}
const GUID guid_alphabar_panel
    = {0x859bbfc0, 0x1e8a, 0x433b, {0x9f, 0x25, 0x09, 0xcc, 0x70, 0xc1, 0xaa, 0xb3}};

constexpr size_t npos = SIZE_MAX;

// Coalesces the callbacks of bulk edits.
constexpr UINT_PTR timer_rebuild = 1;
constexpr UINT rebuild_delay_ms = 100;

bool get_scroll(HWND list, SCROLLINFO& out)
{
    out = {};
    out.cbSize = sizeof(out);
    out.fMask = SIF_ALL;
    return list != nullptr && GetScrollInfo(list, SB_VERT, &out) != FALSE;
}

// Positions overflow WM_VSCROLL's 16-bit high word, so pass them via nTrackPos.
void set_scroll(HWND list, int pos)
{
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS | SIF_TRACKPOS;
    si.nPos = pos;
    si.nTrackPos = pos;
    SetScrollInfo(list, SB_VERT, &si, TRUE);
    SendMessage(list, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, pos & 0xFFFF), 0);
}

wchar_t bucket_label(int bucket)
{
    return bucket == k_bucket_other ? L'#' : static_cast<wchar_t>(L'A' + bucket - 1);
}

// Windows word sort, which foobar2000 sorts with, all but ignores hyphens,
// dashes and apostrophes: "-M-" sorts among the Ms. Ask the collation rather
// than list them: an ignored symbol cannot push "B" outside "A".."C".
bool ignored_by_sort(wchar_t c)
{
    if (c == L' ' || c == L'\t')
        return true;
    if (IsCharAlphaNumericW(c))
        return false;

    const wchar_t probe[] = {c, L'B'};
    return CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, probe, 2, L"A", 1) == CSTR_GREATER_THAN
        && CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, probe, 2, L"C", 1) == CSTR_LESS_THAN;
}

// Folds accents (E-acute -> E). Keeps leading articles, to match a raw sort.
int bucket_from_name(const char* utf8)
{
    if (utf8 == nullptr)
        return k_bucket_other;

    unsigned code_point = 0;
    for (;;) {
        const size_t length = pfc::utf8_decode_char(utf8, code_point);
        if (length == 0 || code_point == 0 || code_point > 0xFFFF)
            return k_bucket_other;
        if (!ignored_by_sort(static_cast<wchar_t>(code_point)))
            break;
        utf8 += length;
    }

    wchar_t input = static_cast<wchar_t>(code_point);
    wchar_t decomposed[8] = {};
    const int length = NormalizeString(NormalizationD, &input, 1, decomposed, ARRAYSIZE(decomposed));
    wchar_t base = (length > 0) ? decomposed[0] : input;

    if (base >= L'a' && base <= L'z')
        base -= (L'a' - L'A');

    if (base >= L'A' && base <= L'Z')
        return 1 + (base - L'A');

    return k_bucket_other;
}

// Splits total into count cells proportional to weights, none under min_cell.
// Expects count * min_cell <= total. A cell whose share falls short is pinned
// to min_cell, which leaves less for the rest: repeat until none falls short.
void distribute_heights(const size_t* weights, int* cells, int count, int total, int min_cell)
{
    std::array<bool, k_bucket_count> pinned{};
    int free_height = total;
    uint64_t free_weight = 0;
    for (int i = 0; i < count; ++i)
        free_weight += weights[i];

    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < count; ++i) {
            if (pinned[i])
                continue;
            if (free_weight == 0
                || static_cast<uint64_t>(free_height) * weights[i]
                    < static_cast<uint64_t>(min_cell) * free_weight) {
                pinned[i] = true;
                cells[i] = min_cell;
                free_height -= min_cell;
                free_weight -= weights[i];
                changed = true;
            }
        }
    }

    if (free_weight == 0)
        return;

    // Floor the shares, then hand the leftover pixels to the largest remainders.
    std::array<uint64_t, k_bucket_count> remainder{};
    int used = 0;
    for (int i = 0; i < count; ++i) {
        if (pinned[i])
            continue;
        const uint64_t scaled = static_cast<uint64_t>(free_height) * weights[i];
        cells[i] = static_cast<int>(scaled / free_weight);
        remainder[i] = scaled % free_weight;
        used += cells[i];
    }
    for (int left = free_height - used; left > 0; --left) {
        int best = -1;
        for (int i = 0; i < count; ++i) {
            if (!pinned[i] && (best < 0 || remainder[i] > remainder[best]))
                best = i;
        }
        ++cells[best];
        remainder[best] = 0;
    }
}

// Main thread only.
std::vector<alphabar_panel*> g_panels;

// titleformat objects are not thread-safe: one per worker.
struct scan_thread_context {
    titleformat_object::ptr script;
    pfc::string8_fastalloc formatted;

    scan_thread_context() { titleformat_compiler::get()->compile_force(script, "%album artist%"); }
};

} // namespace

class alphabar_playlist_watcher : public playlist_callback_single_impl_base {
public:
    explicit alphabar_playlist_watcher(alphabar_panel* owner)
        : playlist_callback_single_impl_base(flag_on_items_added | flag_on_items_removed
              | flag_on_items_reordered | flag_on_items_replaced | flag_on_items_modified
              | flag_on_playlist_switch | flag_on_item_focus_change)
        , m_owner(owner)
    {
    }

    void on_items_added(size_t, metadb_handle_list_cref, const bit_array&) override
    {
        m_owner->schedule_rebuild();
    }
    void on_items_removed(const bit_array&, size_t, size_t) override { m_owner->schedule_rebuild(); }
    void on_items_reordered(const size_t*, size_t) override { m_owner->schedule_rebuild(); }
    void on_items_replaced(const bit_array&,
        const pfc::list_base_const_t<playlist_callback::t_on_items_replaced_entry>&) override
    {
        m_owner->schedule_rebuild();
    }
    void on_items_modified(const bit_array&) override { m_owner->schedule_rebuild(); }
    void on_playlist_switch() override { m_owner->schedule_rebuild(); }
    void on_item_focus_change(size_t, size_t to) override { m_owner->set_active_from_item(to); }

private:
    alphabar_panel* m_owner;
};

const GUID& alphabar_panel::get_extension_guid() const
{
    return guid_alphabar_panel;
}

void alphabar_panel::get_name(pfc::string_base& out) const
{
    out = "Alphabar";
}

void alphabar_panel::get_category(pfc::string_base& out) const
{
    out = "Panels";
}

unsigned alphabar_panel::get_type() const
{
    return uie::type_panel;
}

uie::container_window_v3_config alphabar_panel::get_window_config()
{
    return uie::container_window_v3_config{L"foo_alphabar_panel", false, CS_VREDRAW | CS_HREDRAW};
}

LRESULT alphabar_panel::on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        on_create(wnd);
        return 0;

    case WM_DESTROY:
        on_destroy(wnd);
        return 0;

    case WM_PAINT:
        on_paint(wnd);
        return 0;

    case WM_SIZE:
        update_layout(wnd);
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE:
        on_mouse_move(wnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;

    case WM_MOUSELEAVE:
        set_hover(wnd, -1);
        return 0;

    case WM_LBUTTONDOWN:
        on_click(wnd, GET_Y_LPARAM(lp));
        return 0;

    case WM_TIMER:
        if (wp == timer_rebuild) {
            KillTimer(wnd, timer_rebuild);
            rebuild_map();
            calibrate_scroll_anchors();
            update_layout(wnd);
            InvalidateRect(wnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        InvalidateRect(wnd, nullptr, FALSE);
        break;

    default:
        break;
    }

    return DefWindowProc(wnd, msg, wp, lp);
}

void alphabar_panel::on_create(HWND wnd)
{
    m_first_index.fill(npos);
    m_watcher = new alphabar_playlist_watcher(this);
    g_panels.push_back(this);

    rebuild_map();
    update_layout(wnd);

    // The playlist view may not exist yet; measure on the timer.
    schedule_rebuild();

    set_active_from_item(playlist_manager::get()->activeplaylist_get_focus_item());
}

void alphabar_panel::on_destroy(HWND wnd)
{
    KillTimer(wnd, timer_rebuild);
    detach_playlist();

    g_panels.erase(std::remove(g_panels.begin(), g_panels.end(), this), g_panels.end());

    delete m_watcher;
    m_watcher = nullptr;

    if (m_font != nullptr) {
        DeleteObject(m_font);
        m_font = nullptr;
    }

    m_item_bucket.set_size(0);
}

void alphabar_panel::schedule_rebuild()
{
    const HWND wnd = get_wnd();
    if (wnd != nullptr)
        SetTimer(wnd, timer_rebuild, rebuild_delay_ms, nullptr);
}

void alphabar_panel::rebuild_map()
{
    m_first_index.fill(npos);
    m_bucket_items.fill(0);
    m_item_bucket.set_size(0);

    metadb_handle_list items;
    playlist_manager::get()->activeplaylist_get_all_items(items);

    const size_t count = items.get_count();
    if (count > 0) {
        m_item_bucket.set_size(count);

        auto metadb = metadb_v2::get();
        metadb->queryMultiParallelEx_<scan_thread_context>(items,
            [&](size_t index, const metadb_v2_rec_t& record, scan_thread_context& context) {
                metadb->formatTitle_v2(
                    items[index], record, nullptr, context.formatted, context.script, nullptr);
                m_item_bucket[index]
                    = static_cast<uint8_t>(bucket_from_name(context.formatted.c_str()));
            });

        for (size_t index = 0; index < count; ++index) {
            const uint8_t bucket = m_item_bucket[index];
            ++m_bucket_items[bucket];
            if (m_first_index[bucket] == npos)
                m_first_index[bucket] = index;
        }
    }

    if (m_active >= 0 && m_first_index[m_active] == npos)
        m_active = -1;

    rebuild_slots();
}

void alphabar_panel::rebuild_slots()
{
    // An empty letter has no proportional height to show.
    const bool show_empty = alphabar::show_empty_letters() && !alphabar::proportional_letters();

    m_slot_count = 0;
    for (int bucket = 0; bucket < k_bucket_count; ++bucket) {
        if (show_empty || m_first_index[bucket] != npos)
            m_slots[m_slot_count++] = bucket;
    }
}

void alphabar_panel::on_settings_changed()
{
    const HWND wnd = get_wnd();
    if (wnd == nullptr)
        return;

    rebuild_slots();
    update_layout(wnd);
    InvalidateRect(wnd, nullptr, FALSE);
}

void alphabar_panel::set_active_from_item(size_t item_index)
{
    const int bucket
        = (item_index < m_item_bucket.get_size()) ? static_cast<int>(m_item_bucket[item_index]) : -1;
    set_active(get_wnd(), bucket);
}

void alphabar_panel::jump_to_bucket(int bucket)
{
    if (bucket < 0 || bucket >= k_bucket_count)
        return;

    const size_t target = m_first_index[bucket];
    if (target == npos)
        return;

    auto api = playlist_manager::get();
    api->activeplaylist_set_focus_item(target);

    // ensure_visible() centres; scroll directly to put the letter at the top.
    if (m_playlist != nullptr && m_bucket_scroll[bucket] >= 0) {
        set_scroll(m_playlist, m_bucket_scroll[bucket]);
        return;
    }

    api->activeplaylist_ensure_visible(target);
}

HWND alphabar_panel::find_playlist_window() const
{
    const HWND self = get_wnd();
    if (self == nullptr)
        return nullptr;

    const HWND root = GetAncestor(self, GA_ROOT);
    if (root == nullptr)
        return nullptr;

    HWND found = nullptr;
    EnumChildWindows(
        root,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t name[16] = {};
            GetClassNameW(child, name, ARRAYSIZE(name));
            if (wcscmp(name, L"NGLV") == 0) {
                *reinterpret_cast<HWND*>(param) = child;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));

    return found;
}

// The playlist view sends no scroll notification, and wheel scrolling raises no
// accessibility event either, so watch its messages. Every visible scroll ends
// in one of these, and sync_active_to_scroll() ignores unchanged positions.
LRESULT CALLBACK alphabar_panel::playlist_subclass_proc(
    HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
    auto* self = reinterpret_cast<alphabar_panel*>(ref);

    if (msg == WM_NCDESTROY) {
        self->detach_playlist();
        return DefSubclassProc(wnd, msg, wp, lp);
    }

    const LRESULT result = DefSubclassProc(wnd, msg, wp, lp);
    if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_PAINT)
        self->sync_active_to_scroll();
    return result;
}

void alphabar_panel::attach_playlist(HWND list)
{
    if (list == m_playlist)
        return;

    detach_playlist();
    if (list != nullptr && SetWindowSubclass(list, playlist_subclass_proc,
            reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
        m_playlist = list;
}

void alphabar_panel::detach_playlist()
{
    if (m_playlist != nullptr)
        RemoveWindowSubclass(m_playlist, playlist_subclass_proc, reinterpret_cast<UINT_PTR>(this));
    m_playlist = nullptr;
    m_last_scroll_pos = -1;
}

void alphabar_panel::calibrate_scroll_anchors()
{
    m_bucket_scroll.fill(-1);
    m_last_scroll_pos = -1;

    attach_playlist(find_playlist_window());

    SCROLLINFO si{};
    if (!get_scroll(m_playlist, si) || si.nPage == 0)
        return;

    // Our own probing scrolls must not move the highlight.
    m_calibrating = true;

    // Scroll units are content pixels and row heights vary, so measure each
    // letter. ensure_visible() centres, hence the half page.
    const int half_page = static_cast<int>(si.nPage) / 2;
    const int max_pos = std::max(si.nMax - static_cast<int>(si.nPage) + 1, 0);
    const int saved = si.nPos;

    SendMessage(m_playlist, WM_SETREDRAW, FALSE, 0);

    int line_height = 0;
    {
        SCROLLINFO after{};
        SendMessage(m_playlist, WM_VSCROLL, SB_LINEDOWN, 0);
        if (get_scroll(m_playlist, after))
            line_height = after.nPos - si.nPos;
        if (line_height <= 0) {
            // At the bottom: measure upwards.
            SendMessage(m_playlist, WM_VSCROLL, SB_LINEUP, 0);
            SCROLLINFO up{};
            if (get_scroll(m_playlist, up))
                line_height = si.nPos - up.nPos;
        }
        line_height = std::max(line_height, 0);
    }

    auto api = playlist_manager::get();
    const size_t item_count = api->activeplaylist_get_item_count();

    // Where an item's row centre sits, in scroll units. Near either end the
    // view clamps instead, and clamp says which end.
    struct reading {
        bool ok = false;
        int clamp = 0;
        int centre = 0;
    };
    auto measure = [&](size_t item) {
        reading r;
        // Start from the far end so ensure_visible() always scrolls, and centres.
        set_scroll(m_playlist, item < item_count / 2 ? max_pos : 0);
        api->activeplaylist_ensure_visible(item);

        SCROLLINFO cur{};
        if (!get_scroll(m_playlist, cur))
            return r;
        r.ok = true;
        r.clamp = (cur.nPos <= si.nMin) ? -1 : (cur.nPos >= max_pos) ? 1 : 0;
        r.centre = cur.nPos + half_page;
        return r;
    };
    auto clamped_anchor = [&](const reading& r) { return r.clamp < 0 ? 0 : max_pos; };

    // Rows from the start of last's group to last, in scroll units; -1: unknown.
    // A group's rows sit exactly a row apart and headers widen the gap, so
    // gallop backwards, then bisect. lowest must start a group.
    auto group_rows_height = [&](size_t last, const reading& last_row, size_t lowest) {
        bool ok = true;
        auto same_group = [&](size_t item) {
            const reading r = measure(item);
            ok = r.ok && r.clamp == 0;
            return ok && last_row.centre - r.centre == static_cast<int>(last - item) * line_height;
        };

        size_t inside = last;
        size_t outside = npos;
        for (size_t step = 1; inside > lowest; step *= 2) {
            const size_t probe = (last - lowest > step) ? last - step : lowest;
            if (same_group(probe)) {
                inside = probe;
            } else {
                if (!ok)
                    return -1;
                outside = probe;
                break;
            }
        }
        while (outside != npos && inside - outside > 1) {
            const size_t middle = outside + (inside - outside) / 2;
            if (same_group(middle))
                inside = middle;
            else if (!ok)
                return -1;
            else
                outside = middle;
        }
        return static_cast<int>(last - inside + 1) * line_height;
    };

    // A letter starts a new artist, so its headers sit between the previous
    // group's bottom and the letter's first row, however many levels they span:
    // a multi-disc album adds a disc header. So anchor on that bottom: below the
    // previous row, plus the padding that stretches a short group (a single,
    // say) to its artwork height A, the same for every group.
    //
    // padding = max(0, A - rows). Take the smallest gap between the two rows:
    // plain headers after an unpadded group. Then gap - smallest + rows equals A
    // after a padded group with plain headers and is no less than A elsewhere,
    // so its minimum is A, and never adds padding after an unpadded group.
    struct boundary {
        reading first_row;
        reading previous_row;
        int gap = -1;
        int rows = -1;
    };
    std::array<boundary, k_bucket_count> boundaries{};
    int header_gap = INT_MAX;
    size_t group_start = 0;
    for (int bucket = 0; bucket < k_bucket_count; ++bucket) {
        const size_t item = m_first_index[bucket];
        if (item == npos)
            continue;
        if (item == 0) {
            m_bucket_scroll[bucket] = 0;
            continue;
        }

        boundary& b = boundaries[bucket];
        b.first_row = measure(item);
        b.previous_row = measure(item - 1);
        if (b.first_row.ok && b.first_row.clamp == 0 && b.previous_row.ok
            && b.previous_row.clamp == 0) {
            b.gap = b.first_row.centre - b.previous_row.centre;
            header_gap = std::min(header_gap, b.gap);
            if (line_height > 0)
                b.rows = group_rows_height(item - 1, b.previous_row, group_start);
        }
        group_start = item;
    }

    int artwork = INT_MAX;
    for (const boundary& b : boundaries) {
        if (b.gap >= 0 && b.rows >= 0)
            artwork = std::min(artwork, b.gap - header_gap + b.rows);
    }

    for (int bucket = 0; bucket < k_bucket_count; ++bucket) {
        const boundary& b = boundaries[bucket];
        if (b.gap >= 0) {
            const int padding = (b.rows >= 0 && artwork != INT_MAX)
                ? std::clamp(artwork - b.rows, 0, b.gap - header_gap)
                : 0;
            m_bucket_scroll[bucket]
                = std::clamp(b.previous_row.centre + line_height / 2 + padding, 0, max_pos);
        } else if (b.first_row.ok && b.first_row.clamp == 0 && header_gap != INT_MAX) {
            // The previous row is clamped: plain headers above the first row.
            m_bucket_scroll[bucket]
                = std::clamp(b.first_row.centre - header_gap + line_height / 2, 0, max_pos);
        } else if (b.previous_row.ok) {
            m_bucket_scroll[bucket] = b.previous_row.clamp != 0
                ? clamped_anchor(b.previous_row)
                : std::clamp(b.previous_row.centre + line_height / 2, 0, max_pos);
        }
    }

    set_scroll(m_playlist, saved);
    SendMessage(m_playlist, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(m_playlist, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    m_calibrating = false;

    // Clamped readings can break monotonicity.
    int previous = -1;
    for (int bucket = 0; bucket < k_bucket_count; ++bucket) {
        if (m_bucket_scroll[bucket] < 0)
            continue;
        if (m_bucket_scroll[bucket] < previous)
            m_bucket_scroll[bucket] = previous;
        previous = m_bucket_scroll[bucket];
    }

    sync_active_to_scroll();
}

void alphabar_panel::sync_active_to_scroll()
{
    SCROLLINFO si{};
    if (m_calibrating || !get_scroll(m_playlist, si) || si.nPos == m_last_scroll_pos)
        return;

    m_last_scroll_pos = si.nPos;
    const int bucket = bucket_at_scroll_pos(si.nPos);
    if (bucket >= 0)
        set_active(get_wnd(), bucket);
}

int alphabar_panel::bucket_at_scroll_pos(int pos) const
{
    int result = -1;
    for (int bucket = 0; bucket < k_bucket_count; ++bucket) {
        if (m_bucket_scroll[bucket] < 0)
            continue;
        if (m_bucket_scroll[bucket] <= pos)
            result = bucket;
        else
            break;
    }
    return result;
}

int alphabar_panel::bucket_at(int y) const
{
    for (int slot = 0; slot < m_slot_count; ++slot) {
        if (y >= m_slot_top[slot] && y < m_slot_top[slot + 1])
            return m_slots[slot];
    }
    return -1;
}

void alphabar_panel::update_layout(HWND wnd)
{
    RECT client{};
    GetClientRect(wnd, &client);

    const int width = client.right - client.left;
    const int panel_height = client.bottom - client.top;
    const int uniform_cell = std::max(panel_height / std::max(m_slot_count, 1), 1);

    // Proportional cells need spare height to share: the smallest cells take at
    // most three quarters of a uniform one, leaving at least a quarter of the
    // panel to share by track count.
    const bool proportional = alphabar::proportional_letters();
    const int fit = proportional ? uniform_cell * 3 / 4 - 3 : uniform_cell - 1;
    const int height = std::clamp(std::min(fit, width - 2), 7, 22);
    const int min_cell = height + 3;

    m_label_height = 0;
    if (proportional && m_slot_count > 0 && min_cell * m_slot_count <= panel_height) {
        std::array<size_t, k_bucket_count> weights{};
        std::array<int, k_bucket_count> cells{};
        for (int slot = 0; slot < m_slot_count; ++slot)
            weights[slot] = m_bucket_items[m_slots[slot]];
        distribute_heights(weights.data(), cells.data(), m_slot_count, panel_height, min_cell);

        m_slot_top[0] = 0;
        for (int slot = 0; slot < m_slot_count; ++slot)
            m_slot_top[slot + 1] = m_slot_top[slot] + cells[slot];
        // At the top of its cell, where the letter starts in the playlist.
        m_label_height = min_cell;
    } else {
        // Uniform: the option is off, or the panel is too short for the minimum.
        const int top_offset = std::max((panel_height - uniform_cell * m_slot_count) / 2, 0);
        for (int slot = 0; slot <= m_slot_count; ++slot)
            m_slot_top[slot] = top_offset + slot * uniform_cell;
    }

    if (height == m_font_height && m_font != nullptr)
        return;

    if (m_font != nullptr)
        DeleteObject(m_font);

    m_font_height = height;
    m_font = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");

    // The ascent leaves room for accents above the capitals, so centring the
    // line box puts them low.
    m_cap_height = height * 7 / 10;
    const HDC dc = GetDC(wnd);
    if (dc != nullptr) {
        // otmsCapEmHeight is unsupported: measure the ink of a capital instead.
        const HFONT old_font = static_cast<HFONT>(SelectObject(dc, m_font));
        const MAT2 identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
        GLYPHMETRICS glyph{};
        if (GetGlyphOutlineW(dc, L'H', GGO_METRICS, &glyph, 0, nullptr, &identity) != GDI_ERROR
            && glyph.gmptGlyphOrigin.y > 0)
            m_cap_height = glyph.gmptGlyphOrigin.y;
        SelectObject(dc, old_font);
        ReleaseDC(wnd, dc);
    }
}

void alphabar_panel::on_paint(HWND wnd)
{
    PAINTSTRUCT ps{};
    const HDC hdc = BeginPaint(wnd, &ps);

    RECT client{};
    GetClientRect(wnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    const HDC buffer_dc = CreateCompatibleDC(hdc);
    const HBITMAP buffer = CreateCompatibleBitmap(hdc, width, height);
    const HBITMAP old_bitmap = static_cast<HBITMAP>(SelectObject(buffer_dc, buffer));

    const cui::colours::helper colours;
    const COLORREF background = colours.get_colour(cui::colours::colour_background);
    const COLORREF text = colours.get_colour(cui::colours::colour_text);
    const COLORREF selection_background
        = colours.get_colour(cui::colours::colour_selection_background);
    const COLORREF selection_text = colours.get_colour(cui::colours::colour_selection_text);

    const COLORREF dimmed = RGB((GetRValue(text) + GetRValue(background)) / 2,
        (GetGValue(text) + GetGValue(background)) / 2,
        (GetBValue(text) + GetBValue(background)) / 2);
    const COLORREF hover_background
        = RGB((GetRValue(selection_background) + GetRValue(background)) / 2,
            (GetGValue(selection_background) + GetGValue(background)) / 2,
            (GetBValue(selection_background) + GetBValue(background)) / 2);

    const HBRUSH background_brush = CreateSolidBrush(background);
    FillRect(buffer_dc, &client, background_brush);
    DeleteObject(background_brush);

    const HFONT old_font = static_cast<HFONT>(SelectObject(buffer_dc, m_font));
    SetBkMode(buffer_dc, TRANSPARENT);
    SetTextAlign(buffer_dc, TA_BASELINE | TA_CENTER);

    for (int slot = 0; slot < m_slot_count; ++slot) {
        const int bucket = m_slots[slot];
        RECT cell{client.left, m_slot_top[slot], client.right, m_slot_top[slot + 1]};

        const bool present = m_first_index[bucket] != npos;

        if (bucket == m_active) {
            const HBRUSH brush = CreateSolidBrush(selection_background);
            FillRect(buffer_dc, &cell, brush);
            DeleteObject(brush);
            SetTextColor(buffer_dc, selection_text);
        } else if (bucket == m_hover && present) {
            const HBRUSH brush = CreateSolidBrush(hover_background);
            FillRect(buffer_dc, &cell, brush);
            DeleteObject(brush);
            SetTextColor(buffer_dc, text);
        } else {
            SetTextColor(buffer_dc, present ? text : dimmed);
        }

        const int cell_height = static_cast<int>(cell.bottom - cell.top);
        const int band = (m_label_height > 0) ? std::min(cell_height, m_label_height) : cell_height;
        const int baseline = cell.top + (band + m_cap_height) / 2;

        const wchar_t label[1] = {bucket_label(bucket)};
        TextOutW(buffer_dc, (cell.left + cell.right) / 2, baseline, label, 1);
    }

    SelectObject(buffer_dc, old_font);
    BitBlt(hdc, 0, 0, width, height, buffer_dc, 0, 0, SRCCOPY);

    SelectObject(buffer_dc, old_bitmap);
    DeleteObject(buffer);
    DeleteDC(buffer_dc);

    EndPaint(wnd, &ps);
}

void alphabar_panel::on_mouse_move(HWND wnd, int x, int y)
{
    (void)x;

    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, wnd, 0};
    TrackMouseEvent(&track);

    set_hover(wnd, bucket_at(y));
}

void alphabar_panel::on_click(HWND wnd, int y)
{
    const int bucket = bucket_at(y);
    if (bucket < 0 || m_first_index[bucket] == npos)
        return;

    jump_to_bucket(bucket);
    set_active(wnd, bucket);
}

void alphabar_panel::set_hover(HWND wnd, int bucket)
{
    if (wnd == nullptr || bucket == m_hover)
        return;

    m_hover = bucket;
    InvalidateRect(wnd, nullptr, FALSE);
}

void alphabar_panel::set_active(HWND wnd, int bucket)
{
    if (wnd == nullptr || bucket == m_active)
        return;

    m_active = bucket;
    InvalidateRect(wnd, nullptr, FALSE);
}

void alphabar::notify_settings_changed()
{
    for (alphabar_panel* panel : g_panels)
        panel->on_settings_changed();
}

namespace {
uie::window_factory<alphabar_panel> g_alphabar_factory;
}
