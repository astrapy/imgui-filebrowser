#pragma once

#include "imgui.h"

#include <string>
#include <vector>
#include <filesystem>
#include <system_error>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <fstream>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <ctime>
#include <chrono>
#include <cfloat>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <objbase.h>   // CoInitializeEx
  #include <shlobj.h>    // SHGetKnownFolderPath
  #include <shobjidl.h>  // IShellItem / IEnumShellItems (Quick access)
  #include <shellapi.h>  // SHFileOperationW (Recycle Bin)
  #pragma comment(lib, "Shell32.lib")
  #pragma comment(lib, "Ole32.lib")
  #pragma comment(lib, "Uuid.lib")
#endif

namespace imex {

namespace fs = std::filesystem;

class FileExplorer {
public:
    enum class Column   { Name = 0, Date = 1, Type = 2, Size = 3 };
    enum class ViewMode { Details = 0, List = 1, Tiles = 2, Icons = 3 };

    // result of Draw() for this frame
    enum class Action { None, Open, Cancel };

    FileExplorer() {
        m_configPath = DefaultConfigPath();
        LoadConfig();
        RebuildDrives();
        BuildSidebar();

        fs::path start;
#ifdef _WIN32
        start = KnownFolder(FOLDERID_Profile);
#else
        if (const char* h = std::getenv("HOME")) start = h;
#endif
        std::error_code ec;
        if (start.empty() || !fs::exists(start, ec)) start = fs::current_path(ec);
        Navigate(start, /*record=*/false);
    }

    // fills the current window, Open and Cancel along the bottom
    Action Draw() {
        const float footer = m_showFooter ? FooterHeight() : 0.0f;
        DrawBody(footer);
        Action a = m_activated ? Action::Open : Action::None;
        if (m_showFooter) {
            const Action f = DrawFooter();
            if (f != Action::None) a = f;
        }
        return a;
    }

    // same browser as a modal, call ImGui::OpenPopup(title) first
    Action DrawPickerModal(const char* title, bool foldersOnly = false) {
        Action result = Action::None;
        ImGui::SetNextWindowSize(ImVec2(900, 560), ImGuiCond_FirstUseEver);
        if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoScrollbar)) {
            m_foldersOnly = foldersOnly;
            DrawBody(FooterHeight());
            if (m_activated) result = Action::Open;
            const Action f = DrawFooter();
            if (f != Action::None) result = f;
            if (result != Action::None) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        m_foldersOnly = false;
        return result;
    }

    // hide the button row when embedding as a plain panel
    void ShowFooter(bool on) { m_showFooter = on; }
    bool FooterShown() const { return m_showFooter; }

    const fs::path& CurrentPath() const { return m_current; }

    fs::path SelectedPath() const {
        if (m_sel.empty()) return {};
        const size_t i = *m_sel.begin();
        return i < m_entries.size() ? m_entries[i].path : fs::path();
    }

    std::vector<fs::path> SelectedPaths() const {
        std::vector<fs::path> out;
        out.reserve(m_sel.size());
        for (size_t i : m_sel) if (i < m_entries.size()) out.push_back(m_entries[i].path);
        return out;
    }

    void Navigate(const fs::path& p, bool record = true) {
        std::error_code ec;
        fs::path target = fs::weakly_canonical(p, ec);
        if (ec || target.empty()) target = p;
        if (!fs::exists(target, ec) || !fs::is_directory(target, ec)) {
            SetError("Cannot open location:\n" + p.string());
            return;
        }
        if (record && !m_current.empty() && target != m_current) {
            m_back.push_back(m_current);
            m_forward.clear();
        }
        m_current = target;
        AfterNav();
    }

    void SetConfigPath(const fs::path& p) { m_configPath = p; LoadConfig(); BuildSidebar(); }
    const fs::path& ConfigPath() const    { return m_configPath; }

    void SetViewMode(ViewMode v) { m_view_mode = v; SaveConfig(); }
    ViewMode GetViewMode() const { return m_view_mode; }

    // density only, never the font, so any imgui version works
    void  SetUiScale(float s) { m_uiScale = Clamp(s, 0.6f, 2.0f); }
    float UiScale() const     { return m_uiScale; }

    void ShowHiddenFiles(bool on) { m_showHidden = on; SaveConfig(); Refresh(); }
    bool HiddenFilesShown() const { return m_showHidden; }

    // pins keep on-disk casing, matching is case insensitive
    bool IsPinned(const fs::path& p) const {
        const std::string k = Key(p);
        for (const std::string& s : m_userPins) if (ToLower(s) == k) return true;
        return false;
    }
    void Pin(const fs::path& p) {
        std::error_code ec;
        if (!fs::is_directory(p, ec)) return;
        m_hidden.erase(Key(p));
        if (!IsPinned(p)) m_userPins.push_back(p.string());
        SaveConfig();
        BuildSidebar();
    }
    void Unpin(const fs::path& p) {
        const std::string k = Key(p);
        m_userPins.erase(std::remove_if(m_userPins.begin(), m_userPins.end(),
                                        [&](const std::string& s) { return ToLower(s) == k; }),
                         m_userPins.end());
        SaveConfig();
        BuildSidebar();
    }

private:
    struct Entry {
        std::string name;
        fs::path    path;
        bool        isDir  = false;
        bool        hidden = false;
        uintmax_t   size   = 0;
        std::time_t mtime  = 0;
        std::string type;
    };

    // inherited rows can only be hidden, our own pins can be unpinned
    enum class Src { Inherited, User };

    struct SideItem {
        std::string label;
        fs::path    path;
        Src         src = Src::Inherited;
    };

    struct DriveInfo {
        fs::path    root;
        std::string letter;
        std::string label;
        std::string display;              // "Windows (C:)"
        unsigned    type   = 0;           // DRIVE_FIXED / DRIVE_REMOVABLE / ...
        uint64_t    total  = 0;
        uint64_t    freeSp = 0;
        bool        hasSpace = false;     // false for empty or unready drives
    };

    struct TreeNode {
        bool                  loaded = false;
        bool                  hasKids = true;   // assume yes until proven empty
        std::vector<fs::path> kids;
    };

    fs::path                    m_current;
    std::vector<Entry>          m_entries;      // everything in the folder
    std::vector<size_t>         m_view;         // indices into m_entries after filter and sort
    std::unordered_set<size_t>  m_sel;          // selected entry indices
    int                         m_lastView = -1;// last clicked view row, for shift range

    std::vector<fs::path>       m_back, m_forward;
    std::vector<SideItem>       m_quick;
    std::vector<DriveInfo>      m_drives;

    // nodes only open when the user clicks the arrow
    std::unordered_map<std::string, TreeNode> m_tree;
    float                                     m_sidebarW    = 0.0f;
    bool                                      m_sidebarSized = false;  // auto fit already ran
    bool                                      m_sidebarUser  = false;  // user picked a width

    float m_uiScale = 0.85f;

    // drive hotplug polling
    unsigned long m_driveMask     = 0;
    double        m_lastDrivePoll = -1000.0;

    Column   m_sortCol    = Column::Name;
    bool     m_sortAsc    = true;
    bool     m_groupByDate = false;
    bool     m_showHidden  = false;
    ViewMode m_view_mode   = ViewMode::Details;

    char m_search[256] = "";
    bool m_showFooter = true;
    bool m_foldersOnly = false;

    // address bar editing
    bool m_addrEdit  = false;
    bool m_addrFocus = false;
    char m_addrBuf[520] = "";

    // clipboard
    std::vector<fs::path> m_clip;
    bool m_clipCut = false;

    // inline rename
    int  m_renaming = -1;          // view row currently editing
    char m_renameBuf[260] = "";
    bool m_renameFocus = false;

    // modals
    bool m_openError = false;
    bool m_openDelete = false;
    std::string m_error;
    std::vector<fs::path> m_pendingDelete;

    // persisted config
    fs::path                        m_configPath;
    std::vector<std::string>        m_userPins;
    std::unordered_set<std::string> m_hidden;     // Quick access rows the user hid

    bool m_activated = false;
    int  m_themePush = 0;
    int  m_varPush   = 0;

    // applied after the frame so Refresh never rebuilds m_view mid draw
    fs::path m_navRequest;
    bool     m_navPending = false;

    // payloads copy raw bytes, so it is a marker and the paths ride in m_dragPaths
    std::vector<fs::path> m_dragPaths;
    std::vector<fs::path> m_dropSrc;
    fs::path              m_dropDst;
    bool                  m_dropMove   = false;
    bool                  m_dropQueued = false;

    void DrawBody(float reserveBottom) {
        m_activated = false;
        PushTheme();
        PollDrives();

        DrawToolbar();
        ImGui::Separator();
        DrawAddressBar();
        ImGui::Separator();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float  statusH = ImGui::GetTextLineHeightWithSpacing() + 6.0f;
        float panelH = avail.y - statusH - reserveBottom;
        if (panelH < 80.0f) panelH = 80.0f;

        AutoSizeSidebar();
        m_sidebarW = Clamp(m_sidebarW, 150.0f, std::max(160.0f, avail.x - 200.0f));

        ImGui::BeginChild("##imex_sidebar", ImVec2(m_sidebarW, panelH), false);
        DrawSidebar();
        ImGui::EndChild();

        ImGui::SameLine(0, 0);
        DrawSplitter(panelH);
        ImGui::SameLine(0, 0);

        ImGui::BeginChild("##imex_list", ImVec2(0, panelH), false);
        switch (m_view_mode) {
            case ViewMode::Details: DrawDetailsView(); break;
            case ViewMode::List:    DrawGridView(0);   break;
            case ViewMode::Tiles:   DrawGridView(1);   break;
            case ViewMode::Icons:   DrawGridView(2);   break;
        }
        // BeginPopupContextWindow fails on a ScrollY table, hit test children instead
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            !ImGui::IsAnyItemHovered() &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##imex_bgctx");
        if (ImGui::BeginPopup("##imex_bgctx")) {
            BackgroundContextMenu();
            ImGui::EndPopup();
        }
        ImGui::EndChild();

        // at parent level so shortcuts work with focus in the tree
        HandleShortcuts();

        DrawStatusBar();
        DrawModals();

        ApplyPendingDrop();
        if (m_navPending) { m_navPending = false; Navigate(m_navRequest); }

        PopTheme();
    }

    float FooterGap() const { return 4.0f * m_uiScale; }
    float FooterHeight() const {
        return FooterGap() + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    }

    Action DrawFooter() {
        Action a = Action::None;
        const float bw = 96.0f;

        ImGui::Dummy(ImVec2(0.0f, FooterGap()));

        ImGui::BeginDisabled(m_sel.empty());
        if (ImGui::Button("Open", ImVec2(bw, 0))) a = Action::Open;
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(bw, 0))) a = Action::Cancel;
        return a;
    }

    void RequestNav(const fs::path& p) { m_navRequest = p; m_navPending = true; }

    static const char* DragType() { return "IMEX_PATHS"; }

    // dragging an unselected row selects it first
    void BeginRowDrag(size_t vi) {
        if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) return;

        if (!m_sel.count(m_view[vi])) {
            m_sel.clear();
            m_sel.insert(m_view[vi]);
            m_lastView = (int)vi;
        }
        m_dragPaths = SelectedPaths();

        const char marker = 1;
        ImGui::SetDragDropPayload(DragType(), &marker, sizeof(marker));

        const float ih = ImGui::GetTextLineHeight();
        const ImVec2 ip = ImGui::GetCursorScreenPos();
        DrawFolderIcon(ImVec2(ip.x, ip.y), ih);
        ImGui::Dummy(ImVec2(ih + 4.0f, ih));
        ImGui::SameLine();
        if (m_dragPaths.size() == 1)
            ImGui::TextUnformatted(m_dragPaths[0].filename().string().c_str());
        else
            ImGui::Text("%d items", (int)m_dragPaths.size());

        ImGui::EndDragDropSource();
    }

    // make the item just submitted a drop target
    void AcceptDropOn(const fs::path& dir) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (ImGui::AcceptDragDropPayload(DragType())) {
            m_dropSrc    = m_dragPaths;
            m_dropDst    = dir;
            m_dropMove   = !ImGui::GetIO().KeyCtrl;   // Ctrl held = copy
            m_dropQueued = true;
        }
        ImGui::EndDragDropTarget();
    }

    // is child the same as parent or beneath it
    static bool IsSameOrInside(const fs::path& child, const fs::path& parent) {
        const std::string c = Key(child), p = Key(parent);
        if (c == p) return true;
        if (p.empty() || c.size() <= p.size()) return false;
        if (c.compare(0, p.size(), p) != 0) return false;
        const char after = c[p.size()];
        const char last  = p[p.size() - 1];
        return after == '\\' || after == '/' || last == '\\' || last == '/';
    }

    void ApplyPendingDrop() {
        if (!m_dropQueued) return;
        m_dropQueued = false;

        std::error_code ec;
        for (const fs::path& src : m_dropSrc) {
            if (src.empty()) continue;
            if (IsSameOrInside(m_dropDst, src)) {          // a folder into itself
                SetError("Cannot move a folder into itself:\n" + src.string());
                continue;
            }
            if (Key(src.parent_path()) == Key(m_dropDst)) continue;   // already there

            fs::path dst = UniqueDest(m_dropDst / src.filename());
            if (m_dropMove) {
                fs::rename(src, dst, ec);
                if (ec) {                                  // across volumes, copy then remove
                    ec.clear();
                    CopyRecursive(src, dst, ec);
                    if (!ec) fs::remove_all(src, ec);
                }
            } else {
                CopyRecursive(src, dst, ec);
            }
            if (ec) { SetError("Drop failed:\n" + ec.message()); ec.clear(); }
        }

        InvalidateTree(m_dropDst);
        InvalidateTree(m_current);
        m_dropSrc.clear();
        m_dragPaths.clear();
        Refresh();
    }

    // fit the sidebar to its widest label once, needs a live font
    void AutoSizeSidebar() {
        if (m_sidebarUser || m_sidebarSized) return;
        m_sidebarSized = true;

        float w = 0.0f;
        for (const SideItem& q : m_quick)  w = std::max(w, ImGui::CalcTextSize(q.label.c_str()).x);
        for (const DriveInfo& d : m_drives) w = std::max(w, ImGui::CalcTextSize(d.display.c_str()).x);

        const ImGuiStyle& st = ImGui::GetStyle();
        w += ImGui::GetTreeNodeToLabelSpacing()   // the expand arrow
           + ImGui::GetTextLineHeight()           // the icon drawn before the label
           + st.IndentSpacing                     // rows sit one level under their section
           + st.ScrollbarSize                     // the vertical scrollbar overlaps the text
           + st.WindowPadding.x * 2.0f            // child padding, both sides
           + 20.0f;                               // breathing room so "(C:)" is not flush right
        m_sidebarW = Clamp(w, 190.0f, 360.0f);
    }

    // false at a drive root, parent_path of "C:\" is "C:" which is not one level up
    bool HasUpEntry() const {
        if (!m_current.has_parent_path()) return false;
        if (m_current.parent_path() == m_current) return false;
        return m_current != m_current.root_path();
    }

    std::string ParentLabel() const {
        const fs::path p = m_current.parent_path();
        std::string n = p.filename().string();
        if (n.empty()) n = p.root_name().string();
        if (n.empty()) n = p.string();
        return n;
    }

    void DrawSplitter(float h) {
        const float w = 6.0f;
        ImGui::InvisibleButton("##imex_split", ImVec2(w, h));
        if (ImGui::IsItemActive()) {
            m_sidebarW   += ImGui::GetIO().MouseDelta.x;
            m_sidebarUser = true;
        }
        if (ImGui::IsItemDeactivated()) SaveConfig();
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2((a.x + b.x) * 0.5f - 1.0f, a.y), ImVec2((a.x + b.x) * 0.5f + 1.0f, b.y),
                IM_COL32(120, 160, 210, 200));
        }
    }

    void DrawToolbar() {
        const bool canBack = !m_back.empty();
        const bool canFwd  = !m_forward.empty();
        const bool canUp   = HasUpEntry();

        ImGui::BeginDisabled(!canBack);
        if (ImGui::ArrowButton("##imex_back", ImGuiDir_Left)) GoBack();
        ImGui::EndDisabled();
        Tip("Back");
        ImGui::SameLine();

        ImGui::BeginDisabled(!canFwd);
        if (ImGui::ArrowButton("##imex_fwd", ImGuiDir_Right)) GoForward();
        ImGui::EndDisabled();
        Tip("Forward");
        ImGui::SameLine();

        ImGui::BeginDisabled(!canUp);
        if (ImGui::ArrowButton("##imex_up", ImGuiDir_Up)) NavigateUp();
        ImGui::EndDisabled();
        Tip("Up");
        ImGui::SameLine();

        if (ImGui::Button("Refresh")) RefreshAll();
        ImGui::SameLine();

        // file operations live in the right click menus and on the keyboard
        VSep();

        // view mode picker
        static const char* kViewNames[] = { "Details", "List", "Tiles", "Large icons" };
        ImGui::SetNextItemWidth(112.0f);
        if (ImGui::BeginCombo("##imex_view", kViewNames[(int)m_view_mode])) {
            for (int i = 0; i < 4; ++i)
                if (ImGui::Selectable(kViewNames[i], (int)m_view_mode == i))
                    SetViewMode((ViewMode)i);
            ImGui::EndCombo();
        }
        Tip("View mode  (Ctrl+Shift+1..4)");
        ImGui::SameLine();

        if (ImGui::Checkbox("Group by date", &m_groupByDate)) { /* affects Details only */ }
        ImGui::SameLine();
        if (ImGui::Checkbox("Hidden", &m_showHidden)) { SaveConfig(); Refresh(); }
        Tip("Show hidden and system files  (Ctrl+H)");

        // search box, right-aligned
        const float searchW = 200.0f;
        ImGui::SameLine();
        float off = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - searchW;
        if (off > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(off);
        ImGui::SetNextItemWidth(searchW);
        if (ImGui::InputTextWithHint("##imex_search", "Search", m_search, sizeof(m_search)))
            RebuildView();
    }

    void DrawAddressBar() {
        const float pad = 5.0f * m_uiScale;
        ImGui::Dummy(ImVec2(0.0f, pad));

        if (m_addrEdit) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (m_addrFocus) { ImGui::SetKeyboardFocusHere(); m_addrFocus = false; }
            // AutoSelectAll so typing replaces the path
            const bool enter = ImGui::InputText("##imex_addr", m_addrBuf, sizeof(m_addrBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue |
                                                ImGuiInputTextFlags_AutoSelectAll);
            const bool esc  = ImGui::IsKeyPressed(ImGuiKey_Escape);
            const bool lost = ImGui::IsItemDeactivated() && !enter;
            if (enter) { m_addrEdit = false; if (m_addrBuf[0]) Navigate(fs::path(m_addrBuf)); }
            else if (esc || lost) { m_addrEdit = false; }
            ImGui::Dummy(ImVec2(0.0f, pad));
            return;
        }

        const ImGuiStyle& st = ImGui::GetStyle();
        const float  rowH = ImGui::GetFrameHeight();
        const float  lh   = ImGui::GetTextLineHeight();
        const ImVec2 p0   = ImGui::GetCursorScreenPos();
        const float  w    = ImGui::GetContentRegionAvail().x;
        const ImVec2 p1(p0.x + w, p0.y + rowH);

        // draw it as a real address field so it reads as clickable
        const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p0, p1);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered
                                                            : ImGuiCol_FrameBg), st.FrameRounding);
        dl->AddRect(p0, p1, ImGui::GetColorU32(hovered ? ImGuiCol_SliderGrab : ImGuiCol_Border),
                    st.FrameRounding);

        std::vector<fs::path> chain;
        fs::path p = m_current;
        while (true) {
            chain.push_back(p);
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        std::reverse(chain.begin(), chain.end());

        ImGui::SetCursorScreenPos(ImVec2(p0.x + 6.0f, p0.y + (rowH - lh) * 0.5f));
        for (size_t i = 0; i < chain.size(); ++i) {
            const fs::path& c = chain[i];
            std::string label = c.filename().string();
            if (label.empty()) {                       // drive root e.g. "C:\"
                label = c.root_name().string();
                if (label.empty()) label = c.string(); // "/" on posix
            }
            ImGui::PushID((int)i);
            if (ImGui::SmallButton(label.c_str())) Navigate(c);
            ImGui::PopID();
            ImGui::SameLine(0, 3);
            if (i + 1 < chain.size()) {
                ImGui::TextDisabled(">");
                ImGui::SameLine(0, 3);
            }
        }

        // click anywhere in the rest of the row to type a path
        const float restW = p1.x - ImGui::GetCursorScreenPos().x - 6.0f;
        if (restW > 8.0f) {
            ImGui::InvisibleButton("##imex_addr_blank", ImVec2(restW, lh));
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
            Tip("Click to type a path  (Ctrl+L)");
            if (ImGui::IsItemClicked()) BeginAddressEdit();

            // spell it out while the pointer is over the bar
            if (hovered) {
                const char* hint = "click to type a path";
                const float hw = ImGui::CalcTextSize(hint).x;
                if (hw + 12.0f < restW)
                    dl->AddText(ImVec2(p1.x - hw - 6.0f, p0.y + (rowH - lh) * 0.5f),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
        ImGui::Dummy(ImVec2(0.0f, pad));
    }

    void BeginAddressEdit() {
        std::snprintf(m_addrBuf, sizeof(m_addrBuf), "%s", m_current.string().c_str());
        m_addrEdit = true;
        m_addrFocus = true;
    }

    void DrawSidebar() {
        const ImGuiTreeNodeFlags secFlags =
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

        if (ImGui::TreeNodeEx("Quick access", secFlags)) {
            for (size_t i = 0; i < m_quick.size(); ++i)
                DrawTreeItem(m_quick[i].path, m_quick[i].label, nullptr, m_quick[i].src, 0);
            if (m_quick.empty()) ImGui::TextDisabled("  (empty)");
            ImGui::TreePop();
        }

        ImGui::Spacing();

        if (ImGui::TreeNodeEx("This PC", secFlags)) {
            for (const DriveInfo& d : m_drives)
                DrawTreeItem(d.root, d.display, &d, Src::Inherited, 0);
            ImGui::TreePop();
        }
    }

    // one tree row, di non null draws a capacity bar
    void DrawTreeItem(const fs::path& path, const std::string& label,
                      const DriveInfo* di, Src src, int depth) {
        if (depth > 12) return;

        const std::string key = Key(path);
        TreeNode& node = m_tree[key];

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!node.hasKids)              flags |= ImGuiTreeNodeFlags_Leaf;
        if (SamePath(path, m_current))  flags |= ImGuiTreeNodeFlags_Selected;

        ImGui::PushID(key.c_str());

        const float  ih = ImGui::GetTextLineHeight();
        const ImVec2 ip = ImGui::GetCursorScreenPos();
        const float  lx = ImGui::GetTreeNodeToLabelSpacing();

        std::string text = std::string(LeadingPad(ih)) + label;
        const bool open = ImGui::TreeNodeEx("##node", flags, "%s", text.c_str());

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            Navigate(path);

        AcceptDropOn(path);

        if (ImGui::BeginPopupContextItem("##imex_sbctx")) {
            SidebarContextMenu(path, src);
            ImGui::EndPopup();
        }

        // icon sits between the arrow and the label
        if (di) DrawDriveIcon (ImVec2(ip.x + lx, ip.y), ih, di->type);
        else    DrawFolderIcon(ImVec2(ip.x + lx, ip.y), ih);

        // drive capacity bar
        if (di && di->hasSpace && di->total > 0)
            DrawCapacityBar(*di, ip.x + lx);

        if (open) {
            LoadTreeChildren(path, node);
            for (const fs::path& kid : node.kids)
                DrawTreeItem(kid, kid.filename().string(), nullptr, Src::Inherited, depth + 1);
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void DrawCapacityBar(const DriveInfo& di, float x) {
        const float barH = 5.0f * m_uiScale;
        const float w = ImGui::GetContentRegionAvail().x - 6.0f;
        if (w < 24.0f) return;

        ImGui::Dummy(ImVec2(0.0f, barH + 3.0f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float y = ImGui::GetItemRectMin().y + 1.0f;

        const double usedFrac = di.total ? (double)(di.total - di.freeSp) / (double)di.total : 0.0;
        const bool   low      = usedFrac > 0.90;
        const ImU32  track    = IM_COL32(70, 70, 70, 255);
        const ImU32  fill     = low ? IM_COL32(214, 74, 66, 255) : IM_COL32(56, 140, 220, 255);

        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + barH), track, 2.0f);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + (float)(w * usedFrac), y + barH), fill, 2.0f);

        if (ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + w, y + barH)))
            ImGui::SetTooltip("%s free of %s",
                              FormatSizeExact(di.freeSp).c_str(), FormatSizeExact(di.total).c_str());
    }

    void SidebarContextMenu(const fs::path& path, Src src) {
        if (ImGui::MenuItem("Open")) Navigate(path);
        ImGui::Separator();
        if (src == Src::User) {
            if (ImGui::MenuItem("Unpin from Quick access")) Unpin(path);
        } else if (src == Src::Inherited) {
            if (ImGui::MenuItem("Hide from Quick access")) {
                m_hidden.insert(Key(path));
                SaveConfig();
                BuildSidebar();
            }
            Tip("Windows owns this entry, so it can only be hidden here");
        }
        if (!m_hidden.empty() && ImGui::MenuItem("Restore hidden entries")) {
            m_hidden.clear();
            SaveConfig();
            BuildSidebar();
        }
    }

    void LoadTreeChildren(const fs::path& p, TreeNode& node) {
        if (node.loaded) return;
        node.loaded = true;
        node.kids.clear();

        std::error_code ec;
        for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2) || e2) continue;
            if (!m_showHidden && IsHiddenPath(it->path())) continue;
            node.kids.push_back(it->path());
            if (node.kids.size() >= 2000) break;          // sanity cap
        }
        std::sort(node.kids.begin(), node.kids.end(), [](const fs::path& a, const fs::path& b) {
            return CompareNatural(a.filename().string(), b.filename().string()) < 0;
        });
        node.hasKids = !node.kids.empty();
    }

    void DrawDetailsView() {
        const ImGuiTableFlags flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Reorderable;

        if (!ImGui::BeginTable("##imex_table", 4, flags)) return;

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch |
                                ImGuiTableColumnFlags_DefaultSort, 0.0f, (ImGuiID)Column::Name);
        ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthFixed, 150.0f, (ImGuiID)Column::Date);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 130.0f, (ImGuiID)Column::Type);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f, (ImGuiID)Column::Size);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* s = ImGui::TableGetSortSpecs()) {
            if (s->SpecsDirty && s->SpecsCount > 0) {
                m_sortCol = (Column)s->Specs[0].ColumnUserID;
                m_sortAsc = s->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                SortView();
                s->SpecsDirty = false;
            }
        }

        DrawUpRow();

        const bool grouped = m_groupByDate && m_sortCol == Column::Date;
        std::string lastGroup;

        for (size_t vi = 0; vi < m_view.size(); ++vi) {
            const Entry& e = m_entries[m_view[vi]];
            if (grouped) {
                std::string g = DateGroup(e.mtime);
                if (g != lastGroup) {
                    lastGroup = g;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 190, 240, 255));
                    ImGui::TextUnformatted(g.c_str());
                    ImGui::PopStyleColor();
                }
            }
            DrawRow(vi, e);
        }

        ImGui::EndTable();
    }

    // lives outside m_view so it never joins selection or the count
    void DrawUpRow() {
        if (!HasUpEntry()) return;

        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        const float  ih = ImGui::GetTextLineHeight();
        const ImVec2 ip = ImGui::GetCursorScreenPos();

        const std::string label = std::string(LeadingPad(ih)) + "...###imex_up";
        ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, ih));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Up to %s", ParentLabel().c_str());
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                RequestNav(m_current.parent_path());
        }
        AcceptDropOn(m_current.parent_path());      // drop here to move out of this folder
        DrawUpIcon(ImVec2(ip.x + 1, ip.y), ih);

        ImGui::TableNextColumn();                       // date
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Parent folder");
        ImGui::TableNextColumn();                       // size
    }

    void DrawRow(size_t vi, const Entry& e) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        const float ih = ImGui::GetTextLineHeight();

        if (m_renaming == (int)vi) {
            const ImVec2 ip = ImGui::GetCursorScreenPos();
            if (e.isDir) DrawFolderIcon(ImVec2(ip.x + 1, ip.y), ih);
            else         DrawFileIcon(ImVec2(ip.x + 1, ip.y), ih);
            ImGui::SetCursorScreenPos(ImVec2(ip.x + ih + 4, ip.y));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (m_renameFocus) { ImGui::SetKeyboardFocusHere(); m_renameFocus = false; }
            if (ImGui::InputText("##imex_rename", m_renameBuf, sizeof(m_renameBuf),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                CommitRename();
            else if (ImGui::IsItemDeactivated())
                CommitRename();
        } else {
            const ImVec2 ip = ImGui::GetCursorScreenPos();
            const bool selected = m_sel.count(m_view[vi]) > 0;
            std::string label = std::string(LeadingPad(ih)) + e.name + "###row_" + std::to_string(vi);

            if (e.hidden) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 130));
            if (ImGui::Selectable(label.c_str(), selected,
                    ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, ih)))
                HandleClick(vi);
            if (e.hidden) ImGui::PopStyleColor();

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                Activate(vi);

            BeginRowDrag(vi);
            if (e.isDir) AcceptDropOn(e.path);

            if (ImGui::BeginPopupContextItem()) {
                if (!selected) HandleClick(vi);
                RowContextMenu(vi, e);
                ImGui::EndPopup();
            }

            // icon over the reserved leading space
            if (e.isDir) DrawFolderIcon(ImVec2(ip.x + 1, ip.y), ih);
            else         DrawFileIcon(ImVec2(ip.x + 1, ip.y), ih);
        }

        ImGui::TableNextColumn();
        if (e.mtime) ImGui::TextUnformatted(FormatDate(e.mtime).c_str());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.type.c_str());

        ImGui::TableNextColumn();
        if (!e.isDir) ImGui::TextUnformatted(FormatSizeKB(e.size).c_str());
    }

    void DrawGridView(int kind) {
        const float lh = ImGui::GetTextLineHeight();
        const float s  = m_uiScale;
        float cellW, cellH, iconSz;
        switch (kind) {
            case 0:  cellW = 190.0f * s; cellH = lh + 5.0f * s;          iconSz = lh;        break;  // List
            case 1:  cellW = 250.0f * s; cellH = lh * 2.2f + 6.0f * s;   iconSz = lh * 2.0f; break;  // Tiles
            default: cellW = 110.0f * s; cellH = lh * 2.2f + 62.0f * s;  iconSz = 58.0f * s; break;  // Icons
        }

        const float availW = ImGui::GetContentRegionAvail().x;
        int cols = (int)(availW / (cellW + 4.0f));
        if (cols < 1) cols = 1;

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // the ... cell takes the first slot so everything shifts by one
        const int lead = HasUpEntry() ? 1 : 0;
        if (lead) DrawUpCell(kind, cellW, cellH, iconSz, dl);

        for (size_t vi = 0; vi < m_view.size(); ++vi) {
            const Entry& e = m_entries[m_view[vi]];
            if (((int)vi + lead) % cols != 0) ImGui::SameLine();

            ImGui::PushID((int)vi);
            const ImVec2 ip = ImGui::GetCursorScreenPos();
            const bool selected = m_sel.count(m_view[vi]) > 0;

            if (ImGui::Selectable("##cell", selected, ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(cellW, cellH)))
                HandleClick(vi);

            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                Activate(vi);

            BeginRowDrag(vi);
            if (e.isDir) AcceptDropOn(e.path);

            if (ImGui::BeginPopupContextItem("##imex_cellctx")) {
                if (!selected) HandleClick(vi);
                RowContextMenu(vi, e);
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered() && kind != 2)
                ImGui::SetTooltip("%s", e.name.c_str());

            const ImU32 textCol = e.hidden ? IM_COL32(255, 255, 255, 130)
                                           : ImGui::GetColorU32(ImGuiCol_Text);

            if (kind == 2) {
                // large icon centred, name on up to two lines
                const float ix = ip.x + (cellW - iconSz) * 0.5f;
                const float iy = ip.y + 6.0f;
                if (e.isDir) DrawFolderIcon(ImVec2(ix, iy), iconSz);
                else         DrawFileIcon(ImVec2(ix, iy), iconSz);

                std::string l1, l2;
                WrapTwoLines(e.name, cellW - 8.0f, l1, l2);
                float ty = iy + iconSz + 4.0f;
                CenteredText(dl, ip.x, cellW, ty, l1, textCol);
                if (!l2.empty()) CenteredText(dl, ip.x, cellW, ty + lh, l2, textCol);
            } else if (kind == 1) {
                // icon left, name and type on the right
                const float iy = ip.y + (cellH - iconSz) * 0.5f;
                if (e.isDir) DrawFolderIcon(ImVec2(ip.x + 4.0f, iy), iconSz);
                else         DrawFileIcon(ImVec2(ip.x + 4.0f, iy), iconSz);

                const float tx = ip.x + iconSz + 10.0f;
                const float tw = cellW - (iconSz + 14.0f);
                dl->AddText(ImVec2(tx, ip.y + 3.0f), textCol, Ellipsize(e.name, tw).c_str());

                std::string sub = e.type;
                if (!e.isDir) sub += "   " + FormatSizeExact(e.size);
                dl->AddText(ImVec2(tx, ip.y + 3.0f + lh + 2.0f),
                            IM_COL32(180, 180, 180, 255), Ellipsize(sub, tw).c_str());
            } else {
                // small icon and one line
                if (e.isDir) DrawFolderIcon(ImVec2(ip.x + 3.0f, ip.y + 3.0f), iconSz);
                else         DrawFileIcon(ImVec2(ip.x + 3.0f, ip.y + 3.0f), iconSz);
                dl->AddText(ImVec2(ip.x + iconSz + 8.0f, ip.y + 3.0f), textCol,
                            Ellipsize(e.name, cellW - iconSz - 12.0f).c_str());
            }

            ImGui::PopID();
        }
    }

    void DrawUpCell(int kind, float cellW, float cellH, float iconSz, ImDrawList* dl) {
        ImGui::PushID("##imex_upcell");
        const ImVec2 ip = ImGui::GetCursorScreenPos();
        const float  lh = ImGui::GetTextLineHeight();

        ImGui::Selectable("##up", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cellW, cellH));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Up to %s", ParentLabel().c_str());
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                RequestNav(m_current.parent_path());
        }

        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
        if (kind == 2) {
            const float ix = ip.x + (cellW - iconSz) * 0.5f;
            DrawUpIcon(ImVec2(ix, ip.y + 6.0f), iconSz);
            CenteredText(dl, ip.x, cellW, ip.y + 6.0f + iconSz + 4.0f, "...", col);
        } else if (kind == 1) {
            DrawUpIcon(ImVec2(ip.x + 4.0f, ip.y + (cellH - iconSz) * 0.5f), iconSz);
            const float tx = ip.x + iconSz + 10.0f;
            dl->AddText(ImVec2(tx, ip.y + 3.0f), col, "...");
            dl->AddText(ImVec2(tx, ip.y + 3.0f + lh + 2.0f), IM_COL32(180, 180, 180, 255), "Parent folder");
        } else {
            DrawUpIcon(ImVec2(ip.x + 3.0f, ip.y + 3.0f), iconSz);
            dl->AddText(ImVec2(ip.x + iconSz + 8.0f, ip.y + 3.0f), col, "...");
        }
        ImGui::PopID();
    }

    static void CenteredText(ImDrawList* dl, float x, float w, float y,
                             const std::string& s, ImU32 col) {
        if (s.empty()) return;
        const float tw = ImGui::CalcTextSize(s.c_str()).x;
        dl->AddText(ImVec2(x + (w - tw) * 0.5f, y), col, s.c_str());
    }

    void RowContextMenu(size_t vi, const Entry& e) {
        if (ImGui::MenuItem("Open", "Enter")) Activate(vi);
        ImGui::Separator();
        if (ImGui::MenuItem("Cut",   "Ctrl+X")) DoCopy(true);
        if (ImGui::MenuItem("Copy",  "Ctrl+C")) DoCopy(false);
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_clip.empty())) DoPaste();
        ImGui::Separator();
        if (e.isDir) {
            if (IsPinned(e.path)) {
                if (ImGui::MenuItem("Unpin from Quick access")) Unpin(e.path);
            } else {
                if (ImGui::MenuItem("Pin to Quick access")) Pin(e.path);
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("New folder"))   NewFolder();
        if (ImGui::MenuItem("Rename", "F2")) BeginRename(vi);
        if (ImGui::MenuItem("Delete", "Del")) RequestDelete();
    }

    // right click on empty space in the file list
    void BackgroundContextMenu() {
        if (ImGui::MenuItem("New folder")) NewFolder();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_clip.empty())) DoPaste();
        ImGui::Separator();
        if (HasUpEntry() && ImGui::MenuItem("Up one level", "Backspace")) NavigateUp();
        if (ImGui::MenuItem("Refresh", "F5")) RefreshAll();
        ImGui::Separator();
        if (ImGui::MenuItem("Show hidden files", "Ctrl+H", m_showHidden)) {
            m_showHidden = !m_showHidden;
            SaveConfig();
            Refresh();
        }
        if (!IsPinned(m_current) && ImGui::MenuItem("Pin this folder to Quick access")) Pin(m_current);
    }

    void RefreshAll() { RebuildDrives(); BuildSidebar(); m_tree.clear(); Refresh(); }

    void DrawStatusBar() {
        ImGui::Separator();
        std::string s = std::to_string(m_view.size()) + " items";
        if (!m_sel.empty()) {
            uintmax_t total = 0; bool anyFile = false;
            for (size_t i : m_sel) {
                if (i < m_entries.size() && !m_entries[i].isDir) { total += m_entries[i].size; anyFile = true; }
            }
            s += "   |   " + std::to_string(m_sel.size()) +
                 (m_sel.size() == 1 ? " item selected" : " items selected");
            if (anyFile) s += "   " + FormatSizeExact(total);
        }
        if (const DriveInfo* d = DriveOf(m_current))
            if (d->hasSpace)
                s += "   |   " + FormatSizeExact(d->freeSp) + " free on " + d->display;

        ImGui::TextUnformatted(s.c_str());
    }

    const DriveInfo* DriveOf(const fs::path& p) const {
        std::string rn = ToLower(p.root_name().string());
        if (rn.empty()) return nullptr;
        for (const DriveInfo& d : m_drives)
            if (ToLower(d.letter) == rn) return &d;
        return nullptr;
    }

    void DrawModals() {
        if (m_openError) { ImGui::OpenPopup("Error##imex"); m_openError = false; }
        if (ImGui::BeginPopupModal("Error##imex", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(m_error.c_str());
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (m_openDelete) { ImGui::OpenPopup("Delete##imex"); m_openDelete = false; }
        if (ImGui::BeginPopupModal("Delete##imex", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Move %d item(s) to the Recycle Bin?", (int)m_pendingDelete.size());
            ImGui::Spacing();
            if (ImGui::Button("Delete", ImVec2(100, 0))) { DoDelete(); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) { m_pendingDelete.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }

    void GoBack()  { if (m_back.empty())  return; m_forward.push_back(m_current); m_current = m_back.back();  m_back.pop_back();  AfterNav(); }
    void GoForward(){ if (m_forward.empty()) return; m_back.push_back(m_current); m_current = m_forward.back(); m_forward.pop_back(); AfterNav(); }
    void NavigateUp() {
        if (HasUpEntry()) RequestNav(m_current.parent_path());
    }
    void AfterNav() {
        m_sel.clear();
        m_lastView = -1;
        m_renaming = -1;
        m_addrEdit = false;
        m_search[0] = '\0';
        Refresh();
    }

    void Refresh() {
        m_entries.clear();
        std::error_code ec;
        for (auto it = fs::directory_iterator(m_current, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const fs::directory_entry& de = *it;
            std::error_code e2;
            Entry en;
            en.path   = de.path();
            en.name   = de.path().filename().string();
            en.isDir  = de.is_directory(e2);
            en.hidden = IsHiddenPath(de.path());
            if (!en.isDir) { en.size = de.file_size(e2); if (e2) en.size = 0; }
            auto ft = de.last_write_time(e2);
            en.mtime = e2 ? 0 : FileTimeToTimeT(ft);
            en.type  = TypeString(en);
            m_entries.push_back(std::move(en));
        }
        RebuildView();
    }

    void RebuildView() {
        m_view.clear();
        std::string q = ToLower(m_search);
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].hidden && !m_showHidden) continue;
            if (!q.empty() && ToLower(m_entries[i].name).find(q) == std::string::npos) continue;
            m_view.push_back(i);
        }
        SortView();
        m_sel.clear();
        m_lastView = -1;
    }

    void SortView() {
        const bool grouped = m_groupByDate && m_sortCol == Column::Date;
        auto cmp = [&](size_t a, size_t b) {
            const Entry& ea = m_entries[a];
            const Entry& eb = m_entries[b];
            if (!grouped && ea.isDir != eb.isDir) return ea.isDir;   // folders first
            int c = 0;
            switch (m_sortCol) {
                case Column::Name: c = CompareNatural(ea.name, eb.name); break;
                case Column::Date: c = (ea.mtime < eb.mtime) ? -1 : (ea.mtime > eb.mtime ? 1 : 0); break;
                case Column::Type: c = ea.type.compare(eb.type); if (c) c = c < 0 ? -1 : 1; else c = CompareNatural(ea.name, eb.name); break;
                case Column::Size: {
                    uintmax_t sa = ea.isDir ? 0 : ea.size, sb = eb.isDir ? 0 : eb.size;
                    c = (sa < sb) ? -1 : (sa > sb ? 1 : 0);
                    if (c == 0) c = CompareNatural(ea.name, eb.name);
                } break;
            }
            return m_sortAsc ? c < 0 : c > 0;
        };
        std::stable_sort(m_view.begin(), m_view.end(), cmp);
    }

    void HandleClick(size_t vi) {
        const ImGuiIO& io = ImGui::GetIO();
        const size_t entry = m_view[vi];
        if (io.KeyCtrl) {
            if (m_sel.count(entry)) m_sel.erase(entry); else m_sel.insert(entry);
            m_lastView = (int)vi;
        } else if (io.KeyShift && m_lastView >= 0) {
            m_sel.clear();
            size_t a = std::min((size_t)m_lastView, vi), b = std::max((size_t)m_lastView, vi);
            for (size_t k = a; k <= b && k < m_view.size(); ++k) m_sel.insert(m_view[k]);
        } else {
            m_sel.clear();
            m_sel.insert(entry);
            m_lastView = (int)vi;
        }
    }

    void Activate(size_t vi) {
        const Entry& e = m_entries[m_view[vi]];
        if (e.isDir) {
            RequestNav(e.path);
        } else if (!m_foldersOnly) {
            m_sel.clear();
            m_sel.insert(m_view[vi]);
            m_activated = true;
        }
    }

    void NewFolder() {
        std::error_code ec;
        fs::path p = m_current / "New folder";
        for (int n = 2; fs::exists(p, ec); ++n)
            p = m_current / ("New folder (" + std::to_string(n) + ")");
        fs::create_directory(p, ec);
        if (ec) { SetError("Could not create folder:\n" + ec.message()); return; }
        Refresh();
        InvalidateTree(m_current);
        if (m_view_mode != ViewMode::Details) return;   // inline rename is Details only
        for (size_t vi = 0; vi < m_view.size(); ++vi)
            if (m_entries[m_view[vi]].path == p) { BeginRename(vi); break; }
    }

    void BeginRenameSelected() {
        if (m_sel.size() != 1) return;
        size_t entry = *m_sel.begin();
        for (size_t vi = 0; vi < m_view.size(); ++vi)
            if (m_view[vi] == entry) { BeginRename(vi); return; }
    }

    void BeginRename(size_t vi) {
        m_view_mode = ViewMode::Details;      // rename edits in place in the table
        m_renaming = (int)vi;
        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", m_entries[m_view[vi]].name.c_str());
        m_renameFocus = true;
    }

    void CommitRename() {
        if (m_renaming < 0 || m_renaming >= (int)m_view.size()) { m_renaming = -1; return; }
        const Entry& e = m_entries[m_view[m_renaming]];
        std::string nn = m_renameBuf;
        if (!nn.empty() && nn != e.name) {
            std::error_code ec;
            fs::rename(e.path, e.path.parent_path() / nn, ec);
            if (ec) SetError("Rename failed:\n" + ec.message());
        }
        m_renaming = -1;
        Refresh();
        InvalidateTree(m_current);
    }

    void DoCopy(bool cut) { m_clip = SelectedPaths(); m_clipCut = cut; }

    void DoPaste() {
        std::error_code ec;
        for (const fs::path& src : m_clip) {
            fs::path dst = UniqueDest(m_current / src.filename());
            if (m_clipCut) {
                fs::rename(src, dst, ec);
                if (ec) {                          // across volumes, copy then remove
                    ec.clear();
                    CopyRecursive(src, dst, ec);
                    if (!ec) fs::remove_all(src, ec);
                }
            } else {
                CopyRecursive(src, dst, ec);
            }
            if (ec) { SetError("Paste failed:\n" + ec.message()); ec.clear(); }
        }
        if (m_clipCut) { m_clip.clear(); m_clipCut = false; }
        Refresh();
        InvalidateTree(m_current);
    }

    static void CopyRecursive(const fs::path& src, const fs::path& dst, std::error_code& ec) {
        if (fs::is_directory(src, ec))
            fs::copy(src, dst, fs::copy_options::recursive, ec);
        else
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }

    static fs::path UniqueDest(fs::path dst) {
        std::error_code ec;
        if (!fs::exists(dst, ec)) return dst;
        fs::path dir  = dst.parent_path();
        std::string stem = dst.stem().string();
        std::string ext  = dst.extension().string();
        for (int n = 0;; ++n) {
            std::string suffix = (n == 0) ? " - Copy" : " - Copy (" + std::to_string(n + 1) + ")";
            fs::path cand = dir / (stem + suffix + ext);
            if (!fs::exists(cand, ec)) return cand;
        }
    }

    void RequestDelete() {
        m_pendingDelete = SelectedPaths();
        if (!m_pendingDelete.empty()) m_openDelete = true;
    }

    void DoDelete() {
#ifdef _WIN32
        std::wstring buf;
        for (const fs::path& p : m_pendingDelete) { buf += p.wstring(); buf.push_back(L'\0'); }
        buf.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc  = FO_DELETE;
        op.pFrom  = buf.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        SHFileOperationW(&op);
#else
        std::error_code ec;
        for (const fs::path& p : m_pendingDelete) fs::remove_all(p, ec);
#endif
        m_pendingDelete.clear();
        Refresh();
        InvalidateTree(m_current);
    }

    void InvalidateTree(const fs::path& p) {
        auto it = m_tree.find(Key(p));
        if (it != m_tree.end()) { it->second.loaded = false; it->second.kids.clear(); }
    }

    void HandleShortcuts() {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
        if (m_renaming >= 0 || m_addrEdit) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) return;      // don't steal keys from the search box

        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) NavigateUp();
        if (ImGui::IsKeyPressed(ImGuiKey_F2))        BeginRenameSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_F5))        RefreshAll();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))    RequestDelete();
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_sel.size() == 1) {
            size_t entry = *m_sel.begin();
            for (size_t vi = 0; vi < m_view.size(); ++vi)
                if (m_view[vi] == entry) { Activate(vi); break; }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) DoCopy(false);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) DoCopy(true);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) DoPaste();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L)) BeginAddressEdit();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_H)) { m_showHidden = !m_showHidden; SaveConfig(); Refresh(); }

        if (io.KeyCtrl && io.KeyShift) {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) SetViewMode(ViewMode::Details);
            if (ImGui::IsKeyPressed(ImGuiKey_2)) SetViewMode(ViewMode::List);
            if (ImGui::IsKeyPressed(ImGuiKey_3)) SetViewMode(ViewMode::Tiles);
            if (ImGui::IsKeyPressed(ImGuiKey_4)) SetViewMode(ViewMode::Icons);
        }
    }

    void PushTheme() {
        struct C { ImGuiCol idx; ImU32 col; };
        static const C cols[] = {
            { ImGuiCol_ChildBg,        IM_COL32(32,  32,  32,  255) },
            { ImGuiCol_Header,         IM_COL32(0,   120, 215, 110) },
            { ImGuiCol_HeaderHovered,  IM_COL32(255, 255, 255, 22)  },
            { ImGuiCol_HeaderActive,   IM_COL32(0,   120, 215, 160) },
            { ImGuiCol_TableHeaderBg,  IM_COL32(38,  38,  38,  255) },
            { ImGuiCol_TableRowBg,     IM_COL32(0,   0,   0,   0)   },
            { ImGuiCol_TableRowBgAlt,  IM_COL32(255, 255, 255, 8)   },
        };
        m_themePush = (int)(sizeof(cols) / sizeof(cols[0]));
        for (const C& c : cols) ImGui::PushStyleColor(c.idx, c.col);

        // absolute values so the look does not depend on the host style
        const float s = m_uiScale;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6.0f * s, 3.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(5.0f * s, 2.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,   ImVec2(4.0f * s, 1.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 15.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12.0f * s);
        m_varPush = 5;
    }
    void PopTheme() {
        ImGui::PopStyleVar(m_varPush);   m_varPush = 0;
        ImGui::PopStyleColor(m_themePush); m_themePush = 0;
    }

    // all geometry is a fraction of s so icons scale from 13px to 62px
    static void DrawFolderIcon(ImVec2 p, float s) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 body = IM_COL32(255, 206, 84, 255);
        const ImU32 tab  = IM_COL32(232, 180, 60, 255);
        const float pad     = s * 0.08f;
        const float tabTop  = p.y + s * 0.20f;
        const float bodyTop = p.y + s * 0.31f;
        const float bottom  = p.y + s - pad;
        const float r       = s * 0.07f;
        dl->AddRectFilled(ImVec2(p.x + pad, tabTop), ImVec2(p.x + s * 0.52f, bodyTop + r), tab, r);
        dl->AddRectFilled(ImVec2(p.x + pad, bodyTop), ImVec2(p.x + s - pad, bottom), body, r);
    }
    static void DrawFileIcon(ImVec2 p, float s) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 page = IM_COL32(238, 241, 245, 255);
        const ImU32 fold = IM_COL32(186, 193, 200, 255);
        const ImU32 line = IM_COL32(158, 166, 176, 255);
        const float x0 = p.x + s * 0.20f, x1 = p.x + s * 0.80f;
        const float y0 = p.y + s * 0.06f, y1 = p.y + s * 0.94f;
        const float f  = s * 0.24f;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), page, s * 0.05f);
        dl->AddTriangleFilled(ImVec2(x1 - f, y0), ImVec2(x1, y0 + f), ImVec2(x1 - f, y0 + f), fold);
        if (s >= 28.0f) {                       // only when big enough to read as text
            const float lx0 = x0 + s * 0.09f, lx1 = x1 - s * 0.09f, th = s * 0.05f;
            for (int i = 0; i < 3; ++i) {
                const float ly = y0 + f + s * (0.12f + 0.15f * (float)i);
                if (ly + th > y1 - s * 0.08f) break;
                dl->AddRectFilled(ImVec2(lx0, ly),
                                  ImVec2(i == 2 ? lx1 - s * 0.20f : lx1, ly + th), line);
            }
        }
    }

    // folder with an up arrow, for the ... row
    static void DrawUpIcon(ImVec2 p, float s) {
        DrawFolderIcon(p, s);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 arrow = IM_COL32(70, 55, 15, 255);
        const float cx = p.x + s * 0.5f, cy = p.y + s * 0.60f;
        const float w  = s * 0.20f,      h  = s * 0.17f;
        dl->AddTriangleFilled(ImVec2(cx, cy - h),
                              ImVec2(cx - w, cy + h * 0.20f),
                              ImVec2(cx + w, cy + h * 0.20f), arrow);
        dl->AddRectFilled(ImVec2(cx - w * 0.34f, cy + h * 0.10f),
                          ImVec2(cx + w * 0.34f, cy + h * 0.95f), arrow);
    }

    // fixed grey, removable blue, optical disc, network plug bar
    static void DrawDriveIcon(ImVec2 p, float s, unsigned type) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
#ifdef _WIN32
        const bool removable = (type == DRIVE_REMOVABLE);
        const bool optical   = (type == DRIVE_CDROM);
        const bool network   = (type == DRIVE_REMOTE);
#else
        const bool removable = false, optical = false, network = false;
        (void)type;
#endif
        if (optical) {
            dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.40f, IM_COL32(196, 202, 210, 255), 20);
            dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.12f, IM_COL32(45, 45, 45, 255), 12);
            return;
        }
        const ImU32 body = removable ? IM_COL32(86, 156, 214, 255) : IM_COL32(150, 160, 172, 255);
        dl->AddRectFilled(ImVec2(p.x + s * 0.10f, p.y + s * 0.30f),
                          ImVec2(p.x + s * 0.90f, p.y + s * 0.70f), body, 2.0f);
        dl->AddCircleFilled(ImVec2(p.x + s * 0.72f, p.y + s * 0.50f), s * 0.055f, IM_COL32(50, 50, 50, 255));
        if (network)
            dl->AddRectFilled(ImVec2(p.x + s * 0.20f, p.y + s * 0.74f),
                              ImVec2(p.x + s * 0.80f, p.y + s * 0.84f), IM_COL32(120, 190, 120, 255), 1.5f);
    }

    static void Tip(const char* t) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t); }
    static void VSep() { ImGui::TextDisabled("|"); ImGui::SameLine(); }

    void SetError(std::string msg) { m_error = std::move(msg); m_openError = true; }

    static std::string Key(const fs::path& p) { return ToLower(p.string()); }

    static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    static const char* LeadingPad(float h) {
        static char pad[24];
        float sw = ImGui::CalcTextSize(" ").x; if (sw <= 0.0f) sw = 1.0f;
        int n = (int)(h / sw) + 1;
        if (n < 1) n = 1; if (n > 22) n = 22;
        for (int i = 0; i < n; ++i) pad[i] = ' ';
        pad[n] = '\0';
        return pad;
    }

    static std::string Ellipsize(const std::string& s, float maxW) {
        if (maxW <= 0.0f) return std::string();
        if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
        std::string out = s;
        while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > maxW)
            out.pop_back();
        return out + "...";
    }

    static void WrapTwoLines(const std::string& s, float maxW, std::string& l1, std::string& l2) {
        l1.clear(); l2.clear();
        if (maxW <= 0.0f) return;
        if (ImGui::CalcTextSize(s.c_str()).x <= maxW) { l1 = s; return; }
        size_t cut = 1;
        for (size_t i = 1; i <= s.size(); ++i) {
            if (ImGui::CalcTextSize(s.substr(0, i).c_str()).x > maxW) { cut = (i > 1) ? i - 1 : 1; break; }
            cut = i;
        }
        l1 = s.substr(0, cut);
        l2 = Ellipsize(s.substr(cut), maxW);
    }

    static bool SamePath(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        return fs::equivalent(a, b, ec);
    }

    static std::string ToLower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static bool IsHiddenPath(const fs::path& p) {
#ifdef _WIN32
        DWORD a = ::GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
#else
        std::string n = p.filename().string();
        return !n.empty() && n[0] == '.';
#endif
    }

    static int CompareNatural(const std::string& a, const std::string& b) {
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
                size_t si = i, sj = j;
                while (i < a.size() && std::isdigit((unsigned char)a[i])) ++i;
                while (j < b.size() && std::isdigit((unsigned char)b[j])) ++j;
                std::string na = a.substr(si, i - si), nb = b.substr(sj, j - sj);
                size_t za = na.find_first_not_of('0'); na = (za == std::string::npos) ? "0" : na.substr(za);
                size_t zb = nb.find_first_not_of('0'); nb = (zb == std::string::npos) ? "0" : nb.substr(zb);
                if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
                int c = na.compare(nb); if (c) return c < 0 ? -1 : 1;
            } else {
                char ca = (char)std::tolower((unsigned char)a[i]);
                char cb = (char)std::tolower((unsigned char)b[j]);
                if (ca != cb) return ca < cb ? -1 : 1;
                ++i; ++j;
            }
        }
        if (i < a.size()) return 1;
        if (j < b.size()) return -1;
        return 0;
    }

    static std::string TypeString(const Entry& e) {
        if (e.isDir) return "File folder";
        std::string ext = ToLower(e.path.extension().string());
        if (ext.empty()) return "File";
        static const std::unordered_map<std::string, std::string> known = {
            {".lua","Lua Source File"}, {".txt","Text Document"}, {".md","Markdown File"},
            {".cpp","C++ Source File"}, {".cc","C++ Source File"}, {".c","C Source File"},
            {".h","C/C++ Header File"}, {".hpp","C++ Header File"}, {".ini","Configuration File"},
            {".cfg","Configuration File"}, {".json","JSON File"}, {".xml","XML File"},
            {".png","PNG File"}, {".jpg","JPG File"}, {".jpeg","JPEG File"}, {".bmp","BMP File"},
            {".gif","GIF File"}, {".dll","Application Extension"}, {".exe","Application"},
            {".zip","Compressed (zipped) Folder"}, {".rar","RAR Archive"}, {".7z","7z Archive"},
            {".pdf","PDF Document"}, {".py","Python File"}, {".cs","C# Source File"},
            {".js","JavaScript File"}, {".ts","TypeScript File"}, {".html","HTML File"},
            {".css","CSS File"}, {".bat","Windows Batch File"}, {".ttf","TrueType Font"},
        };
        auto it = known.find(ext);
        if (it != known.end()) return it->second;
        std::string up = ext.substr(1);
        for (char& c : up) c = (char)std::toupper((unsigned char)c);
        return up + " File";
    }

    static std::time_t FileTimeToTimeT(fs::file_time_type ft) {
        using namespace std::chrono;
        auto sctp = time_point_cast<system_clock::duration>(
            ft - fs::file_time_type::clock::now() + system_clock::now());
        return system_clock::to_time_t(sctp);
    }

    static std::string FormatDate(std::time_t t) {
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%d-%b-%y %H:%M", &tm);   // e.g. 05-Jul-26 21:48
        return buf;
    }

    static std::string WithThousands(uintmax_t v) {
        std::string s = std::to_string(v), out;
        int cnt = 0;
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            if (cnt && cnt % 3 == 0) out.push_back(',');
            out.push_back(*it); ++cnt;
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    static std::string FormatSizeKB(uintmax_t bytes) {           // Explorer column: KB, rounded up
        uintmax_t kb = (bytes + 1023) / 1024;
        return WithThousands(kb) + " KB";
    }

    static std::string FormatSizeExact(uint64_t bytes) {         // status bar: "9.24 KB"
        const char* u[] = { "bytes", "KB", "MB", "GB", "TB" };
        double v = (double)bytes; int i = 0;
        while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
        char buf[64];
        if (i == 0) std::snprintf(buf, sizeof(buf), "%llu bytes", (unsigned long long)bytes);
        else        std::snprintf(buf, sizeof(buf), "%.2f %s", v, u[i]);
        return buf;
    }

    static std::string DateGroup(std::time_t t) {
        if (t == 0) return "Unknown";
        double days = std::difftime(std::time(nullptr), t) / 86400.0;
        if (days < 1)   return "Today";
        if (days < 2)   return "Yesterday";
        if (days < 7)   return "Earlier this week";
        if (days < 14)  return "Last week";
        if (days < 31)  return "Earlier this month";
        if (days < 62)  return "Last month";
        if (days < 365) return "Earlier this year";
        return "A long time ago";
    }

#ifdef _WIN32
    // suppress the "no disk in the drive" dialog while probing
    struct QuietErrors {
        UINT prev;
        QuietErrors()  { prev = ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX); }
        ~QuietErrors() { ::SetErrorMode(prev); }
    };

    static std::string DriveTypeName(unsigned t) {
        switch (t) {
            case DRIVE_REMOVABLE: return "USB Drive";
            case DRIVE_FIXED:     return "Local Disk";
            case DRIVE_REMOTE:    return "Network Drive";
            case DRIVE_CDROM:     return "DVD Drive";
            case DRIVE_RAMDISK:   return "RAM Disk";
            default:              return "Drive";
        }
    }

    static void QueryDrive(DriveInfo& d) {
        QuietErrors quiet;
        const std::wstring root = d.root.wstring();

        d.type = ::GetDriveTypeW(root.c_str());

        wchar_t name[MAX_PATH + 1] = {};
        DWORD serial = 0, maxComp = 0, flags = 0;
        wchar_t fsName[MAX_PATH + 1] = {};
        if (::GetVolumeInformationW(root.c_str(), name, MAX_PATH, &serial, &maxComp, &flags,
                                    fsName, MAX_PATH))
            d.label = Narrow(name);
        else
            d.label.clear();

        ULARGE_INTEGER freeToCaller{}, total{}, freeTotal{};
        if (::GetDiskFreeSpaceExW(root.c_str(), &freeToCaller, &total, &freeTotal) && total.QuadPart) {
            d.total    = (uint64_t)total.QuadPart;
            d.freeSp   = (uint64_t)freeToCaller.QuadPart;
            d.hasSpace = true;
        } else {
            d.total = d.freeSp = 0;
            d.hasSpace = false;
        }

        d.display = (d.label.empty() ? DriveTypeName(d.type) : d.label) + " (" + d.letter + ")";
    }

    static std::string Narrow(const wchar_t* w) {
        if (!w || !*w) return std::string();
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return std::string();
        std::string out((size_t)n - 1, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
        return out;
    }
#endif

    void RebuildDrives() {
        m_drives.clear();
#ifdef _WIN32
        m_driveMask = ::GetLogicalDrives();
        for (char c = 'A'; c <= 'Z'; ++c) {
            if (!(m_driveMask & (1u << (c - 'A')))) continue;
            DriveInfo d;
            d.letter = std::string(1, c) + ":";
            d.root   = d.letter + "\\";
            QueryDrive(d);
            m_drives.push_back(std::move(d));
        }
#else
        DriveInfo d;
        d.letter = "/";
        d.root   = "/";
        d.display = "Filesystem (/)";
        m_drives.push_back(std::move(d));
#endif
    }

    // one bitmask call per 2s, volumes re-queried only when letters change
    void PollDrives() {
#ifdef _WIN32
        const double now = ImGui::GetTime();
        if (now - m_lastDrivePoll < 2.0) return;
        m_lastDrivePoll = now;

        const unsigned long mask = ::GetLogicalDrives();
        if (mask != m_driveMask) {
            RebuildDrives();
            m_tree.clear();
            return;
        }
        // only drives that are cheap to ask
        for (DriveInfo& d : m_drives) {
            if (d.type != DRIVE_FIXED && d.type != DRIVE_REMOVABLE) continue;
            QuietErrors quiet;
            ULARGE_INTEGER freeToCaller{}, total{}, freeTotal{};
            if (::GetDiskFreeSpaceExW(d.root.wstring().c_str(), &freeToCaller, &total, &freeTotal)
                && total.QuadPart) {
                d.total    = (uint64_t)total.QuadPart;
                d.freeSp   = (uint64_t)freeToCaller.QuadPart;
                d.hasSpace = true;
            }
        }
#endif
    }

#ifdef _WIN32
    static fs::path KnownFolder(REFKNOWNFOLDERID id) {
        PWSTR w = nullptr;
        fs::path result;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &w))) result = w;
        if (w) CoTaskMemFree(w);
        return result;
    }

    struct ComInit {
        bool inited = false;
        ComInit() {
            HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            inited = SUCCEEDED(hr);          // RPC_E_CHANGED_MODE means someone else owns it
        }
        ~ComInit() { if (inited) ::CoUninitialize(); }
    };

    // the real Quick access shell folder, filesystem folders only
    static std::vector<fs::path> ShellQuickAccess() {
        std::vector<fs::path> out;
        ComInit com;

        IShellItem* folder = nullptr;
        if (FAILED(::SHCreateItemFromParsingName(
                L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}", nullptr,
                IID_PPV_ARGS(&folder))) || !folder)
            return out;

        IEnumShellItems* en = nullptr;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en))) && en) {
            IShellItem* item = nullptr;
            while (en->Next(1, &item, nullptr) == S_OK && item) {
                SFGAOF attrs = 0;
                PWSTR  path  = nullptr;
                if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attrs)) && (attrs & SFGAO_FOLDER) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out.push_back(fs::path(path));
                }
                if (path) ::CoTaskMemFree(path);
                item->Release();
                item = nullptr;
                if (out.size() >= 64) break;
            }
            en->Release();
        }
        folder->Release();
        return out;
    }
#endif

    void BuildSidebar() {
        m_quick.clear();
        std::unordered_set<std::string> seen;

        auto push = [&](const std::string& label, const fs::path& p, Src src) {
            if (p.empty()) return;
            const std::string k = Key(p);
            if (seen.count(k) || m_hidden.count(k)) return;
            std::error_code ec;
            if (!fs::is_directory(p, ec)) return;
            seen.insert(k);
            m_quick.push_back({ label.empty() ? p.filename().string() : label, p, src });
        };

#ifdef _WIN32
        // Home, then pins, then Windows Quick access, then the library folders
        push("Home", KnownFolder(FOLDERID_Profile), Src::Inherited);

        for (const std::string& s : m_userPins) push(std::string(), fs::path(s), Src::User);

        for (const fs::path& p : ShellQuickAccess())
            push(p.filename().string(), p, Src::Inherited);

        struct KF { const char* label; const KNOWNFOLDERID* id; };
        const KF kfs[] = {
            { "Desktop",   &FOLDERID_Desktop   },
            { "Downloads", &FOLDERID_Downloads },
            { "Documents", &FOLDERID_Documents },
            { "Pictures",  &FOLDERID_Pictures  },
            { "Music",     &FOLDERID_Music     },
            { "Videos",    &FOLDERID_Videos    },
        };
        for (const KF& kf : kfs) push(kf.label, KnownFolder(*kf.id), Src::Inherited);
#else
        if (const char* home = std::getenv("HOME")) {
            fs::path h(home);
            push("Home", h, Src::Inherited);
            for (const std::string& s : m_userPins) push(std::string(), fs::path(s), Src::User);
            const char* subs[] = { "Desktop", "Downloads", "Documents", "Pictures", "Music", "Videos" };
            for (const char* s : subs) push(s, h / s, Src::Inherited);
        }
#endif
    }

    static fs::path DefaultConfigPath() {
#ifdef _WIN32
        PWSTR w = nullptr;
        fs::path base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &w)) && w) base = w;
        if (w) CoTaskMemFree(w);
        if (base.empty()) return {};
        return base / "imex" / "FileBrowser.ini";
#else
        if (const char* h = std::getenv("HOME")) return fs::path(h) / ".config" / "imex_filebrowser.ini";
        return {};
#endif
    }

    void LoadConfig() {
        m_userPins.clear();
        m_hidden.clear();
        if (m_configPath.empty()) return;

        std::ifstream in(m_configPath);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const std::string val = line.substr(eq + 1);
            if      (key == "pin")     { if (!val.empty()) m_userPins.push_back(val); }
            else if (key == "hide")    { if (!val.empty()) m_hidden.insert(ToLower(val)); }
            else if (key == "view")    { int v = std::atoi(val.c_str()); if (v >= 0 && v <= 3) m_view_mode = (ViewMode)v; }
            else if (key == "hidden")  { m_showHidden = std::atoi(val.c_str()) != 0; }
            else if (key == "scale")   { float f = (float)std::atof(val.c_str()); if (f >= 0.6f && f <= 2.0f) m_uiScale = f; }
            else if (key == "sidebar") {
                const float f = (float)std::atof(val.c_str());
                if (f >= 150.0f && f <= 900.0f) { m_sidebarW = f; m_sidebarUser = true; }
            }
        }
    }

    void SaveConfig() {
        if (m_configPath.empty()) return;
        std::error_code ec;
        fs::create_directories(m_configPath.parent_path(), ec);

        std::ofstream out(m_configPath, std::ios::trunc);
        if (!out) return;
        out << "# filebrowser.h settings\n";
        out << "view="    << (int)m_view_mode << "\n";
        out << "hidden="  << (m_showHidden ? 1 : 0) << "\n";
        out << "scale="   << m_uiScale << "\n";
        if (m_sidebarUser) out << "sidebar=" << m_sidebarW << "\n";
        for (const std::string& p : m_userPins) out << "pin="  << p << "\n";
        for (const std::string& p : m_hidden)   out << "hide=" << p << "\n";
    }
};

} // namespace imex
