#pragma once
// The module bar's keys: Tab and Shift+Tab change the group, < and > (or , and .) move along its modules, Return or
// Enter inserts the focused one. The patch window hands every key to the toolbar first (CPatchView::HandleKeyDownUp
// @000f93d4 -> CTBWindow::RouteKeySpec), so they work while the patch has the keyboard (manual p115-116). Each group
// keeps its own focused module, its first at start (CTabButtonBar+0x50). Plain C++.

#include <utility>
#include <vector>

namespace g2ui {

struct ModuleBarKey {
    bool tab = false;     // the Tab key
    bool insert = false;  // Return or Enter
    char character = 0;   // the character typed, for < > , .
    bool shift = false;
    bool other = false;   // any other modifier: Option, Command, Control
    bool repeat = false;  // the key's auto-repeat
};

class ModuleBarKeys {
public:
    enum class Result { None, Handled, Insert };

    // The number of modules in each group, in tab order.
    explicit ModuleBarKeys(std::vector<int> counts) : counts_(std::move(counts)), focused_(counts_.size(), 0) {}

    int group() const { return group_; }
    int focused(int group) const { return focused_[static_cast<std::size_t>(group)]; }
    void setGroup(int group) { group_ = group; }
    void setFocused(int group, int module) { focused_[static_cast<std::size_t>(group)] = module; }

    Result press(const ModuleBarKey& k)
    {
        const int groups = static_cast<int>(counts_.size());
        if (k.tab) {
            // CTabBarView::RouteKeySpec @0012603c: key down or repeat, no modifier but Shift.
            if (k.other || groups == 0)
                return Result::None;
            group_ = k.shift ? (group_ == 0 ? groups - 1 : group_ - 1) : (group_ + 1 >= groups ? 0 : group_ + 1);
            return Result::Handled;
        }
        const int count = groups > 0 ? counts_[static_cast<std::size_t>(group_)] : 0;
        auto& f = focused_[static_cast<std::size_t>(group_)];
        if (k.insert) {
            // CTabButtonBar::RouteKeySpec @00127238: key down only, the focused button's Action (insert).
            return k.repeat || count == 0 ? Result::None : Result::Insert;
        }
        // The characters, whatever the modifiers; key down or repeat.
        if (k.character == '<' || k.character == ',') {
            if (count > 0)
                f = f == 0 ? count - 1 : f - 1;
            return Result::Handled;
        }
        if (k.character == '>' || k.character == '.') {
            if (count > 0)
                f = f + 1 >= count ? 0 : f + 1;
            return Result::Handled;
        }
        return Result::None;
    }

private:
    std::vector<int> counts_;
    std::vector<int> focused_;
    int group_ = 0;
};

} // namespace g2ui
