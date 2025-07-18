#include "sizes.h"
#include "column.h"
#include "row.h"

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

extern HANDLE PHANDLE;
extern std::function<SDispatchResult(std::string)> orig_moveFocusTo;
extern ScrollerSizes scroller_sizes;

Column::Column(PHLWINDOW cwindow, const Row *row)
    : reorder(Reorder::Auto), row(row)
{
    width = scroller_sizes.get_column_default_width(cwindow);
    const Box &max = row->get_max();
    Window *window = new Window(cwindow, max.y, max.h, width);
    windows.push_back(window);
    active = windows.first();
    update_width(width, max.w);

    // We know it will be located on the right of row->active
    const Column *col = row->get_active_column();
    if (col != nullptr) {
        geom.x = col->get_geom_x() + col->get_geom_w();
    } else {
        // first window, locate it at the center
        geom.x = max.x + 0.5 * (max.w - geom.w);
    }
}

Column::Column(Window *window, StandardSize width, double maxw, const Row *row)
    : width(width), reorder(Reorder::Auto), row(row)
{
    const Box &max = row->get_max();
    windows.push_back(window);
    active = windows.first();
    update_width(width, maxw);
}

Column::Column(const Row *pRow, const Column *column, List<Window *> &pWindows)
{
    width = column->width;
    reorder = column->reorder;
    geom = column->geom;
    mem = column->mem;
    for (auto win = pWindows.first(); win != nullptr; win = win->next()) {
        windows.push_back(win->data());
    }
    active = windows.first();
    name = column->name;
    row = pRow;
}

Column::~Column()
{
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        delete win->data();
    }
    windows.clear();
}

bool Column::has_window(PHLWINDOW window) const
{
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        if (win->data()->is_window(window))
            return true;
    }
    return false;
}

Window *Column::get_window(PHLWINDOW window) const
{
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        if (win->data()->is_window(window))
            return win->data();
    }
    return nullptr;
}

void Column::add_active_window(PHLWINDOW window)
{
    reorder = Reorder::Auto;
    // Store the default window width internally, regardless of that of the column
    auto wwidth = scroller_sizes.get_column_default_width(window);
    auto w = new Window(window, row->get_max().y, row->get_max().h, wwidth);

    if (row->get_pinned_column() == this)
        w->pin(true);

    ModeModifier modifier = row->get_mode_modifier();
    auto focus = modifier.get_focus();
    auto node = active;
    switch (modifier.get_position()) {
    case ModeModifier::POSITION_AFTER:
    default:
        node = windows.emplace_after(active, w);
        break;
    case ModeModifier::POSITION_BEFORE:
        node = windows.emplace_before(active, w);
        break;
    case ModeModifier::POSITION_END:
        node = windows.emplace_after(windows.last(), w);
        break;
    case ModeModifier::POSITION_BEGINNING:
        node = windows.emplace_before(windows.first(), w);
        break;
    }
    if (focus == ModeModifier::FOCUS_FOCUS)
        active = node;
    else
        window->m_noInitialFocus = true;
}

void Column::remove_window(PHLWINDOW window)
{
    reorder = Reorder::Auto;
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        if (win->data()->is_window(window)) {
            if (active->data()->is_window(window)) {
                // Make next window active (like PaperWM)
                // If it is the last, make the previous one active.
                // If it is the only window. active will point to nullptr,
                // but it doesn't matter because the caller will delete
                // the column.
                active = active != windows.last() ? active->next() : active->prev();
            }
            if (row->get_pinned_column() == this)
                win->data()->pin(false);
            windows.erase(win);
            delete win->data();
            return;
        }
    }
}

void Column::focus_window(PHLWINDOW window)
{
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        if (win->data()->is_window(window)) {
            active = win;
            return;
        }
    }
}

// Recalculates the geometry of the windows in the column
void Column::recalculate_col_geometry(const Vector2D &gap_x, double gap, bool animate)
{
    const Box &max = row->get_max();
    // In theory, every window in the Columm should have the same size,
    // but the standard layouts don't follow this rule (to make the code
    // simpler?). Windows close to the border of the monitor will have
    // their sizes affected by gaps_out vs. gaps_in.
    // I follow the same rules.
    // Each window has a gap to its bounding box of "gaps_in + border",
    // except on the monitor sides, where the gap is "gaps_out + border",
    // but the window sizes are different because of those different
    // gaps. So the distance between two window border boundaries is
    // two times gaps_in (one per window).
    auto gap0 = active == windows.first() ? 0.0 : gap;
    auto a_y0 = std::round(active->data()->get_geom_y(gap0));
    auto a_y1 = std::round(a_y0 + active->data()->get_geom_h());

    if (row->get_mode_modifier().get_center_window().value() && row->get_active_column() == this) {
        double start = max.y + 0.5 * (max.h - (a_y1 - a_y0));
        active->data()->move_to_pos(geom.x, start, gap_x, gap0);
        adjust_windows(active, gap_x, gap, animate);
        return;
    }

    if (a_y0 < max.y) {
        // active starts above, set it on the top edge, unless it is the last one and there are more,
        // then move it to the bottom
        if (active == windows.last() && active->prev() != nullptr) {
            active->data()->move_to_bottom(geom.x, max, gap_x, gap0);
        } else {
            active->data()->move_to_top(geom.x, max, gap_x, gap0);
        }
    } else if (a_y1 > max.y + max.h) {
        // active overflows below the bottom, move to bottom of viewport, unless it is the first window
        // and there are more, then move it to the top
        if (active == windows.first() && active->next() != nullptr) {
            active->data()->move_to_top(geom.x, max, gap_x, gap0);
        } else {
            active->data()->move_to_bottom(geom.x, max, gap_x, gap0);
        }
    } else {
        // active window is inside the viewport
        if (reorder == Reorder::Auto) {
            // The active window should always be completely in the viewport.
            // If any of the windows next to it, above or below are already
            // in the viewport, keep the current position.
            bool keep_current = false;
            if (active->prev() != nullptr) {
                auto gap0 = active->prev() == windows.first() ? 0.0 : gap;
                auto p_y0 = std::round(active->prev()->data()->get_geom_y(gap0));
                auto p_y1 = std::round(p_y0 + active->prev()->data()->get_geom_h());
                if (p_y0 >= max.y && p_y1 <= max.y + max.h) {
                    keep_current = true;
                }
            }
            if (!keep_current && active->next() != nullptr) {
                auto gap0 = active->next() == windows.first() ? 0.0 : gap;
                auto p_y0 = std::round(active->next()->data()->get_geom_y(gap0));
                auto p_y1 = std::round(p_y0 + active->next()->data()->get_geom_h());
                if (p_y0 >= max.y && p_y1 <= max.y + max.h) {
                    keep_current = true;
                }
            }
            if (!keep_current) {
                // If not:
                // We try to fit the window right below it if it fits
                // completely, otherwise the one above it. If none of them fit,
                // we leave it as it is.
                if (active->next() != nullptr) {
                    if (std::round(active->data()->get_geom_h() + active->next()->data()->get_geom_h()) <= max.h) {
                        // set next at the bottom edge of the viewport
                        active->data()->move_to_pos(geom.x, max.y + max.h - active->data()->get_geom_h() - active->next()->data()->get_geom_h(), gap_x, gap0);
                    } else if (active->prev() != nullptr) {
                        if (std::round(active->prev()->data()->get_geom_h() + active->data()->get_geom_h()) <= max.h) {
                            // set previous at the top edge of the viewport
                            active->data()->move_to_pos(geom.x, max.y + active->prev()->data()->get_geom_h(), gap_x, gap0);
                        } else {
                            // none of them fit, leave active as it is (only modify x)
                            active->data()->set_geom_x(geom.x, gap_x);
                        }
                    } else {
                        // nothing above, move active to top of viewport
                        active->data()->move_to_top(geom.x, max, gap_x, gap0);
                    }
                } else if (active->prev() != nullptr) {
                    if (std::round(active->prev()->data()->get_geom_h() + active->data()->get_geom_h()) <= max.h) {
                        // set previous at the top edge of the viewport
                        active->data()->move_to_pos(geom.x, max.y + active->prev()->data()->get_geom_h(), gap_x, gap0);
                    } else {
                        // it doesn't fit and nothing above, move active to bottom of viewport
                        active->data()->move_to_bottom(geom.x, max, gap_x, gap0);
                    }
                } else {
                    // nothing on the right or left, move to the top
                    active->data()->move_to_top(geom.x, max, gap_x, gap0);
                }
            } else {
                // the window is in a correct position, but
                // if the window is first or last, and some windows don't fit,
                // ensure it is at the edge
                const Vector2D h = get_height();
                if (std::round(h.y - h.x) >= max.h) {
                    if (active == windows.first()) {
                        active->data()->move_to_top(geom.x, max, gap_x, gap0);
                    } else if (active == windows.last()) {
                        active->data()->move_to_bottom(geom.x, max, gap_x, gap0);
                    } else {
                        active->data()->set_geom_x(geom.x, gap_x);
                    }
                } else {
                    active->data()->set_geom_x(geom.x, gap_x);
                }
            }
        } else {
            // the window is in a correct position
            active->data()->set_geom_x(geom.x, gap_x);
        }
    }
    adjust_windows(active, gap_x, gap, animate);
}

// Recalculates the geometry of the windows in the column for overview mode
void Column::recalculate_col_geometry_overview(const Vector2D &gap_x, double gap)
{
    windows.first()->data()->move_to_pos(geom.x, geom.vy, gap_x, 0.0);
    adjust_windows(windows.first(), gap_x, gap, true);
}

void Column::move_active_up()
{
    if (active == windows.first())
        return;

    reorder = Reorder::Auto;
    auto prev = active->prev();
    windows.swap(active, prev);
}

void Column::move_active_down()
{
    if (active == windows.last())
        return;

    reorder = Reorder::Auto;
    auto next = active->next();
    windows.swap(active, next);
}

bool Column::move_focus_up(bool focus_wrap)
{
    if (active == windows.first()) {
        PHLMONITOR monitor = g_pCompositor->getMonitorInDirection('u');
        if (monitor == nullptr) {
            if (focus_wrap) {
                active = windows.last();
                return true;
            } else {
                static auto* const *movefocus_changes_workspace = (Hyprlang::INT* const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:movefocus_changes_workspace")->getDataStaticPtr();
                if (**movefocus_changes_workspace) {
                    g_pKeybindManager->m_dispatchers["workspace"]("m-1");
                }
                return false;
            }
        }
        // use default dispatch for movefocus (change monitor)
        orig_moveFocusTo("u");
        return false;
    }
    reorder = Reorder::Auto;
    active = active->prev();
    return true;
}

bool Column::move_focus_down(bool focus_wrap)
{
    if (active == windows.last()) {
        PHLMONITOR monitor = g_pCompositor->getMonitorInDirection('d');
        if (monitor == nullptr) {
            if (focus_wrap) {
                active = windows.first();
                return true;
            } else {
                static auto* const *movefocus_changes_workspace = (Hyprlang::INT* const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:movefocus_changes_workspace")->getDataStaticPtr();
                if (**movefocus_changes_workspace) {
                    g_pKeybindManager->m_dispatchers["workspace"]("m+1");
                }
                return false;
            }
        }
        // use default dispatch for movefocus (change monitor)
        orig_moveFocusTo("d");
        return false;
    }
    reorder = Reorder::Auto;
    active = active->next();
    return true;
}

void Column::admit_window(Window *window)
{
    reorder = Reorder::Auto;
    active = windows.emplace_after(active, window);
}

Window *Column::expel_active(const Vector2D &gap_x)
{
    reorder = Reorder::Auto;
    Window *window = active->data();
    auto act = active == windows.first() ? active->next() : active->prev();
    windows.erase(active);
    active = act;
    // If only one window is left, take its stored width
    if (windows.size() == 1) {
        double maxw = width == StandardSize::Free ? active->data()->get_geom_w(gap_x) : row->get_max().w;
        update_width(active->data()->get_width(), maxw);
    }
    return window;
}

void Column::align_window(Direction direction, const Vector2D &gap_x, double gap)
{
    const Box &max = row->get_max();
    auto gap0 = active == windows.first() ? 0.0 : gap;
    auto gap1 = active == windows.last() ? 0.0 : gap;
    switch (direction) {
    case Direction::Up:
        reorder = Reorder::Lazy;
        active->data()->move_to_top(geom.x, max, gap_x, gap0);
        break;
    case Direction::Down:
        reorder = Reorder::Lazy;
        active->data()->move_to_bottom(geom.x, max, gap_x, gap0);
        break;
    case Direction::Center:
        reorder = Reorder::Lazy;
        active->data()->move_to_center(geom.x, max, gap_x, gap0, gap1);
        break;
    default:
        break;
    }
}

// Update heights according to new maxh
void Column::update_heights()
{
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        Window *window = win->data();
        window->update_height(window->get_height(), row->get_max().h);
    }
}

void Column::update_width(StandardSize cwidth, double maxw, bool internal_too)
{
    if (maximized()) {
        geom.w = maxw;
    } else {
        switch (cwidth) {
        case StandardSize::OneEighth:
            geom.w = maxw / 8.0;
            break;
        case StandardSize::OneSixth:
            geom.w = maxw / 6.0;
            break;
        case StandardSize::OneFourth:
            geom.w = maxw / 4.0;
            break;
        case StandardSize::OneThird:
            geom.w = maxw / 3.0;
            break;
        case StandardSize::ThreeEighths:
            geom.w = 3.0 * maxw / 8.0;
            break;
        case StandardSize::OneHalf:
            geom.w = maxw / 2.0;
            break;
        case StandardSize::FiveEighths:
            geom.w = 5.0 * maxw / 8.0;
            break;
        case StandardSize::TwoThirds:
            geom.w = 2.0 * maxw / 3.0;
            break;
        case StandardSize::ThreeQuarters:
            geom.w = 3.0 * maxw / 4.0;
            break;
        case StandardSize::FiveSixths:
            geom.w = 5.0 * maxw / 6.0;
            break;
        case StandardSize::SevenEighths:
            geom.w = 7.0 * maxw / 8.0;
            break;
        case StandardSize::One:
            geom.w = maxw;
            break;
        case StandardSize::Free:
            // Only used when creating a column from an expelled window
            geom.w = maxw;
        default:
            break;
        }
    }
    width = cwidth;
    // Update active window's width
    if (internal_too) {
        for (auto w = windows.first(); w != nullptr; w = w->next()) {
            w->data()->set_width(width);
        }
    }
}

void Column::fit_size(FitSize fitsize, const Vector2D &gap_x, double gap)
{
    const Box &max = row->get_max();
    reorder = Reorder::Auto;
    ListNode<Window *> *from, *to;
    switch (fitsize) {
    case FitSize::Active:
        from = to = active;
        break;
    case FitSize::Visible:
        for (auto w = windows.first(); w != nullptr; w = w->next()) {
            auto gap0 = w == windows.first() ? 0.0 : gap;
            auto c0 = std::round(w->data()->get_geom_y(gap0));
            auto c1 = std::round(c0 + w->data()->get_geom_h());
            if ((c0 < max.y + max.h && c0 >= max.y) ||
                (c1 > max.y && c1 <= max.y + max.h) ||
                // should never happen as windows are never taller than the screen
                (c0 < max.y && c1 >= max.y + max.h)) {
                from = w;
                break;
            }
        }
        for (auto w = windows.last(); w != nullptr; w = w->prev()) {
            auto gap0 = w == windows.first() ? 0.0 : gap;
            auto c0 = std::round(w->data()->get_geom_y(gap0));
            auto c1 = std::round(c0 + w->data()->get_geom_h());
            if ((c0 < max.y + max.h && c0 >= max.y) ||
                (c1 > max.y && c1 <= max.y + max.h) ||
                // should never happen as windows are never taller than the screen
                (c0 < max.y && c1 >= max.y + max.h)) {
                to = w;
                break;
            }
        }
        break;
    case FitSize::All:
        from = windows.first();
        to = windows.last();
        break;
    case FitSize::ToEnd:
        from = active;
        to = windows.last();
        break;
    case FitSize::ToBeg:
        from = windows.first();
        to = active;
        break;
    default:
        return;
    }

    // Now align from top of the screen (max.y), split height of
    // screen (max.h) among from->to, and readapt the rest
    if (from != nullptr && to != nullptr) {
        double total = 0.0;
        for (auto c = from; c != to->next(); c = c->next()) {
            total += c->data()->get_geom_h();
        }
        for (auto c = from; c != to->next(); c = c->next()) {
            Window *win = c->data();
            win->set_height_free();
            win->set_geom_h(win->get_geom_h() / total * max.h);
        }
        auto gap0 = from == windows.first() ? 0.0 : gap;
        from->data()->move_to_top(geom.x, max, gap_x, gap0);

        adjust_windows(from, gap_x, gap, true);
    }
}

void Column::cycle_size_active_window(int step, const Vector2D &gap_x, double gap)
{
    reorder = Reorder::Auto;
    StandardSize height = active->data()->get_height();
    if (height == StandardSize::Free) {

        // When cycle-resizing from Free mode, move back to closest or default
        static auto* const *CYCLESIZE_CLOSEST = (Hyprlang::INT* const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:cyclesize_closest")->getDataStaticPtr();
        if (**CYCLESIZE_CLOSEST) {
            double fraction = active->data()->get_geom_h() / row->get_max().h;
            height = scroller_sizes.get_window_closest_height(g_pCompositor->m_lastMonitor, fraction, step);
        } else {
            height = scroller_sizes.get_window_default_height(active->data()->get_window());
        }
    } else {
        height = scroller_sizes.get_next_window_height(height, step);
    }
    active->data()->update_height(height, row->get_max().h);
    recalculate_col_geometry(gap_x, gap, true);
}

void Column::size_active_window(StandardSize height, const Vector2D &gap_x, double gap)
{
    reorder = Reorder::Auto;
    active->data()->update_height(height, row->get_max().h);
    recalculate_col_geometry(gap_x, gap, true);
}

void Column::resize_active_window(const Vector2D &gap_x, double gap, const Vector2D &delta)
{
    const Box &max = row->get_max();
    if (!active->data()->can_resize_width(geom.w, max.w, gap_x, gap, delta.x))
        return;

    if (std::abs(static_cast<int>(delta.y)) > 0) {
        for (auto win = windows.first(); win != nullptr; win = win->next()) {
            auto gap0 = win == windows.first() ? 0.0 : gap;
            auto gap1 = win == windows.last() ? 0.0 : gap;
            if (!win->data()->can_resize_height(max.h, win == active, gap0, gap1, delta.y))
                return;
        }
    }
    reorder = Reorder::Auto;
    // Now, resize.
    if (std::abs(static_cast<int>(delta.x)) > 0) {
        width = StandardSize::Free;
        geom.w += delta.x;
        for (auto win = windows.first(); win != nullptr; win = win->next()) {
            Window *window = win->data();
            window->set_width(StandardSize::Free);
            window->set_geom_w(geom.w, gap_x);
        }
    }
    if (std::abs(static_cast<int>(delta.y)) > 0) {
        for (auto win = windows.first(); win != nullptr; win = win->next()) {
            Window *window = win->data();
            if (win == active) {
                window->set_geom_h(window->get_geom_h() + delta.y);
                window->set_height_free();
            }
        }
    }
}

// Adjust all the windows in the column using 'window' as anchor
void Column::adjust_windows(ListNode<Window *> *win, const Vector2D &gap_x, double gap, bool animate)
{
    // 2. adjust positions of windows above
    for (auto w = win->prev(), p = win; w != nullptr; p = w, w = w->prev()) {
        auto gap0 = w == windows.first() ? 0.0 : gap;
        w->data()->move_to_pos(geom.x, p->data()->get_geom_y(gap) - w->data()->get_geom_h(), gap_x, gap0);
    }
    // 3. adjust positions of windows below
    for (auto w = win->next(), p = win; w != nullptr; p = w, w = w->next()) {
        auto gap0 = p == windows.first() ? 0.0 : gap;
        w->data()->move_to_pos(geom.x, p->data()->get_geom_y(gap0) + p->data()->get_geom_h(), gap_x, gap);
    }
    for (auto w = windows.first(); w != nullptr; w = w->next()) {
        auto gap0 = w == windows.first() ? 0.0 : gap;
        auto gap1 = w == windows.last() ? 0.0 : gap;
        w->data()->update_window(geom.w, gap_x, gap0, gap1, animate);
    }
}

void Column::scroll_update(double delta_y)
{
    for (auto w = windows.first(); w != nullptr; w = w->next()) {
        w->data()->scroll(delta_y);
    }
}

void Column::scroll_end(Direction dir, double gap)
{
    if (dir == Direction::Up) {
        auto newactive = windows.last();
        // Take the first after active that has its left edge in the viewport
        const auto &max = row->get_max();
        for (auto win = active->next(); win != nullptr; win = win->next()) {
            const auto y0 = win->data()->get_geom_y(gap);
            if (y0 > max.y && y0 < max.y + max.h) {
                newactive = win;
                break;
            }
        }
        active = newactive;
    } else if (dir == Direction::Down) {
        // Search on the left
        auto newactive = windows.first();
        // Take the first abefore active that has its right edge in the viewport
        const auto &max = row->get_max();
        for (auto win = active->prev(); win != nullptr; win = win->prev()) {
            auto gap0 = win == windows.first() ? 0.0f : gap;
            const auto y0 = win->data()->get_geom_y(gap0);
            const auto y1 = y0 + win->data()->get_geom_h();
            if (y1 > max.y && y1 < max.y + max.h) {
                newactive = win;
                break;
            }
        }
        active = newactive;
    }
}

void Column::selection_toggle()
{
    active->data()->selection_toggle();
}

void Column::selection_set(PHLWINDOWREF window)
{
    for (auto w = windows.first(); w != nullptr; w = w->next()) {
        if (w->data()->get_window() == window) {
            w->data()->selection_set();
            return;
        }
    }
}

void Column::selection_all()
{
    for (auto w = windows.first(); w != nullptr; w = w->next()) {
        w->data()->selection_set();
    }
}

void Column::selection_reset()
{
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        win->data()->selection_reset();
    }
}

bool Column::selection_exists() const
{
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        if (win->data()->is_selected())
            return true;
    }
    return false;
}

Column *Column::selection_get(const Row *row)
{
    Column *column = nullptr;
    List<Window *> selection;
    ListNode<Window *> *win = windows.first();
    PHLWORKSPACE workspace = g_pCompositor->getWorkspaceByID(row->get_workspace());
    while (win != nullptr) {
        auto next = win->next();
        if (win->data()->is_selected()) {
            win->data()->move_to_workspace(workspace);
            selection.push_back(win->data());
            if (active == win) {
                active = active != windows.last() ? active->next() : active->prev();
            }
            windows.erase(win);
        }
        win = next;
    }
    if (selection.size() > 0) {
        column = new Column(row, this, selection);
    }
    return column;
}

void Column::pin(bool pin) const
{
    for (auto win = windows.first(); win != nullptr; win = win->next()) {
        win->data()->pin(pin);
    }
}

