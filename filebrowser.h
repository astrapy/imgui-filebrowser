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
#include <cmath>
#include <ctime>
#include <chrono>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <objbase.h>
  #include <shlobj.h>
  #include <shobjidl.h>
  #include <shellapi.h>
  #pragma comment(lib, "Shell32.lib")
  #pragma comment(lib, "Ole32.lib")
  #pragma comment(lib, "Uuid.lib")
#endif

namespace fb {

namespace fs = std::filesystem;

class FileBrowser {
public:
    enum class Action { None, Open, Cancel };
    enum class View   { Details = 0, List = 1, Tiles = 2, Icons = 3 };
    enum class Sort   { Name = 0, Date = 1, Kind = 2, Size = 3 };

    FileBrowser() {
        m_configPath = DefaultConfigPath();
        LoadConfig();
        ScanDrives();
        BuildNav();
        m_open.insert("::thispc");

        fs::path start;
#ifdef _WIN32
        start = KnownFolder(FOLDERID_Profile);
#else
        if (const char* h = std::getenv("HOME")) start = h;
#endif
        std::error_code ec;
        if (start.empty() || !fs::exists(start, ec)) start = fs::current_path(ec);
        Navigate(start, false);
    }

    // fill the current window, return the action taken this frame
    Action Draw() { return Body(m_footer ? FooterH() : 0.0f); }

    // modal variant, call OpenPopup with the same title first
    Action DrawModal(const char* title, bool foldersOnly = false) {
        Action r = Action::None;
        ImGui::SetNextWindowSize(ImVec2(920.0f, 590.0f), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool open = ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        if (open) {
            m_foldersOnly = foldersOnly;
            r = Body(FooterH());
            if (r != Action::None) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        m_foldersOnly = false;
        return r;
    }

    const fs::path& CurrentPath() const { return m_cwd; }

    fs::path SelectedPath() const {
        if (m_sel.empty()) return {};
        const size_t i = *m_sel.begin();
        return i < m_all.size() ? m_all[i].path : fs::path();
    }

    std::vector<fs::path> SelectedPaths() const {
        std::vector<fs::path> out;
        out.reserve(m_sel.size());
        for (size_t i : m_sel) if (i < m_all.size()) out.push_back(m_all[i].path);
        return out;
    }

    void Navigate(const fs::path& p, bool record = true) {
        std::error_code ec;
        fs::path target = fs::weakly_canonical(p, ec);
        if (ec || target.empty()) target = p;
        if (!fs::exists(target, ec) || !fs::is_directory(target, ec)) {
            Fail("That location could not be opened.\n" + p.string());
            return;
        }
        if (record && !m_cwd.empty() && target != m_cwd) {
            m_back.push_back(m_cwd);
            m_fwd.clear();
        }
        m_cwd = target;
        m_sel.clear();
        m_anchor = -1;
        m_renaming = -1;
        m_editPath = false;
        m_search[0] = '\0';
        Reload();
    }

    void  SetScale(float s)   { m_scale = Clamp(s, 0.7f, 2.0f); Save(); }
    float Scale() const       { return m_scale; }
    void  SetView(View v)     { m_mode = v; Save(); }
    View  ViewMode() const    { return m_mode; }
    void  ShowFooter(bool on) { m_footer = on; }
    bool  FooterShown() const { return m_footer; }
    void  ShowHidden(bool on) { m_showHidden = on; Save(); Reload(); }
    bool  HiddenShown() const { return m_showHidden; }
    void  SetConfigPath(const fs::path& p) { m_configPath = p; LoadConfig(); BuildNav(); }
    const fs::path& ConfigPath() const     { return m_configPath; }

    bool IsPinned(const fs::path& p) const {
        const std::string k = Key(p);
        for (const std::string& s : m_pins) if (Lower(s) == k) return true;
        return false;
    }
    void Pin(const fs::path& p) {
        std::error_code ec;
        if (!fs::is_directory(p, ec)) return;
        m_hidden.erase(Key(p));
        if (!IsPinned(p)) m_pins.push_back(p.string());
        Save();
        BuildNav();
    }
    void Unpin(const fs::path& p) {
        const std::string k = Key(p);
        m_pins.erase(std::remove_if(m_pins.begin(), m_pins.end(),
                                    [&](const std::string& s) { return Lower(s) == k; }),
                     m_pins.end());
        Save();
        BuildNav();
    }

private:
    // windows 11 dark palette
    struct Palette {
        ImU32 chrome, body, pill, pillHot, line;
        ImU32 text, dim, faint;
        ImU32 hover, sel;
        ImU32 accent, accentInk, warn;
    };
    static Palette Colors() {
        Palette p;
        p.chrome    = IM_COL32(0x20, 0x20, 0x20, 255);
        p.body      = IM_COL32(0x19, 0x19, 0x19, 255);
        p.pill      = IM_COL32(255, 255, 255, 13);
        p.pillHot   = IM_COL32(255, 255, 255, 21);
        p.line      = IM_COL32(255, 255, 255, 15);
        p.text      = IM_COL32(0xF0, 0xF0, 0xF0, 255);
        p.dim       = IM_COL32(0xAD, 0xAD, 0xAD, 255);
        p.faint     = IM_COL32(0x71, 0x71, 0x71, 255);
        p.hover     = IM_COL32(255, 255, 255, 10);
        p.sel       = IM_COL32(255, 255, 255, 17);
        p.accent    = IM_COL32(0x60, 0xCD, 0xFF, 255);
        p.accentInk = IM_COL32(0x00, 0x2B, 0x44, 255);
        p.warn      = IM_COL32(0xE0, 0x51, 0x45, 255);
        return p;
    }

    struct Entry {
        std::string name;
        fs::path    path;
        bool        isDir  = false;
        bool        hidden = false;
        uintmax_t   size   = 0;
        std::time_t mtime  = 0;
        std::string kind;
        int         cat    = 0;
    };

    enum class Origin { System, User };
    enum class NavKind { Home, Folder, PC, Drive };

    struct NavItem {
        std::string label;
        fs::path    path;
        Origin      origin = Origin::System;
    };

    struct Drive {
        fs::path    root;
        std::string letter, label, caption;
        unsigned    type  = 0;
        uint64_t    total = 0, freeb = 0;
        bool        space = false;
    };

    struct Branch {
        bool                  loaded  = false;
        bool                  hasKids = true;
        std::vector<fs::path> kids;
    };

    // list row, either group heading or file entry
    struct Row {
        bool        heading = false;
        size_t      index   = 0;
        std::string text;
    };

    fs::path                   m_cwd;
    std::vector<Entry>         m_all;
    std::vector<size_t>        m_view;
    std::vector<Row>           m_rows;
    std::unordered_set<size_t> m_sel;
    int                        m_anchor = -1;

    std::vector<fs::path> m_back, m_fwd;
    std::vector<NavItem>  m_nav;
    std::vector<Drive>    m_drives;

    std::unordered_map<std::string, Branch> m_tree;
    std::unordered_set<std::string>         m_open;
    float m_navW     = 0.0f;
    bool  m_navSized = false;
    bool  m_navUser  = false;

    unsigned long m_driveMask = 0;
    double        m_lastPoll  = -1000.0;

    Sort  m_sort  = Sort::Name;
    bool  m_asc   = true;
    bool  m_group = false;
    bool  m_showHidden = false;
    View  m_mode  = View::Details;
    float m_scale = 0.9f;
    bool  m_footer = true;
    bool  m_foldersOnly = false;

    float m_colDate = 150.0f;
    float m_colKind = 110.0f;
    float m_colSize = 88.0f;

    char m_search[128] = "";
    bool m_searchActive = false;
    bool m_editPath  = false;
    bool m_editFocus = false;
    char m_pathBuf[520] = "";

    std::vector<fs::path> m_clip;
    bool m_cut = false;

    int  m_renaming = -1;
    char m_renameBuf[260] = "";
    bool m_renameFocus = false;

    bool        m_wantError = false, m_wantDelete = false;
    std::string m_error;
    std::vector<fs::path> m_doomed;

    fs::path                        m_configPath;
    std::vector<std::string>        m_pins;
    std::unordered_set<std::string> m_hidden;

    bool     m_activated = false;
    fs::path m_navReq;
    bool     m_navPending = false;

    std::vector<fs::path> m_dragging, m_dropSrc;
    fs::path m_dropDst;
    bool     m_dropMove = false, m_dropQueued = false;

    Palette C;

    float S()        const { return m_scale; }
    float RowH()     const { return 29.0f * S(); }
    float Pad()      const { return 10.0f * S(); }
    float Rr()       const { return 4.0f * S(); }
    float AddrH()    const { return 40.0f * S(); }
    float CmdH()     const { return 34.0f * S(); }
    float StatusH()  const { return 26.0f * S(); }
    float FooterH()  const { return 30.0f * S() + 10.0f * S(); }

    // commit a new layout cursor position
    static void Seek(ImVec2 p) { ImGui::SetCursorScreenPos(p); ImGui::Dummy(ImVec2(0.0f, 0.0f)); }

    // one frame of the whole browser, reserve leaves room for the footer
    Action Body(float reserve) {
        C = Colors();
        m_activated = false;
        Action act = Action::None;

        PushStyle();
        PollDrives();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail  = ImGui::GetContentRegionAvail();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), C.chrome);
        const float chromeH = AddrH() + CmdH();
        const float bottomH = StatusH() + reserve;
        dl->AddRectFilled(ImVec2(origin.x, origin.y + chromeH),
                          ImVec2(origin.x + avail.x, origin.y + avail.y - bottomH), C.body);
        dl->AddLine(ImVec2(origin.x, origin.y + chromeH),
                    ImVec2(origin.x + avail.x, origin.y + chromeH), C.line, 1.0f);
        dl->AddLine(ImVec2(origin.x, origin.y + avail.y - bottomH),
                    ImVec2(origin.x + avail.x, origin.y + avail.y - bottomH), C.line, 1.0f);

        AddressRow();
        CommandRow();

        float bodyH = avail.y - chromeH - bottomH;
        if (bodyH < 80.0f) bodyH = 80.0f;
        Panes(bodyH);
        StatusBar();

        if (m_footer && reserve > 0.0f) act = Footer();
        if (m_activated) act = Action::Open;

        Popups();
        ApplyDrop();
        if (m_navPending) { m_navPending = false; Navigate(m_navReq); }

        PopStyle();
        return act;
    }

    // push a full style block so the host application style stays untouched
    void PushStyle() {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,     Rr());
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,     8.0f * S());
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,     11.0f * S());
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,     ImVec2(Pad(), Pad()));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,       ImVec2(6.0f * S(), 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,      ImVec2(9.0f * S(), 5.0f * S()));

        ImGui::PushStyleColor(ImGuiCol_Text,          C.text);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled,  C.faint);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,       0);
        ImGui::PushStyleColor(ImGuiCol_PopupBg,       IM_COL32(0x2C, 0x2C, 0x2C, 252));
        ImGui::PushStyleColor(ImGuiCol_Border,        C.line);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,       0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
        ImGui::PushStyleColor(ImGuiCol_Header,        C.sel);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, C.hover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  C.sel);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,   0);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(255, 255, 255, 30));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(255, 255, 255, 55));
        ImGui::PushStyleColor(ImGuiCol_Separator,     C.line);
    }
    void PopStyle() {
        ImGui::PopStyleColor(15);
        ImGui::PopStyleVar(8);
    }

    // nav arrows, breadcrumb pill and search pill
    void AddressRow() {
        const float h = AddrH();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cy = p.y + h * 0.5f;

        ImGui::Dummy(ImVec2(w, h));
        const ImVec2 after = ImGui::GetCursorScreenPos();

        const float bh = 30.0f * S();
        float x = p.x + Pad() + bh * 0.5f;
        if (NavBtn(dl, "##back", ImVec2(x, cy), 0, !m_back.empty())) GoBack();
        x += bh + 2.0f * S();
        if (NavBtn(dl, "##fwd", ImVec2(x, cy), 1, !m_fwd.empty())) GoFwd();
        x += bh + 2.0f * S();
        if (NavBtn(dl, "##up", ImVec2(x, cy), 2, CanGoUp())) GoUp();
        x += bh + 2.0f * S();
        if (NavBtn(dl, "##refresh", ImVec2(x, cy), 3, true)) HardReload();
        x += bh * 0.5f + 10.0f * S();

        const float searchW = 220.0f * S();
        const float addrX0 = x;
        const float addrX1 = p.x + w - Pad() - searchW - 8.0f * S();
        AddressPill(dl, addrX0, addrX1, cy);
        SearchPill(dl, ImVec2(p.x + w - Pad(), cy), searchW);

        Seek(after);
    }

    void AddressPill(ImDrawList* dl, float x0, float x1, float cy) {
        const float h = 30.0f * S();
        const float lh = ImGui::GetTextLineHeight();
        const ImVec2 a(x0, cy - h * 0.5f);
        const ImVec2 b(x1, cy + h * 0.5f);
        if (b.x - a.x < 60.0f) return;

        if (m_editPath) {
            ImGui::SetCursorScreenPos(a);
            ImGui::SetNextItemWidth(b.x - a.x);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, C.pill);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f * S(), (h - lh) * 0.5f));
            if (m_editFocus) { ImGui::SetKeyboardFocusHere(); m_editFocus = false; }
            const bool go = ImGui::InputText("##path", m_pathBuf, sizeof(m_pathBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue |
                                             ImGuiInputTextFlags_AutoSelectAll);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            const bool esc  = ImGui::IsKeyPressed(ImGuiKey_Escape);
            const bool gone = ImGui::IsItemDeactivated() && !go;
            if (go) { m_editPath = false; if (m_pathBuf[0]) Navigate(fs::path(m_pathBuf)); }
            else if (esc || gone) m_editPath = false;
            return;
        }

        const bool over = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b);
        dl->AddRectFilled(a, b, over ? C.pillHot : C.pill, Rr());
        dl->AddRect(a, b, C.line, Rr());

        std::vector<fs::path> chain;
        for (fs::path c = m_cwd;;) {
            chain.push_back(c);
            if (!c.has_parent_path() || c.parent_path() == c) break;
            c = c.parent_path();
        }
        std::reverse(chain.begin(), chain.end());

        float x = a.x + 5.0f * S();
        for (size_t i = 0; i < chain.size(); ++i) {
            std::string label = chain[i].filename().string();
            if (label.empty()) {
                label = chain[i].root_name().string();
                if (label.empty()) label = chain[i].string();
            }
            const float tw = ImGui::CalcTextSize(label.c_str()).x;
            const ImVec2 c0(x, a.y + 3.0f * S());
            const ImVec2 c1(x + tw + 14.0f * S(), b.y - 3.0f * S());
            if (c1.x > b.x - 22.0f * S()) {
                dl->AddText(ImVec2(x + 4.0f * S(), a.y + (h - lh) * 0.5f), C.faint, "...");
                x = b.x;
                break;
            }
            ImGui::SetCursorScreenPos(c0);
            ImGui::PushID((int)i);
            const bool hit = ImGui::InvisibleButton("##crumb", ImVec2(c1.x - c0.x, c1.y - c0.y));
            const bool hot = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hot) dl->AddRectFilled(c0, c1, C.hover, Rr());
            dl->AddText(ImVec2(x + 7.0f * S(), a.y + (h - lh) * 0.5f),
                        i + 1 == chain.size() ? C.text : C.dim, label.c_str());
            if (hit) Nav(chain[i]);
            x = c1.x;
            if (i + 1 < chain.size()) {
                Chevron(dl, ImVec2(x + 5.0f * S(), cy), 2.8f * S(), C.faint, 0.0f);
                x += 12.0f * S();
            }
        }

        if (b.x - x > 22.0f * S()) {
            ImGui::SetCursorScreenPos(ImVec2(x, a.y));
            if (ImGui::InvisibleButton("##pathblank", ImVec2(b.x - x, h))) BeginEditPath();
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
    }

    void SearchPill(ImDrawList* dl, ImVec2 rightMid, float width) {
        const float h = 30.0f * S();
        const ImVec2 a(rightMid.x - width, rightMid.y - h * 0.5f);
        const ImVec2 b(rightMid.x, rightMid.y + h * 0.5f);

        dl->AddRectFilled(a, b, m_searchActive ? C.pillHot : C.pill, Rr());
        dl->AddRect(a, b, m_searchActive ? C.accent : C.line, Rr());
        SearchGlyph(dl, ImVec2(a.x + h * 0.46f, rightMid.y), 5.6f * S(),
                    m_searchActive ? C.text : C.faint);

        ImGui::SetCursorScreenPos(ImVec2(a.x + h * 0.88f, a.y));
        ImGui::SetNextItemWidth(width - h * 0.88f - 8.0f * S());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(0.0f, (h - ImGui::GetTextLineHeight()) * 0.5f));
        if (ImGui::InputTextWithHint("##search", "Search", m_search, sizeof(m_search))) Rebuild();
        m_searchActive = ImGui::IsItemActive();
        ImGui::PopStyleVar();
    }

    enum class Cmd { Cut, Copy, Paste, Rename, Del };

    // new button, edit icons, view menu and overflow
    void CommandRow() {
        const float h = CmdH();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cy = p.y + h * 0.5f;

        ImGui::Dummy(ImVec2(w, h));
        const ImVec2 after = ImGui::GetCursorScreenPos();

        float x = p.x + Pad();
        x = NewButton(dl, x, cy);
        x += 6.0f * S();
        dl->AddLine(ImVec2(x, cy - h * 0.26f), ImVec2(x, cy + h * 0.26f), C.line, 1.0f);
        x += 6.0f * S();

        const bool haveSel = !m_sel.empty();
        if (CmdBtn(dl, "##cut",    x, cy, Cmd::Cut,    haveSel)) Copy(true);
        if (CmdBtn(dl, "##copy",   x, cy, Cmd::Copy,   haveSel)) Copy(false);
        if (CmdBtn(dl, "##paste",  x, cy, Cmd::Paste,  !m_clip.empty())) Paste();
        if (CmdBtn(dl, "##rename", x, cy, Cmd::Rename, m_sel.size() == 1)) BeginRenameSelected();
        if (CmdBtn(dl, "##delete", x, cy, Cmd::Del,    haveSel)) AskDelete();

        x += 6.0f * S();
        dl->AddLine(ImVec2(x, cy - h * 0.26f), ImVec2(x, cy + h * 0.26f), C.line, 1.0f);
        x += 6.0f * S();

        x = ViewButton(dl, x, cy);
        x = MoreButton(dl, x, cy);
        (void)x;

        Seek(after);
    }

    float NewButton(ImDrawList* dl, float x, float cy) {
        const float h = 26.0f * S();
        const float lh = ImGui::GetTextLineHeight();
        const char* label = "New";
        const float tw = ImGui::CalcTextSize(label).x;
        const float bw = h + tw + 12.0f * S();

        ImGui::SetCursorScreenPos(ImVec2(x, cy - h * 0.5f));
        const bool hit = ImGui::InvisibleButton("##new", ImVec2(bw, h));
        const bool hot = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (hot) dl->AddRectFilled(a, b, C.hover, Rr());

        const ImU32 ink = hot ? C.text : C.dim;
        const float r = 4.6f * S();
        const ImVec2 pm(a.x + h * 0.5f, cy);
        dl->AddRectFilled(ImVec2(pm.x - r, pm.y - 1.0f), ImVec2(pm.x + r, pm.y + 1.0f), ink);
        dl->AddRectFilled(ImVec2(pm.x - 1.0f, pm.y - r), ImVec2(pm.x + 1.0f, pm.y + r), ink);
        dl->AddText(ImVec2(a.x + h, cy - lh * 0.5f), ink, label);

        if (hit) NewFolder();
        return b.x;
    }

    bool CmdBtn(ImDrawList* dl, const char* id, float& x, float cy, Cmd kind, bool enabled) {
        const float h = 28.0f * S();
        ImGui::SetCursorScreenPos(ImVec2(x, cy - h * 0.5f));
        ImGui::BeginDisabled(!enabled);
        ImGui::InvisibleButton(id, ImVec2(h, h));
        const bool hit = ImGui::IsItemActivated();
        const bool hot = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (hot && enabled) dl->AddRectFilled(a, b, C.hover, Rr());

        const ImU32 ink = !enabled ? C.faint : (hot ? C.text : C.dim);
        CmdGlyph(dl, ImVec2((a.x + b.x) * 0.5f, cy), 5.8f * S(), kind, ink);
        x = b.x + 3.0f * S();
        return hit && enabled;
    }

    static void CmdGlyph(ImDrawList* dl, ImVec2 m, float r, Cmd kind, ImU32 ink) {
        switch (kind) {
            case Cmd::Cut:
                dl->AddLine(ImVec2(m.x - r * 0.8f, m.y - r), ImVec2(m.x + r * 0.55f, m.y + r * 0.6f), ink, 1.6f);
                dl->AddLine(ImVec2(m.x + r * 0.8f, m.y - r), ImVec2(m.x - r * 0.55f, m.y + r * 0.6f), ink, 1.6f);
                dl->AddCircle(ImVec2(m.x - r * 0.72f, m.y + r * 0.85f), r * 0.34f, ink, 12, 1.6f);
                dl->AddCircle(ImVec2(m.x + r * 0.72f, m.y + r * 0.85f), r * 0.34f, ink, 12, 1.6f);
                break;
            case Cmd::Copy:
                dl->AddRect(ImVec2(m.x - r * 0.4f, m.y - r), ImVec2(m.x + r, m.y + r * 0.4f), ink, r * 0.25f, 0, 1.5f);
                dl->AddRect(ImVec2(m.x - r, m.y - r * 0.4f), ImVec2(m.x + r * 0.4f, m.y + r), ink, r * 0.25f, 0, 1.5f);
                break;
            case Cmd::Paste:
                dl->AddRect(ImVec2(m.x - r * 0.8f, m.y - r * 0.85f), ImVec2(m.x + r * 0.8f, m.y + r), ink, r * 0.25f, 0, 1.5f);
                dl->AddRectFilled(ImVec2(m.x - r * 0.35f, m.y - r * 1.05f), ImVec2(m.x + r * 0.35f, m.y - r * 0.6f), ink, r * 0.15f);
                break;
            case Cmd::Rename:
                dl->AddLine(ImVec2(m.x - r * 0.55f, m.y + r * 0.55f), ImVec2(m.x + r * 0.75f, m.y - r * 0.75f), ink, 2.1f);
                dl->AddTriangleFilled(ImVec2(m.x - r * 0.95f, m.y + r * 0.95f),
                                      ImVec2(m.x - r * 0.45f, m.y + r * 0.75f),
                                      ImVec2(m.x - r * 0.75f, m.y + r * 0.45f), ink);
                break;
            case Cmd::Del:
                dl->AddLine(ImVec2(m.x - r * 0.9f, m.y - r * 0.6f), ImVec2(m.x + r * 0.9f, m.y - r * 0.6f), ink, 1.6f);
                dl->AddRectFilled(ImVec2(m.x - r * 0.3f, m.y - r * 0.95f), ImVec2(m.x + r * 0.3f, m.y - r * 0.6f), ink);
                dl->AddRect(ImVec2(m.x - r * 0.62f, m.y - r * 0.6f), ImVec2(m.x + r * 0.62f, m.y + r), ink, r * 0.2f, 0, 1.5f);
                dl->AddLine(ImVec2(m.x - r * 0.22f, m.y - r * 0.22f), ImVec2(m.x - r * 0.22f, m.y + r * 0.6f), ink, 1.4f);
                dl->AddLine(ImVec2(m.x + r * 0.22f, m.y - r * 0.22f), ImVec2(m.x + r * 0.22f, m.y + r * 0.6f), ink, 1.4f);
                break;
        }
    }

    float ViewButton(ImDrawList* dl, float x, float cy) {
        const float h = 26.0f * S();
        const float lh = ImGui::GetTextLineHeight();
        const char* label = "View";
        const float tw = ImGui::CalcTextSize(label).x;
        const float bw = tw + 26.0f * S();

        ImGui::SetCursorScreenPos(ImVec2(x, cy - h * 0.5f));
        if (ImGui::InvisibleButton("##view", ImVec2(bw, h))) ImGui::OpenPopup("##viewmenu");
        const bool hot = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (hot) dl->AddRectFilled(a, b, C.hover, Rr());
        const ImU32 ink = hot ? C.text : C.dim;
        dl->AddText(ImVec2(a.x + 7.0f * S(), cy - lh * 0.5f), ink, label);
        Chevron(dl, ImVec2(b.x - 10.0f * S(), cy), 2.8f * S(), ink, 90.0f);

        ImGui::SetNextWindowPos(ImVec2(a.x, b.y + 2.0f));
        if (ImGui::BeginPopup("##viewmenu")) {
            if (ImGui::MenuItem("Details",     "Ctrl+Shift+1", m_mode == View::Details)) SetView(View::Details);
            if (ImGui::MenuItem("List",        "Ctrl+Shift+2", m_mode == View::List))    SetView(View::List);
            if (ImGui::MenuItem("Tiles",       "Ctrl+Shift+3", m_mode == View::Tiles))   SetView(View::Tiles);
            if (ImGui::MenuItem("Large icons", "Ctrl+Shift+4", m_mode == View::Icons))   SetView(View::Icons);
            ImGui::Separator();
            if (ImGui::MenuItem("Group by date", nullptr, m_group)) { m_group = !m_group; Save(); Rebuild(); }
            ImGui::EndPopup();
        }
        return b.x + 2.0f * S();
    }

    float MoreButton(ImDrawList* dl, float x, float cy) {
        const float h = 26.0f * S();
        ImGui::SetCursorScreenPos(ImVec2(x, cy - h * 0.5f));
        if (ImGui::InvisibleButton("##more", ImVec2(h, h))) ImGui::OpenPopup("##moremenu");
        const bool hot = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (hot) dl->AddRectFilled(a, b, C.hover, Rr());
        const ImU32 ink = hot ? C.text : C.dim;
        for (int i = -1; i <= 1; ++i)
            dl->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f + (float)i * 4.4f * S(), cy), 1.5f * S(), ink, 8);

        ImGui::SetNextWindowPos(ImVec2(a.x, b.y + 2.0f));
        if (ImGui::BeginPopup("##moremenu")) {
            if (ImGui::MenuItem("Show hidden files", "Ctrl+H", m_showHidden)) {
                m_showHidden = !m_showHidden; Save(); Reload();
            }
            if (!IsPinned(m_cwd)) { if (ImGui::MenuItem("Pin this folder")) Pin(m_cwd); }
            else                  { if (ImGui::MenuItem("Unpin this folder")) Unpin(m_cwd); }
            ImGui::EndPopup();
        }
        return b.x;
    }

    // sidebar and file pane with a draggable splitter between them
    void Panes(float h) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        ImDrawList*  dl = ImGui::GetWindowDrawList();

        SizeNav();
        m_navW = Clamp(m_navW, 170.0f, w - 260.0f > 180.0f ? w - 260.0f : 180.0f);

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 4.0f * S()));
        ImGui::BeginChild("##nav", ImVec2(m_navW, h - 4.0f * S()), false, ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 1.0f * S()));
        NavPane();
        ImGui::PopStyleVar();
        ImGui::EndChild();

        dl->AddLine(ImVec2(p.x + m_navW + 3.0f, p.y + 6.0f * S()),
                    ImVec2(p.x + m_navW + 3.0f, p.y + h - 6.0f * S()), C.line, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + m_navW, p.y));
        Splitter(h);

        ImGui::SetCursorScreenPos(ImVec2(p.x + m_navW + 7.0f, p.y + 4.0f * S()));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##list", ImVec2(w - m_navW - 7.0f, h - 4.0f * S()), false,
                          ImGuiWindowFlags_NoBackground);
        if (m_mode == View::Details) Details();
        else                         Grid();
        BackgroundMenu();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        Shortcuts();
        Seek(ImVec2(p.x, p.y + h));
    }

    void Splitter(float h) {
        ImGui::InvisibleButton("##split", ImVec2(7.0f, h));
        if (ImGui::IsItemActive()) { m_navW += ImGui::GetIO().MouseDelta.x; m_navUser = true; }
        if (ImGui::IsItemDeactivated()) Save();
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float cx = (a.x + b.x) * 0.5f;
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(cx - 1.0f, a.y + h * 0.3f), ImVec2(cx + 1.0f, b.y - h * 0.3f), C.accent, 1.0f);
        }
    }

    void SizeNav() {
        if (m_navUser || m_navSized) return;
        m_navSized = true;
        float w = ImGui::CalcTextSize("This PC").x;
        for (const NavItem& n : m_nav)  w = std::max(w, ImGui::CalcTextSize(n.label.c_str()).x);
        for (const Drive& d : m_drives) w = std::max(w, ImGui::CalcTextSize(d.caption.c_str()).x + 15.0f * S());
        w += ImGui::GetTextLineHeight() + 72.0f * S();
        m_navW = Clamp(w, 200.0f, 330.0f);
    }

    void SideSep() {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(w, 9.0f * S()));
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p.x + 10.0f * S(), p.y + 4.5f * S()),
            ImVec2(p.x + w - 10.0f * S(), p.y + 4.5f * S()), C.line, 1.0f);
    }

    void NavPane() {
#ifdef _WIN32
        const fs::path home = KnownFolder(FOLDERID_Profile);
#else
        fs::path home;
        if (const char* h = std::getenv("HOME")) home = h;
#endif
        NavRow(home, "Home", NavKind::Home, nullptr, Origin::System, 0);
        SideSep();
        for (const NavItem& n : m_nav) {
            if (SamePath(n.path, home)) continue;
            NavRow(n.path, n.label, NavKind::Folder, nullptr, n.origin, 0);
        }
        SideSep();
        NavRow(fs::path(), "This PC", NavKind::PC, nullptr, Origin::System, 0);
        if (m_open.count("::thispc"))
            for (const Drive& d : m_drives)
                NavRow(d.root, d.caption, NavKind::Drive, &d, Origin::System, 1);
        ImGui::Dummy(ImVec2(0.0f, Pad()));
    }

    // one sidebar row, depth indents the tree, drives get a capacity bar
    void NavRow(const fs::path& path, const std::string& label,
                NavKind kind, const Drive* drive, Origin origin, int depth) {
        if (depth > 12) return;

        const bool isPC = kind == NavKind::PC;
        const std::string key = isPC ? std::string("::thispc") : Key(path);
        Branch* node = isPC ? nullptr : &m_tree[key];

        const float h  = RowH();
        const float lh = ImGui::GetTextLineHeight();
        const bool hasBar = drive && drive->space && drive->total > 0;
        const float total = hasBar ? h + 8.0f * S() : h;
        const float indent = 14.0f * S() * (float)depth;

        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::Dummy(ImVec2(w, total));
        const ImVec2 after = ImGui::GetCursorScreenPos();

        ImGui::PushID(key.c_str());

        const float tx = a.x + 6.0f * S() + indent;
        const float tw = 15.0f * S();
        bool open = m_open.count(key) > 0;
        bool twirlHot = false;
        const bool expandable = isPC || (node && node->hasKids);
        if (expandable) {
            ImGui::SetCursorScreenPos(ImVec2(tx, a.y + (h - tw) * 0.5f));
            if (ImGui::InvisibleButton("##twirl", ImVec2(tw, tw))) {
                if (open) m_open.erase(key); else m_open.insert(key);
                open = !open;
            }
            twirlHot = ImGui::IsItemHovered();
        }

        const float bodyX = tx + tw + 2.0f * S();
        ImGui::SetCursorScreenPos(ImVec2(bodyX, a.y));
        const float bodyW = a.x + w - bodyX > 20.0f ? a.x + w - bodyX : 20.0f;
        ImGui::InvisibleButton("##row", ImVec2(bodyW, total));
        const bool hot     = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        if (!isPC) AcceptDrop(path);
        if (!isPC && ImGui::BeginPopupContextItem("##navctx")) {
            NavMenu(path, origin);
            ImGui::EndPopup();
        }

        const bool current = !isPC && SamePath(path, m_cwd);
        const ImVec2 r0(a.x + 2.0f * S(), a.y);
        const ImVec2 r1(a.x + w - 2.0f * S(), a.y + h);
        if (current)              dl->AddRectFilled(r0, r1, C.sel, Rr());
        else if (hot || twirlHot) dl->AddRectFilled(r0, r1, C.hover, Rr());
        if (current) {
            const float bh = h * 0.45f;
            dl->AddRectFilled(ImVec2(r0.x + 1.0f, a.y + (h - bh) * 0.5f),
                              ImVec2(r0.x + 4.0f, a.y + (h + bh) * 0.5f), C.accent, 2.0f);
        }

        if (expandable)
            Chevron(dl, ImVec2(tx + tw * 0.5f, a.y + h * 0.5f), 2.8f * S(),
                    twirlHot ? C.text : C.faint, open ? 90.0f : 0.0f);

        const float icon = lh * 1.15f;
        const ImVec2 ip(bodyX + 2.0f * S(), a.y + (h - icon) * 0.5f);
        switch (kind) {
            case NavKind::Home:   HomeGlyph(dl, ip, icon);               break;
            case NavKind::PC:     PCGlyph(dl, ip, icon);                 break;
            case NavKind::Drive:  DriveGlyph(dl, ip, icon, drive->type); break;
            default:              FolderGlyph(dl, ip, icon);             break;
        }

        const float lx = bodyX + icon + 9.0f * S();
        dl->AddText(ImVec2(lx, a.y + (h - lh) * 0.5f), current ? C.text : C.dim,
                    Elide(label, r1.x - lx - 18.0f * S()).c_str());

        if (origin == Origin::User)
            PinGlyph(dl, ImVec2(r1.x - 12.0f * S(), a.y + h * 0.5f), 4.0f * S(), C.faint);

        if (hasBar) {
            const float bx = bodyX + 2.0f * S();
            const float bw = r1.x - bx - 10.0f * S();
            const float by = a.y + h;
            const float bh = 3.5f * S();
            const double used = (double)(drive->total - drive->freeb) / (double)drive->total;
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh),
                              IM_COL32(255, 255, 255, 22), bh * 0.5f);
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + (float)(bw * used), by + bh),
                              used > 0.9 ? C.warn : C.accent, bh * 0.5f);
        }

        if (clicked) {
            if (isPC) { if (open) m_open.erase(key); else m_open.insert(key); }
            else Nav(path);
        }
        ImGui::PopID();
        Seek(after);

        if (!isPC && open && node) {
            LoadBranch(path, *node);
            for (const fs::path& kid : node->kids)
                NavRow(kid, kid.filename().string(), NavKind::Folder, nullptr, Origin::System, depth + 1);
        }
    }

    void NavMenu(const fs::path& path, Origin origin) {
        if (ImGui::MenuItem("Open")) Nav(path);
        ImGui::Separator();
        if (origin == Origin::User) {
            if (ImGui::MenuItem("Unpin")) Unpin(path);
        } else if (ImGui::MenuItem("Hide from sidebar")) {
            m_hidden.insert(Key(path));
            Save();
            BuildNav();
        }
        if (!m_hidden.empty() && ImGui::MenuItem("Restore hidden entries")) {
            m_hidden.clear();
            Save();
            BuildNav();
        }
    }

    void LoadBranch(const fs::path& p, Branch& node) {
        if (node.loaded) return;
        node.loaded = true;
        node.kids.clear();
        std::error_code ec;
        for (auto it = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2) || e2) continue;
            if (!m_showHidden && IsHidden(it->path())) continue;
            node.kids.push_back(it->path());
            if (node.kids.size() >= 2000) break;
        }
        std::sort(node.kids.begin(), node.kids.end(), [](const fs::path& x, const fs::path& y) {
            return Natural(x.filename().string(), y.filename().string()) < 0;
        });
        node.hasKids = !node.kids.empty();
    }

    void Details() {
        const float lh = ImGui::GetTextLineHeight();
        const float hh = 30.0f * S();
        const float w  = ImGui::GetContentRegionAvail().x;
        const float pad = 12.0f * S();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 hp = ImGui::GetCursorScreenPos();

        const float sizeX = hp.x + w - pad - m_colSize;
        const float kindX = sizeX - m_colKind;
        const float dateX = kindX - m_colDate;

        ImGui::Dummy(ImVec2(w, hh));
        const ImVec2 after = ImGui::GetCursorScreenPos();
        HeaderRow(dl, hp, w, pad, dateX, kindX, sizeX, hh, lh);
        dl->AddLine(ImVec2(hp.x + 2.0f, hp.y + hh - 1.0f), ImVec2(hp.x + w - pad, hp.y + hh - 1.0f),
                    C.line, 1.0f);
        Seek(after);

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##rows", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_NoBackground);
        UpRow(pad);
        ImGuiListClipper clipper;
        clipper.Begin((int)m_rows.size(), RowH());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                DetailRow((size_t)i, pad, dateX, kindX);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void HeaderRow(ImDrawList* dl, ImVec2 hp, float w, float pad,
                   float dateX, float kindX, float sizeX, float hh, float lh) {
        struct Col { const char* text; float x; Sort key; };
        const Col cols[] = {
            { "Name",          hp.x + pad, Sort::Name },
            { "Date modified", dateX,      Sort::Date },
            { "Type",          kindX,      Sort::Kind },
            { "Size",          sizeX,      Sort::Size },
        };

        for (int i = 0; i < 4; ++i) {
            const float x1 = (i == 3) ? hp.x + w - pad : cols[i + 1].x;
            const float cw = x1 - cols[i].x - 8.0f > 20.0f ? x1 - cols[i].x - 8.0f : 20.0f;
            ImGui::SetCursorScreenPos(ImVec2(cols[i].x, hp.y));
            ImGui::PushID(i);
            const bool hit = ImGui::InvisibleButton("##col", ImVec2(cw, hh));
            const bool hot = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hit) {
                if (m_sort == cols[i].key) m_asc = !m_asc;
                else { m_sort = cols[i].key; m_asc = true; }
                Rebuild();
            }
            const ImU32 ink = hot ? C.text : C.dim;
            dl->AddText(ImVec2(cols[i].x, hp.y + (hh - lh) * 0.5f), ink, cols[i].text);
            if (m_sort == cols[i].key)
                SortArrow(dl, ImVec2(cols[i].x + ImGui::CalcTextSize(cols[i].text).x + 9.0f * S(),
                                     hp.y + hh * 0.5f), 3.0f * S(), m_asc, C.dim);
            if (i > 0)
                dl->AddLine(ImVec2(cols[i].x - 8.0f, hp.y + hh * 0.22f),
                            ImVec2(cols[i].x - 8.0f, hp.y + hh * 0.78f), C.line, 1.0f);
        }

        Grip(0, dateX, hp.y, hh, m_colDate);
        Grip(1, kindX, hp.y, hh, m_colKind);
        Grip(2, sizeX, hp.y, hh, m_colSize);
    }

    void Grip(int id, float x, float y, float h, float& target) {
        ImGui::SetCursorScreenPos(ImVec2(x - 11.0f, y));
        ImGui::PushID(900 + id);
        ImGui::InvisibleButton("##grip", ImVec2(7.0f, h));
        if (ImGui::IsItemActive()) target = Clamp(target - ImGui::GetIO().MouseDelta.x, 60.0f, 400.0f);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::PopID();
    }

    void UpRow(float pad) {
        if (!CanGoUp()) return;
        const float h  = RowH();
        const float lh = ImGui::GetTextLineHeight();
        const float w  = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::InvisibleButton("##up_row", ImVec2(w, h));
        const ImVec2 a = ImGui::GetItemRectMin();
        const bool hot = ImGui::IsItemHovered();
        if (hot) dl->AddRectFilled(ImVec2(a.x + 2.0f, a.y + 1.0f), ImVec2(a.x + w - pad * 0.4f, a.y + h - 1.0f),
                                   C.hover, Rr());
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Nav(m_cwd.parent_path());
        AcceptDrop(m_cwd.parent_path());

        const float icon = lh * 1.15f;
        UpGlyph(dl, ImVec2(a.x + pad, a.y + (h - icon) * 0.5f), icon);
        dl->AddText(ImVec2(a.x + pad + icon + 9.0f * S(), a.y + (h - lh) * 0.5f), C.dim, "..");
    }

    void DetailRow(size_t ri, float pad, float dateX, float kindX) {
        const Row& row = m_rows[ri];
        const float h  = RowH();
        const float lh = ImGui::GetTextLineHeight();
        const float w  = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        if (row.heading) {
            ImGui::Dummy(ImVec2(w, h));
            const ImVec2 hpos = ImGui::GetItemRectMin();
            dl->AddText(ImVec2(hpos.x + pad, hpos.y + h - lh - 2.0f * S()), C.accent, row.text.c_str());
            return;
        }

        const Entry& e = m_all[row.index];
        const bool selected = m_sel.count(row.index) > 0;

        ImGui::PushID((int)ri);
        if (m_renaming == (int)ri) { RenameRow(e, pad); ImGui::PopID(); return; }

        ImGui::InvisibleButton("##row", ImVec2(w, h));
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b(a.x + w, a.y + h);
        const bool hot = ImGui::IsItemHovered();

        const ImVec2 r0(a.x + 2.0f, a.y + 1.0f);
        const ImVec2 r1(b.x - pad * 0.4f, b.y - 1.0f);
        if (selected)  dl->AddRectFilled(r0, r1, C.sel, Rr());
        else if (hot)  dl->AddRectFilled(r0, r1, C.hover, Rr());

        if (ImGui::IsItemClicked()) Select(ri);
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Activate(ri);
        BeginDrag(row.index);
        if (e.isDir) AcceptDrop(e.path);
        if (ImGui::BeginPopupContextItem("##rowctx")) {
            if (!selected) Select(ri);
            ItemMenu(row.index);
            ImGui::EndPopup();
        }

        const ImU32 ink = e.hidden ? C.faint : C.text;
        const float icon = lh * 1.15f;
        float x = a.x + pad;
        if (e.isDir) FolderGlyph(dl, ImVec2(x, a.y + (h - icon) * 0.5f), icon);
        else         FileGlyph(dl, ImVec2(x, a.y + (h - icon) * 0.5f), icon, e.cat);
        x += icon + 9.0f * S();

        dl->AddText(ImVec2(x, a.y + (h - lh) * 0.5f), ink, Elide(e.name, dateX - x - 14.0f * S()).c_str());
        if (e.mtime)
            dl->AddText(ImVec2(dateX, a.y + (h - lh) * 0.5f), C.dim, Stamp(e.mtime).c_str());
        dl->AddText(ImVec2(kindX, a.y + (h - lh) * 0.5f), C.dim,
                    Elide(e.kind, m_colKind - 14.0f * S()).c_str());
        if (!e.isDir) {
            const std::string sz = Bytes(e.size);
            const float tsz = ImGui::CalcTextSize(sz.c_str()).x;
            dl->AddText(ImVec2(b.x - pad - tsz, a.y + (h - lh) * 0.5f), C.dim, sz.c_str());
        }
        ImGui::PopID();
    }

    void RenameRow(const Entry& e, float pad) {
        const float h  = RowH();
        const float lh = ImGui::GetTextLineHeight();
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float icon = lh * 1.15f;

        if (e.isDir) FolderGlyph(dl, ImVec2(a.x + pad, a.y + (h - icon) * 0.5f), icon);
        else         FileGlyph(dl, ImVec2(a.x + pad, a.y + (h - icon) * 0.5f), icon, e.cat);

        ImGui::SetCursorScreenPos(ImVec2(a.x + pad + icon + 8.0f * S(), a.y + 2.0f * S()));
        ImGui::SetNextItemWidth(280.0f * S());
        ImGui::PushStyleColor(ImGuiCol_FrameBg, C.pill);
        if (m_renameFocus) { ImGui::SetKeyboardFocusHere(); m_renameFocus = false; }
        if (ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            CommitRename();
        else if (ImGui::IsItemDeactivated())
            CommitRename();
        ImGui::PopStyleColor();
        Seek(ImVec2(a.x, a.y + h));
    }

    // list, tiles and large icons share this layout
    void Grid() {
        const float lh = ImGui::GetTextLineHeight();
        const float pad = 12.0f * S();
        float cw, ch, icon;
        switch (m_mode) {
            case View::List:  cw = 200.0f * S(); ch = RowH();        icon = lh * 1.15f;  break;
            case View::Tiles: cw = 240.0f * S(); ch = RowH() * 1.7f; icon = lh * 2.0f;   break;
            default:          cw = 112.0f * S(); ch = 108.0f * S();  icon = 50.0f * S(); break;
        }

        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + pad * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * S(), 4.0f * S()));
        const float childW = ImGui::GetContentRegionAvail().x - pad;
        ImGui::BeginChild("##cells", ImVec2(childW > 40.0f ? childW : 40.0f, 0.0f), false,
                          ImGuiWindowFlags_NoBackground);

        int cols = (int)(ImGui::GetContentRegionAvail().x / (cw + 4.0f * S()));
        if (cols < 1) cols = 1;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int lead = CanGoUp() ? 1 : 0;
        if (lead) UpCell(dl, cw, ch, icon);
        for (size_t vi = 0; vi < m_view.size(); ++vi) {
            if (((int)vi + lead) % cols != 0) ImGui::SameLine();
            Cell(dl, vi, cw, ch, icon);
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void UpCell(ImDrawList* dl, float cw, float ch, float icon) {
        ImGui::PushID("upcell");
        ImGui::InvisibleButton("##c", ImVec2(cw, ch));
        const ImVec2 a = ImGui::GetItemRectMin();
        const bool hot = ImGui::IsItemHovered();
        if (hot) dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch), C.hover, Rr());
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Nav(m_cwd.parent_path());
        AcceptDrop(m_cwd.parent_path());
        PaintCell(dl, a, cw, ch, icon, "..", "", -1, true, C.dim, true);
        ImGui::PopID();
    }

    void Cell(ImDrawList* dl, size_t vi, float cw, float ch, float icon) {
        const Entry& e = m_all[m_view[vi]];
        const bool selected = m_sel.count(m_view[vi]) > 0;

        ImGui::PushID((int)vi);
        ImGui::InvisibleButton("##c", ImVec2(cw, ch));
        const ImVec2 a = ImGui::GetItemRectMin();
        const bool hot = ImGui::IsItemHovered();

        if (selected)  dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch), C.sel, Rr());
        else if (hot)  dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch), C.hover, Rr());

        if (ImGui::IsItemClicked()) SelectView(vi);
        if (hot && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ActivateView(vi);
        BeginDrag(m_view[vi]);
        if (e.isDir) AcceptDrop(e.path);
        if (ImGui::BeginPopupContextItem("##cellctx")) {
            if (!selected) SelectView(vi);
            ItemMenu(m_view[vi]);
            ImGui::EndPopup();
        }
        if (hot && m_mode != View::Icons) ImGui::SetTooltip("%s", e.name.c_str());

        std::string sub = e.kind;
        if (!e.isDir) sub += "  " + Bytes(e.size);
        PaintCell(dl, a, cw, ch, icon, e.name, sub, e.isDir ? -1 : e.cat, e.isDir,
                  e.hidden ? C.faint : C.text, false);
        ImGui::PopID();
    }

    void PaintCell(ImDrawList* dl, ImVec2 a, float cw, float ch, float icon,
                   const std::string& name, const std::string& sub, int cat, bool dir,
                   ImU32 ink, bool up) {
        const float lh = ImGui::GetTextLineHeight();
        auto icon_at = [&](ImVec2 at) {
            if (up)       UpGlyph(dl, at, icon);
            else if (dir) FolderGlyph(dl, at, icon);
            else          FileGlyph(dl, at, icon, cat);
        };
        if (m_mode == View::Icons) {
            icon_at(ImVec2(a.x + (cw - icon) * 0.5f, a.y + 12.0f * S()));
            std::string l1, l2;
            Wrap2(name, cw - 10.0f * S(), l1, l2);
            const float ty = a.y + 12.0f * S() + icon + 7.0f * S();
            Centered(dl, a.x, cw, ty, l1, ink);
            if (!l2.empty()) Centered(dl, a.x, cw, ty + lh, l2, ink);
        } else if (m_mode == View::Tiles) {
            icon_at(ImVec2(a.x + 10.0f * S(), a.y + (ch - icon) * 0.5f));
            const float tx = a.x + icon + 18.0f * S();
            const float tw = cw - (icon + 26.0f * S());
            dl->AddText(ImVec2(tx, a.y + ch * 0.5f - lh - 1.0f * S()), ink, Elide(name, tw).c_str());
            dl->AddText(ImVec2(tx, a.y + ch * 0.5f + 2.0f * S()), C.faint, Elide(sub, tw).c_str());
        } else {
            icon_at(ImVec2(a.x + 7.0f * S(), a.y + (ch - icon) * 0.5f));
            dl->AddText(ImVec2(a.x + icon + 14.0f * S(), a.y + (ch - lh) * 0.5f), ink,
                        Elide(name, cw - icon - 20.0f * S()).c_str());
        }
    }

    void StatusBar() {
        const float h = StatusH();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float lh = ImGui::GetTextLineHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::Dummy(ImVec2(w, h));
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const float cy = p.y + h * 0.5f;

        std::string left = std::to_string(m_view.size()) + (m_view.size() == 1 ? " item" : " items");
        if (!m_sel.empty()) {
            uintmax_t total = 0;
            bool files = false;
            for (size_t i : m_sel)
                if (i < m_all.size() && !m_all[i].isDir) { total += m_all[i].size; files = true; }
            left += "   |   " + std::to_string(m_sel.size()) + " selected";
            if (files) left += "  " + Bytes(total);
        }
        dl->AddText(ImVec2(p.x + Pad(), cy - lh * 0.5f), C.faint, left.c_str());

        float x = p.x + w - Pad() - 22.0f * S();
        if (MiniToggle(dl, "##mini_icons", ImVec2(x, cy), 1, m_mode == View::Icons)) SetView(View::Icons);
        x -= 24.0f * S();
        if (MiniToggle(dl, "##mini_details", ImVec2(x, cy), 0, m_mode == View::Details)) SetView(View::Details);

        if (const Drive* d = DriveOf(m_cwd)) {
            if (d->space) {
                const std::string r = Bytes(d->freeb) + " free";
                const float tw = ImGui::CalcTextSize(r.c_str()).x;
                dl->AddText(ImVec2(x - 14.0f * S() - tw, cy - lh * 0.5f), C.faint, r.c_str());
            }
        }
        Seek(after);
    }

    bool MiniToggle(ImDrawList* dl, const char* id, ImVec2 mid, int glyph, bool on) {
        const float h = 20.0f * S();
        ImGui::SetCursorScreenPos(ImVec2(mid.x - h * 0.5f, mid.y - h * 0.5f));
        const bool hit = ImGui::InvisibleButton(id, ImVec2(h, h));
        const bool hot = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (on)       dl->AddRectFilled(a, b, IM_COL32(0x60, 0xCD, 0xFF, 34), Rr() * 0.8f);
        else if (hot) dl->AddRectFilled(a, b, C.hover, Rr() * 0.8f);
        const ImU32 ink = on ? C.accent : (hot ? C.text : C.faint);
        const float u = 1.9f * S();
        if (glyph == 0) {
            for (int i = 0; i < 3; ++i) {
                const float y = mid.y + (float)(i - 1) * u * 1.9f;
                dl->AddRectFilled(ImVec2(mid.x - u * 2.6f, y - u * 0.3f), ImVec2(mid.x + u * 2.6f, y + u * 0.3f), ink);
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                const float ox = (i % 2 ? 1.0f : -1.0f) * u * 1.45f;
                const float oy = (i / 2 ? 1.0f : -1.0f) * u * 1.45f;
                dl->AddRectFilled(ImVec2(mid.x + ox - u, mid.y + oy - u),
                                  ImVec2(mid.x + ox + u, mid.y + oy + u), ink, 1.0f);
            }
        }
        return hit;
    }

    Action Footer() {
        Action a = Action::None;
        const float h = 30.0f * S();
        const float bw = 100.0f * S();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::Dummy(ImVec2(w, h));
        const ImVec2 after = ImGui::GetCursorScreenPos();

        if (Pill(dl, "##open", ImVec2(p.x + Pad(), p.y), ImVec2(bw, h), "Open", true, !m_sel.empty()))
            a = Action::Open;
        if (Pill(dl, "##cancel", ImVec2(p.x + Pad() + bw + 8.0f * S(), p.y), ImVec2(bw, h),
                 "Cancel", false, true))
            a = Action::Cancel;
        Seek(after);
        return a;
    }

    bool Pill(ImDrawList* dl, const char* id, ImVec2 pos, ImVec2 size,
              const char* label, bool primary, bool enabled) {
        ImGui::SetCursorScreenPos(pos);
        ImGui::BeginDisabled(!enabled);
        ImGui::InvisibleButton(id, size);
        const bool hit  = ImGui::IsItemActivated();
        const bool hot  = ImGui::IsItemHovered();
        const bool down = ImGui::IsItemActive();
        ImGui::EndDisabled();

        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImU32 fill, ink;
        if (primary) {
            fill = !enabled ? IM_COL32(255, 255, 255, 16)
                 : down     ? IM_COL32(0x45, 0xA3, 0xD6, 255)
                 : hot      ? IM_COL32(0x7A, 0xD6, 0xFF, 255)
                            : C.accent;
            ink = enabled ? C.accentInk : C.faint;
        } else {
            fill = down ? IM_COL32(255, 255, 255, 30)
                 : hot  ? IM_COL32(255, 255, 255, 22)
                        : IM_COL32(255, 255, 255, 14);
            ink = C.text;
        }
        dl->AddRectFilled(a, b, fill, Rr());
        if (!primary) dl->AddRect(a, b, C.line, Rr());
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), ink, label);
        return hit && enabled;
    }

    void ItemMenu(size_t entry) {
        const Entry& e = m_all[entry];
        if (ImGui::MenuItem("Open", "Enter")) OpenEntry(entry);
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X"))  Copy(true);
        if (ImGui::MenuItem("Copy", "Ctrl+C")) Copy(false);
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_clip.empty())) Paste();
        ImGui::Separator();
        if (e.isDir) {
            if (IsPinned(e.path)) { if (ImGui::MenuItem("Unpin")) Unpin(e.path); }
            else                  { if (ImGui::MenuItem("Pin to sidebar")) Pin(e.path); }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("New folder"))    NewFolder();
        if (ImGui::MenuItem("Rename", "F2"))  { SelectEntry(entry); BeginRenameSelected(); }
        if (ImGui::MenuItem("Delete", "Del")) AskDelete();
    }

    void BackgroundMenu() {
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            !ImGui::IsAnyItemHovered() &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##bgctx");
        if (!ImGui::BeginPopup("##bgctx")) return;
        if (ImGui::MenuItem("New folder")) NewFolder();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_clip.empty())) Paste();
        ImGui::Separator();
        if (CanGoUp() && ImGui::MenuItem("Up one level", "Backspace")) GoUp();
        if (ImGui::MenuItem("Refresh", "F5")) HardReload();
        ImGui::Separator();
        if (ImGui::MenuItem("Show hidden files", "Ctrl+H", m_showHidden)) {
            m_showHidden = !m_showHidden; Save(); Reload();
        }
        if (!IsPinned(m_cwd) && ImGui::MenuItem("Pin this folder")) Pin(m_cwd);
        ImGui::EndPopup();
    }

    void Popups() {
        if (m_wantError) { ImGui::OpenPopup("Problem"); m_wantError = false; }
        if (ImGui::BeginPopupModal("Problem", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(m_error.c_str());
            ImGui::Dummy(ImVec2(0.0f, 8.0f * S()));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(220.0f * S(), 30.0f * S()));
            if (Pill(ImGui::GetWindowDrawList(), "##ok", p, ImVec2(96.0f * S(), 30.0f * S()),
                     "OK", true, true))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (m_wantDelete) { ImGui::OpenPopup("Delete"); m_wantDelete = false; }
        if (ImGui::BeginPopupModal("Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Move %d item(s) to the Recycle Bin?", (int)m_doomed.size());
            ImGui::Dummy(ImVec2(0.0f, 8.0f * S()));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(220.0f * S(), 30.0f * S()));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (Pill(dl, "##del", p, ImVec2(100.0f * S(), 30.0f * S()), "Delete", true, true)) {
                DoDelete();
                ImGui::CloseCurrentPopup();
            }
            if (Pill(dl, "##keep", ImVec2(p.x + 108.0f * S(), p.y), ImVec2(100.0f * S(), 30.0f * S()),
                     "Cancel", false, true)) {
                m_doomed.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    bool CanGoUp() const {
        if (!m_cwd.has_parent_path()) return false;
        if (m_cwd.parent_path() == m_cwd) return false;
        return m_cwd != m_cwd.root_path();
    }
    // queue navigation, applied at the end of the frame
    void Nav(const fs::path& p) { m_navReq = p; m_navPending = true; }
    void GoUp()   { if (CanGoUp()) Nav(m_cwd.parent_path()); }
    void GoBack() { if (m_back.empty()) return; m_fwd.push_back(m_cwd); m_cwd = m_back.back(); m_back.pop_back(); AfterJump(); }
    void GoFwd()  { if (m_fwd.empty()) return; m_back.push_back(m_cwd); m_cwd = m_fwd.back(); m_fwd.pop_back(); AfterJump(); }
    void AfterJump() { m_sel.clear(); m_anchor = -1; m_renaming = -1; m_search[0] = '\0'; Reload(); }
    void HardReload() { ScanDrives(); BuildNav(); m_tree.clear(); Reload(); }

    void BeginEditPath() {
        std::snprintf(m_pathBuf, sizeof(m_pathBuf), "%s", m_cwd.string().c_str());
        m_editPath = true;
        m_editFocus = true;
    }

    void Reload() {
        m_all.clear();
        std::error_code ec;
        for (auto it = fs::directory_iterator(m_cwd, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            Entry en;
            en.path   = it->path();
            en.name   = it->path().filename().string();
            en.isDir  = it->is_directory(e2);
            en.hidden = IsHidden(it->path());
            if (!en.isDir) { en.size = it->file_size(e2); if (e2) en.size = 0; }
            const auto ft = it->last_write_time(e2);
            en.mtime = e2 ? 0 : ToTimeT(ft);
            en.kind  = KindOf(en);
            en.cat   = CatOf(en);
            m_all.push_back(std::move(en));
        }
        Rebuild();
    }

    void Rebuild() {
        m_view.clear();
        const std::string q = Lower(m_search);
        for (size_t i = 0; i < m_all.size(); ++i) {
            if (m_all[i].hidden && !m_showHidden) continue;
            if (!q.empty() && Lower(m_all[i].name).find(q) == std::string::npos) continue;
            m_view.push_back(i);
        }
        SortView();
        BuildRows();
        m_sel.clear();
        m_anchor = -1;
    }

    void SortView() {
        const bool grouped = m_group && m_sort == Sort::Date;
        auto cmp = [&](size_t x, size_t y) {
            const Entry& ea = m_all[x];
            const Entry& eb = m_all[y];
            if (!grouped && ea.isDir != eb.isDir) return ea.isDir;
            int c = 0;
            switch (m_sort) {
                case Sort::Name: c = Natural(ea.name, eb.name); break;
                case Sort::Date: c = ea.mtime < eb.mtime ? -1 : (ea.mtime > eb.mtime ? 1 : 0); break;
                case Sort::Kind: {
                    const int k = ea.kind.compare(eb.kind);
                    c = k < 0 ? -1 : (k > 0 ? 1 : Natural(ea.name, eb.name));
                } break;
                default: {
                    const uintmax_t sa = ea.isDir ? 0 : ea.size, sb = eb.isDir ? 0 : eb.size;
                    c = sa < sb ? -1 : (sa > sb ? 1 : Natural(ea.name, eb.name));
                } break;
            }
            return m_asc ? c < 0 : c > 0;
        };
        std::stable_sort(m_view.begin(), m_view.end(), cmp);
    }

    // keep headings in the row list so all rows share one height
    void BuildRows() {
        m_rows.clear();
        m_rows.reserve(m_view.size() + 8);
        const bool grouped = m_group && m_sort == Sort::Date;
        std::string last;
        for (size_t i : m_view) {
            if (grouped) {
                std::string g = Bucket(m_all[i].mtime);
                if (g != last) { last = g; Row r; r.heading = true; r.text = g; m_rows.push_back(r); }
            }
            Row r;
            r.index = i;
            m_rows.push_back(r);
        }
    }

    void Select(size_t ri) {
        if (m_rows[ri].heading) return;
        const ImGuiIO& io = ImGui::GetIO();
        const size_t entry = m_rows[ri].index;
        if (io.KeyCtrl) {
            if (m_sel.count(entry)) m_sel.erase(entry); else m_sel.insert(entry);
            m_anchor = (int)ri;
        } else if (io.KeyShift && m_anchor >= 0) {
            m_sel.clear();
            const size_t lo = std::min((size_t)m_anchor, ri), hi = std::max((size_t)m_anchor, ri);
            for (size_t k = lo; k <= hi && k < m_rows.size(); ++k)
                if (!m_rows[k].heading) m_sel.insert(m_rows[k].index);
        } else {
            m_sel.clear();
            m_sel.insert(entry);
            m_anchor = (int)ri;
        }
    }

    void SelectView(size_t vi) {
        const ImGuiIO& io = ImGui::GetIO();
        const size_t entry = m_view[vi];
        if (io.KeyCtrl) {
            if (m_sel.count(entry)) m_sel.erase(entry); else m_sel.insert(entry);
            m_anchor = (int)vi;
        } else if (io.KeyShift && m_anchor >= 0) {
            m_sel.clear();
            const size_t lo = std::min((size_t)m_anchor, vi), hi = std::max((size_t)m_anchor, vi);
            for (size_t k = lo; k <= hi && k < m_view.size(); ++k) m_sel.insert(m_view[k]);
        } else {
            m_sel.clear();
            m_sel.insert(entry);
            m_anchor = (int)vi;
        }
    }

    void SelectEntry(size_t entry) { m_sel.clear(); m_sel.insert(entry); }

    void Activate(size_t ri)     { if (!m_rows[ri].heading) OpenEntry(m_rows[ri].index); }
    void ActivateView(size_t vi) { OpenEntry(m_view[vi]); }

    void OpenEntry(size_t entry) {
        const Entry& e = m_all[entry];
        if (e.isDir) { Nav(e.path); return; }
        if (m_foldersOnly) return;
        m_sel.clear();
        m_sel.insert(entry);
        m_activated = true;
    }

    void NewFolder() {
        std::error_code ec;
        fs::path p = m_cwd / "New folder";
        for (int n = 2; fs::exists(p, ec); ++n)
            p = m_cwd / ("New folder (" + std::to_string(n) + ")");
        fs::create_directory(p, ec);
        if (ec) { Fail("The folder could not be created.\n" + ec.message()); return; }
        Reload();
        Forget(m_cwd);
        if (m_mode != View::Details) return;
        for (size_t i = 0; i < m_rows.size(); ++i)
            if (!m_rows[i].heading && m_all[m_rows[i].index].path == p) { BeginRename(i); break; }
    }

    void BeginRename(size_t ri) {
        if (m_rows[ri].heading) return;
        m_mode = View::Details;
        m_renaming = (int)ri;
        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", m_all[m_rows[ri].index].name.c_str());
        m_renameFocus = true;
    }

    void BeginRenameSelected() {
        if (m_sel.size() != 1) return;
        const size_t entry = *m_sel.begin();
        m_mode = View::Details;
        for (size_t i = 0; i < m_rows.size(); ++i)
            if (!m_rows[i].heading && m_rows[i].index == entry) { BeginRename(i); return; }
    }

    void CommitRename() {
        if (m_renaming < 0 || m_renaming >= (int)m_rows.size()) { m_renaming = -1; return; }
        const Entry& e = m_all[m_rows[m_renaming].index];
        const std::string name = m_renameBuf;
        if (!name.empty() && name != e.name) {
            std::error_code ec;
            fs::rename(e.path, e.path.parent_path() / name, ec);
            if (ec) Fail("That name could not be used.\n" + ec.message());
        }
        m_renaming = -1;
        Reload();
        Forget(m_cwd);
    }

    void Copy(bool cut) { m_clip = SelectedPaths(); m_cut = cut; }

    void Paste() {
        std::error_code ec;
        for (const fs::path& src : m_clip) {
            fs::path dst = FreeName(m_cwd / src.filename());
            if (m_cut) {
                fs::rename(src, dst, ec);
                if (ec) { ec.clear(); CopyTree(src, dst, ec); if (!ec) fs::remove_all(src, ec); }
            } else {
                CopyTree(src, dst, ec);
            }
            if (ec) { Fail("The paste did not finish.\n" + ec.message()); ec.clear(); }
        }
        if (m_cut) { m_clip.clear(); m_cut = false; }
        Reload();
        Forget(m_cwd);
    }

    static void CopyTree(const fs::path& src, const fs::path& dst, std::error_code& ec) {
        if (fs::is_directory(src, ec)) fs::copy(src, dst, fs::copy_options::recursive, ec);
        else                           fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    }

    // append copy until the destination name is free
    static fs::path FreeName(fs::path dst) {
        std::error_code ec;
        if (!fs::exists(dst, ec)) return dst;
        const fs::path dir = dst.parent_path();
        const std::string stem = dst.stem().string();
        const std::string ext  = dst.extension().string();
        for (int n = 0;; ++n) {
            const std::string tag = n == 0 ? " copy" : " copy " + std::to_string(n + 1);
            fs::path cand = dir / (stem + tag + ext);
            if (!fs::exists(cand, ec)) return cand;
        }
    }

    void AskDelete() {
        m_doomed = SelectedPaths();
        if (!m_doomed.empty()) m_wantDelete = true;
    }

    void DoDelete() {
#ifdef _WIN32
        std::wstring buf;
        for (const fs::path& p : m_doomed) { buf += p.wstring(); buf.push_back(L'\0'); }
        buf.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc  = FO_DELETE;
        op.pFrom  = buf.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        SHFileOperationW(&op);
#else
        std::error_code ec;
        for (const fs::path& p : m_doomed) fs::remove_all(p, ec);
#endif
        m_doomed.clear();
        Reload();
        Forget(m_cwd);
    }

    void Forget(const fs::path& p) {
        auto it = m_tree.find(Key(p));
        if (it != m_tree.end()) { it->second.loaded = false; it->second.kids.clear(); }
    }

    static const char* DragId() { return "FB_PATHS"; }

    void BeginDrag(size_t entry) {
        if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) return;
        if (!m_sel.count(entry)) { m_sel.clear(); m_sel.insert(entry); }
        m_dragging = SelectedPaths();
        const char marker = 1;
        ImGui::SetDragDropPayload(DragId(), &marker, sizeof(marker));
        const float lh = ImGui::GetTextLineHeight();
        FolderGlyph(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), lh);
        ImGui::Dummy(ImVec2(lh + 6.0f, lh));
        ImGui::SameLine();
        if (m_dragging.size() == 1) ImGui::TextUnformatted(m_dragging[0].filename().string().c_str());
        else                        ImGui::Text("%d items", (int)m_dragging.size());
        ImGui::EndDragDropSource();
    }

    void AcceptDrop(const fs::path& dir) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (ImGui::AcceptDragDropPayload(DragId())) {
            m_dropSrc    = m_dragging;
            m_dropDst    = dir;
            m_dropMove   = !ImGui::GetIO().KeyCtrl;
            m_dropQueued = true;
        }
        ImGui::EndDragDropTarget();
    }

    // true when child is parent itself or lives underneath it
    static bool Inside(const fs::path& child, const fs::path& parent) {
        const std::string c = Key(child), p = Key(parent);
        if (c == p) return true;
        if (p.empty() || c.size() <= p.size()) return false;
        if (c.compare(0, p.size(), p) != 0) return false;
        const char after = c[p.size()];
        const char last  = p[p.size() - 1];
        return after == '\\' || after == '/' || last == '\\' || last == '/';
    }

    void ApplyDrop() {
        if (!m_dropQueued) return;
        m_dropQueued = false;
        std::error_code ec;
        for (const fs::path& src : m_dropSrc) {
            if (src.empty()) continue;
            if (Inside(m_dropDst, src)) { Fail("A folder cannot be moved into itself.\n" + src.string()); continue; }
            if (Key(src.parent_path()) == Key(m_dropDst)) continue;
            fs::path dst = FreeName(m_dropDst / src.filename());
            if (m_dropMove) {
                fs::rename(src, dst, ec);
                if (ec) { ec.clear(); CopyTree(src, dst, ec); if (!ec) fs::remove_all(src, ec); }
            } else {
                CopyTree(src, dst, ec);
            }
            if (ec) { Fail("The move did not finish.\n" + ec.message()); ec.clear(); }
        }
        Forget(m_dropDst);
        Forget(m_cwd);
        m_dropSrc.clear();
        m_dragging.clear();
        Reload();
    }

    // explorer shortcuts, skipped while any text field is active
    void Shortcuts() {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
        if (m_renaming >= 0 || m_editPath) return;
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) return;

        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) GoUp();
        if (ImGui::IsKeyPressed(ImGuiKey_F2))        BeginRenameSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_F5))        HardReload();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))    AskDelete();
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_sel.size() == 1) OpenEntry(*m_sel.begin());
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) Copy(false);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) Copy(true);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) Paste();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L)) BeginEditPath();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            m_sel.clear();
            for (size_t i : m_view) m_sel.insert(i);
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_H)) { m_showHidden = !m_showHidden; Save(); Reload(); }
        if (io.KeyCtrl && io.KeyShift) {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) SetView(View::Details);
            if (ImGui::IsKeyPressed(ImGuiKey_2)) SetView(View::List);
            if (ImGui::IsKeyPressed(ImGuiKey_3)) SetView(View::Tiles);
            if (ImGui::IsKeyPressed(ImGuiKey_4)) SetView(View::Icons);
        }
    }

    bool NavBtn(ImDrawList* dl, const char* id, ImVec2 mid, int glyph, bool enabled) {
        const float h = 30.0f * S();
        ImGui::SetCursorScreenPos(ImVec2(mid.x - h * 0.5f, mid.y - h * 0.5f));
        ImGui::BeginDisabled(!enabled);
        ImGui::InvisibleButton(id, ImVec2(h, h));
        const bool hit = ImGui::IsItemActivated();
        const bool hot = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        if (hot && enabled) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddRectFilled(ImVec2(a.x + 3.0f, a.y + 3.0f), ImVec2(b.x - 3.0f, b.y - 3.0f), C.hover, Rr());
        }
        const ImU32 ink = enabled ? (hot ? C.text : C.dim) : C.faint;
        const float r = 4.4f * S();
        if (glyph == 0)      Arrow(dl, mid, r, ink, true);
        else if (glyph == 1) Arrow(dl, mid, r, ink, false);
        else if (glyph == 2) ArrowUp(dl, mid, r, ink);
        else {
            dl->PathArcTo(mid, r * 1.05f, 0.8f, 5.6f, 22);
            dl->PathStroke(ink, 0, 1.6f);
            dl->AddTriangleFilled(ImVec2(mid.x + r * 1.5f, mid.y - r * 0.25f),
                                  ImVec2(mid.x + r * 0.4f, mid.y - r * 0.55f),
                                  ImVec2(mid.x + r * 1.1f, mid.y + r * 0.55f), ink);
        }
        return hit && enabled;
    }

    static void Arrow(ImDrawList* dl, ImVec2 m, float r, ImU32 ink, bool left) {
        const float d = left ? -1.0f : 1.0f;
        dl->AddLine(ImVec2(m.x - r * d, m.y), ImVec2(m.x + r * d, m.y), ink, 1.6f);
        dl->AddLine(ImVec2(m.x + r * d, m.y), ImVec2(m.x + r * 0.15f * d, m.y - r * 0.8f), ink, 1.6f);
        dl->AddLine(ImVec2(m.x + r * d, m.y), ImVec2(m.x + r * 0.15f * d, m.y + r * 0.8f), ink, 1.6f);
    }

    static void ArrowUp(ImDrawList* dl, ImVec2 m, float r, ImU32 ink) {
        dl->AddLine(ImVec2(m.x, m.y + r), ImVec2(m.x, m.y - r), ink, 1.6f);
        dl->AddLine(ImVec2(m.x, m.y - r), ImVec2(m.x - r * 0.8f, m.y - r * 0.15f), ink, 1.6f);
        dl->AddLine(ImVec2(m.x, m.y - r), ImVec2(m.x + r * 0.8f, m.y - r * 0.15f), ink, 1.6f);
    }

    static void Chevron(ImDrawList* dl, ImVec2 mid, float r, ImU32 col, float deg) {
        const float rad = deg * 3.14159265f / 180.0f;
        const float cs = std::cos(rad), sn = std::sin(rad);
        auto rot = [&](float x, float y) {
            return ImVec2(mid.x + x * cs - y * sn, mid.y + x * sn + y * cs);
        };
        dl->AddLine(rot(-r * 0.35f, -r), rot(r * 0.5f, 0.0f), col, 1.5f);
        dl->AddLine(rot(r * 0.5f, 0.0f), rot(-r * 0.35f, r), col, 1.5f);
    }

    static void SortArrow(ImDrawList* dl, ImVec2 mid, float r, bool up, ImU32 col) {
        if (up) dl->AddTriangleFilled(ImVec2(mid.x - r, mid.y + r * 0.5f),
                                      ImVec2(mid.x + r, mid.y + r * 0.5f),
                                      ImVec2(mid.x, mid.y - r * 0.7f), col);
        else    dl->AddTriangleFilled(ImVec2(mid.x - r, mid.y - r * 0.5f),
                                      ImVec2(mid.x + r, mid.y - r * 0.5f),
                                      ImVec2(mid.x, mid.y + r * 0.7f), col);
    }

    static void SearchGlyph(ImDrawList* dl, ImVec2 mid, float r, ImU32 col) {
        dl->AddCircle(ImVec2(mid.x - r * 0.15f, mid.y - r * 0.15f), r * 0.8f, col, 14, 1.4f);
        dl->AddLine(ImVec2(mid.x + r * 0.4f, mid.y + r * 0.4f),
                    ImVec2(mid.x + r * 1.0f, mid.y + r * 1.0f), col, 1.5f);
    }

    static void PinGlyph(ImDrawList* dl, ImVec2 mid, float r, ImU32 col) {
        dl->AddCircleFilled(ImVec2(mid.x + r * 0.3f, mid.y - r * 0.3f), r * 0.55f, col, 10);
        dl->AddLine(ImVec2(mid.x + r * 0.1f, mid.y + r * 0.1f),
                    ImVec2(mid.x - r * 0.8f, mid.y + r * 1.0f), col, 1.4f);
    }

    void HomeGlyph(ImDrawList* dl, ImVec2 p, float s) const {
        const ImU32 ink = C.dim;
        const float x0 = p.x + s * 0.12f, x1 = p.x + s * 0.88f;
        const float ym = p.y + s * 0.46f, y1 = p.y + s * 0.9f;
        dl->AddTriangleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.08f),
                              ImVec2(x0 - s * 0.02f, ym), ImVec2(x1 + s * 0.02f, ym), ink);
        dl->AddRectFilled(ImVec2(x0 + s * 0.08f, ym), ImVec2(x1 - s * 0.08f, y1), ink, s * 0.06f);
        dl->AddRectFilled(ImVec2(p.x + s * 0.42f, p.y + s * 0.62f),
                          ImVec2(p.x + s * 0.58f, y1), C.body);
    }

    void PCGlyph(ImDrawList* dl, ImVec2 p, float s) const {
        const ImU32 ink = C.dim;
        dl->AddRect(ImVec2(p.x + s * 0.08f, p.y + s * 0.14f),
                    ImVec2(p.x + s * 0.92f, p.y + s * 0.66f), ink, s * 0.08f, 0, 1.4f);
        dl->AddLine(ImVec2(p.x + s * 0.5f, p.y + s * 0.66f),
                    ImVec2(p.x + s * 0.5f, p.y + s * 0.82f), ink, 1.4f);
        dl->AddLine(ImVec2(p.x + s * 0.3f, p.y + s * 0.86f),
                    ImVec2(p.x + s * 0.7f, p.y + s * 0.86f), ink, 1.4f);
    }

    static void FolderGlyph(ImDrawList* dl, ImVec2 p, float s) {
        const ImU32 back  = IM_COL32(0xE8, 0xA3, 0x3D, 255);
        const ImU32 front = IM_COL32(0xFF, 0xD1, 0x60, 255);
        const float r = s * 0.12f;
        dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.16f),
                          ImVec2(p.x + s * 0.46f, p.y + s * 0.42f), back, r);
        dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.24f),
                          ImVec2(p.x + s * 0.95f, p.y + s * 0.86f), back, r);
        dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.34f),
                          ImVec2(p.x + s * 0.95f, p.y + s * 0.86f), front, r);
    }

    static void UpGlyph(ImDrawList* dl, ImVec2 p, float s) {
        FolderGlyph(dl, p, s);
        const ImU32 ink = IM_COL32(0x6B, 0x4A, 0x10, 255);
        const float cx = p.x + s * 0.5f, cy = p.y + s * 0.61f;
        const float w = s * 0.18f, h = s * 0.15f;
        dl->AddTriangleFilled(ImVec2(cx, cy - h), ImVec2(cx - w, cy + h * 0.2f),
                              ImVec2(cx + w, cy + h * 0.2f), ink);
        dl->AddRectFilled(ImVec2(cx - w * 0.32f, cy + h * 0.1f), ImVec2(cx + w * 0.32f, cy + h * 0.95f), ink);
    }

    // file category colors
    static ImU32 CatColor(int cat) {
        switch (cat) {
            case 1:  return IM_COL32(0x5B, 0xA9, 0xF5, 255);
            case 2:  return IM_COL32(0x5F, 0xC9, 0x8B, 255);
            case 3:  return IM_COL32(0xE0, 0xA5, 0x4A, 255);
            case 4:  return IM_COL32(0xE8, 0x6E, 0x6E, 255);
            case 5:  return IM_COL32(0xB7, 0x8C, 0xF0, 255);
            case 6:  return IM_COL32(0x8F, 0x9B, 0xAB, 255);
            default: return IM_COL32(0x8A, 0x8A, 0x8A, 255);
        }
    }

    static void FileGlyph(ImDrawList* dl, ImVec2 p, float s, int cat) {
        const ImU32 sheet = IM_COL32(0xEC, 0xEF, 0xF3, 255);
        const ImU32 fold  = IM_COL32(0xBD, 0xC5, 0xD0, 255);
        const float x0 = p.x + s * 0.18f, x1 = p.x + s * 0.82f;
        const float y0 = p.y + s * 0.05f, y1 = p.y + s * 0.95f;
        const float f  = s * 0.25f;
        const float r  = s * 0.1f;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), sheet, r);
        dl->AddTriangleFilled(ImVec2(x1 - f, y0), ImVec2(x1, y0 + f), ImVec2(x1 - f, y0 + f), fold);
        if (cat >= 0) {
            const float bh = s * 0.17f;
            dl->AddRectFilled(ImVec2(x0, y1 - bh), ImVec2(x1, y1), CatColor(cat), r);
            dl->AddRectFilled(ImVec2(x0, y1 - bh), ImVec2(x1, y1 - bh + r * 0.9f), CatColor(cat));
        }
    }

    static void DriveGlyph(ImDrawList* dl, ImVec2 p, float s, unsigned type) {
#ifdef _WIN32
        const bool removable = type == DRIVE_REMOVABLE;
        const bool optical   = type == DRIVE_CDROM;
        const bool network   = type == DRIVE_REMOTE;
#else
        const bool removable = false, optical = false, network = false;
        (void)type;
#endif
        if (optical) {
            dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.4f,
                                IM_COL32(0xC3, 0xCA, 0xD4, 255), 22);
            dl->AddCircleFilled(ImVec2(p.x + s * 0.5f, p.y + s * 0.5f), s * 0.12f,
                                IM_COL32(0x19, 0x19, 0x19, 255), 12);
            return;
        }
        const ImU32 body = removable ? IM_COL32(0x6D, 0xB7, 0xF0, 255) : IM_COL32(0xA4, 0xAD, 0xB9, 255);
        dl->AddRectFilled(ImVec2(p.x + s * 0.07f, p.y + s * 0.3f),
                          ImVec2(p.x + s * 0.93f, p.y + s * 0.72f), body, s * 0.13f);
        dl->AddCircleFilled(ImVec2(p.x + s * 0.75f, p.y + s * 0.51f), s * 0.06f,
                            IM_COL32(0x19, 0x19, 0x19, 200), 10);
        if (network)
            dl->AddRectFilled(ImVec2(p.x + s * 0.22f, p.y + s * 0.77f),
                              ImVec2(p.x + s * 0.78f, p.y + s * 0.86f),
                              IM_COL32(0x7C, 0xD9, 0x92, 255), s * 0.05f);
    }

    static void Centered(ImDrawList* dl, float x, float w, float y, const std::string& s, ImU32 col) {
        if (s.empty()) return;
        const float tw = ImGui::CalcTextSize(s.c_str()).x;
        dl->AddText(ImVec2(x + (w - tw) * 0.5f, y), col, s.c_str());
    }

    static std::string Elide(const std::string& s, float maxW) {
        if (maxW <= 0.0f) return std::string();
        if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
        std::string out = s;
        while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > maxW) out.pop_back();
        return out + "...";
    }

    static void Wrap2(const std::string& s, float maxW, std::string& l1, std::string& l2) {
        l1.clear();
        l2.clear();
        if (maxW <= 0.0f) return;
        if (ImGui::CalcTextSize(s.c_str()).x <= maxW) { l1 = s; return; }
        size_t cut = 1;
        for (size_t i = 1; i <= s.size(); ++i) {
            if (ImGui::CalcTextSize(s.substr(0, i).c_str()).x > maxW) { cut = i > 1 ? i - 1 : 1; break; }
            cut = i;
        }
        l1 = s.substr(0, cut);
        l2 = Elide(s.substr(cut), maxW);
    }

    void Fail(std::string msg) { m_error = std::move(msg); m_wantError = true; }

    static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static std::string Key(const fs::path& p) { return Lower(p.string()); }

    static std::string Lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static bool SamePath(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        return fs::equivalent(a, b, ec);
    }

    static bool IsHidden(const fs::path& p) {
#ifdef _WIN32
        const DWORD a = ::GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
#else
        const std::string n = p.filename().string();
        return !n.empty() && n[0] == '.';
#endif
    }

    // natural compare, digit runs compare by value
    static int Natural(const std::string& a, const std::string& b) {
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
                const size_t si = i, sj = j;
                while (i < a.size() && std::isdigit((unsigned char)a[i])) ++i;
                while (j < b.size() && std::isdigit((unsigned char)b[j])) ++j;
                std::string na = a.substr(si, i - si), nb = b.substr(sj, j - sj);
                const size_t za = na.find_first_not_of('0');
                na = za == std::string::npos ? "0" : na.substr(za);
                const size_t zb = nb.find_first_not_of('0');
                nb = zb == std::string::npos ? "0" : nb.substr(zb);
                if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
                const int c = na.compare(nb);
                if (c) return c < 0 ? -1 : 1;
            } else {
                const char ca = (char)std::tolower((unsigned char)a[i]);
                const char cb = (char)std::tolower((unsigned char)b[j]);
                if (ca != cb) return ca < cb ? -1 : 1;
                ++i;
                ++j;
            }
        }
        if (i < a.size()) return 1;
        if (j < b.size()) return -1;
        return 0;
    }

    static std::string Ext(const Entry& e) { return Lower(e.path.extension().string()); }

    static int CatOf(const Entry& e) {
        if (e.isDir) return -1;
        const std::string x = Ext(e);
        static const std::unordered_map<std::string, int> cats = {
            {".c",1},{".h",1},{".cpp",1},{".hpp",1},{".cc",1},{".cs",1},{".py",1},{".js",1},
            {".ts",1},{".lua",1},{".rs",1},{".go",1},{".java",1},{".json",1},{".xml",1},
            {".html",1},{".css",1},{".sh",1},{".bat",1},{".ini",1},{".cfg",1},{".toml",1},{".yml",1},
            {".png",2},{".jpg",2},{".jpeg",2},{".gif",2},{".bmp",2},{".svg",2},{".webp",2},{".ico",2},
            {".zip",3},{".rar",3},{".7z",3},{".tar",3},{".gz",3},{".iso",3},
            {".pdf",4},{".doc",4},{".docx",4},{".txt",4},{".md",4},{".rtf",4},{".xls",4},{".xlsx",4},
            {".mp3",5},{".wav",5},{".flac",5},{".mp4",5},{".mkv",5},{".avi",5},{".mov",5},
            {".exe",6},{".dll",6},{".so",6},{".sys",6},{".bin",6},{".o",6},{".obj",6},
        };
        const auto it = cats.find(x);
        return it == cats.end() ? 0 : it->second;
    }

    static std::string KindOf(const Entry& e) {
        if (e.isDir) return "File folder";
        const std::string x = Ext(e);
        if (x.empty()) return "File";
        static const std::unordered_map<std::string, std::string> known = {
            {".lua","Lua source"},{".txt","Text document"},{".md","Markdown"},{".cpp","C++ source"},
            {".cc","C++ source"},{".c","C source"},{".h","C header"},{".hpp","C++ header"},
            {".ini","Settings"},{".cfg","Settings"},{".json","JSON"},{".xml","XML"},
            {".png","PNG image"},{".jpg","JPEG image"},{".jpeg","JPEG image"},{".gif","GIF image"},
            {".bmp","Bitmap"},{".svg","SVG image"},{".dll","App extension"},{".exe","Application"},
            {".zip","Zip archive"},{".rar","RAR archive"},{".7z","7z archive"},{".iso","Disc image"},
            {".pdf","PDF"},{".py","Python source"},{".cs","C# source"},{".js","JavaScript"},
            {".ts","TypeScript"},{".html","HTML"},{".css","Stylesheet"},{".bat","Batch file"},
            {".ttf","Font"},{".mp3","Audio"},{".wav","Audio"},{".mp4","Video"},{".mkv","Video"},
        };
        const auto it = known.find(x);
        if (it != known.end()) return it->second;
        std::string up = x.substr(1);
        for (char& c : up) c = (char)std::toupper((unsigned char)c);
        return up + " file";
    }

    static std::time_t ToTimeT(fs::file_time_type ft) {
        using namespace std::chrono;
        const auto tp = time_point_cast<system_clock::duration>(
            ft - fs::file_time_type::clock::now() + system_clock::now());
        return system_clock::to_time_t(tp);
    }

    static std::string Stamp(std::time_t t) {
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &tm);
        return buf;
    }

    static std::string Bytes(uint64_t n) {
        const char* u[] = { "bytes", "KB", "MB", "GB", "TB" };
        double v = (double)n;
        int i = 0;
        while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
        char buf[64];
        if (i == 0)          std::snprintf(buf, sizeof(buf), "%llu bytes", (unsigned long long)n);
        else if (v >= 100.0) std::snprintf(buf, sizeof(buf), "%.0f %s", v, u[i]);
        else                 std::snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
        return buf;
    }

    static std::string Bucket(std::time_t t) {
        if (t == 0) return "Unknown";
        const double days = std::difftime(std::time(nullptr), t) / 86400.0;
        if (days < 1)   return "Today";
        if (days < 2)   return "Yesterday";
        if (days < 7)   return "Earlier this week";
        if (days < 31)  return "Earlier this month";
        if (days < 365) return "Earlier this year";
        return "A long time ago";
    }

#ifdef _WIN32
    struct Quiet {
        UINT prev;
        Quiet()  { prev = ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX); }
        ~Quiet() { ::SetErrorMode(prev); }
    };

    static std::string TypeName(unsigned t) {
        switch (t) {
            case DRIVE_REMOVABLE: return "USB Drive";
            case DRIVE_FIXED:     return "Local Disk";
            case DRIVE_REMOTE:    return "Network Drive";
            case DRIVE_CDROM:     return "DVD Drive";
            case DRIVE_RAMDISK:   return "RAM Disk";
            default:              return "Drive";
        }
    }

    static std::string Narrow(const wchar_t* w) {
        if (!w || !*w) return std::string();
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return std::string();
        std::string out((size_t)n - 1, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
        return out;
    }

    static void Probe(Drive& d) {
        Quiet quiet;
        const std::wstring root = d.root.wstring();
        d.type = ::GetDriveTypeW(root.c_str());

        wchar_t name[MAX_PATH + 1] = {};
        wchar_t fsn[MAX_PATH + 1] = {};
        DWORD serial = 0, comp = 0, flags = 0;
        if (::GetVolumeInformationW(root.c_str(), name, MAX_PATH, &serial, &comp, &flags, fsn, MAX_PATH))
            d.label = Narrow(name);
        else
            d.label.clear();

        ULARGE_INTEGER freeCaller{}, total{}, freeTotal{};
        if (::GetDiskFreeSpaceExW(root.c_str(), &freeCaller, &total, &freeTotal) && total.QuadPart) {
            d.total = (uint64_t)total.QuadPart;
            d.freeb = (uint64_t)freeCaller.QuadPart;
            d.space = true;
        } else {
            d.total = 0;
            d.freeb = 0;
            d.space = false;
        }
        d.caption = (d.label.empty() ? TypeName(d.type) : d.label) + " (" + d.letter + ")";
    }

    static fs::path KnownFolder(REFKNOWNFOLDERID id) {
        PWSTR w = nullptr;
        fs::path out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &w))) out = w;
        if (w) CoTaskMemFree(w);
        return out;
    }

    struct Com {
        bool ok = false;
        Com()  { ok = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)); }
        ~Com() { if (ok) ::CoUninitialize(); }
    };

    // read the quick access shell folder, filesystem folders only
    static std::vector<fs::path> ShellQuick() {
        std::vector<fs::path> out;
        Com com;
        IShellItem* folder = nullptr;
        if (FAILED(::SHCreateItemFromParsingName(L"shell:::{679f85cb-0220-4080-b29b-5540cc05aab6}",
                                                 nullptr, IID_PPV_ARGS(&folder))) || !folder)
            return out;
        IEnumShellItems* en = nullptr;
        if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en))) && en) {
            IShellItem* item = nullptr;
            while (en->Next(1, &item, nullptr) == S_OK && item) {
                SFGAOF attrs = 0;
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attrs)) && (attrs & SFGAO_FOLDER) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    out.push_back(fs::path(path));
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

    void ScanDrives() {
        m_drives.clear();
#ifdef _WIN32
        m_driveMask = ::GetLogicalDrives();
        for (char c = 'A'; c <= 'Z'; ++c) {
            if (!(m_driveMask & (1u << (c - 'A')))) continue;
            Drive d;
            d.letter = std::string(1, c) + ":";
            d.root   = d.letter + "\\";
            Probe(d);
            m_drives.push_back(std::move(d));
        }
#else
        Drive d;
        d.letter  = "/";
        d.root    = "/";
        d.caption = "Filesystem (/)";
        m_drives.push_back(std::move(d));
#endif
    }

    // poll one cheap call every two seconds, requery volumes only on change
    void PollDrives() {
#ifdef _WIN32
        const double now = ImGui::GetTime();
        if (now - m_lastPoll < 2.0) return;
        m_lastPoll = now;
        const unsigned long mask = ::GetLogicalDrives();
        if (mask != m_driveMask) { ScanDrives(); m_tree.clear(); return; }
        for (Drive& d : m_drives) {
            if (d.type != DRIVE_FIXED && d.type != DRIVE_REMOVABLE) continue;
            Quiet quiet;
            ULARGE_INTEGER fc{}, total{}, ft{};
            if (::GetDiskFreeSpaceExW(d.root.wstring().c_str(), &fc, &total, &ft) && total.QuadPart) {
                d.total = (uint64_t)total.QuadPart;
                d.freeb = (uint64_t)fc.QuadPart;
                d.space = true;
            }
        }
#endif
    }

    const Drive* DriveOf(const fs::path& p) const {
        const std::string rn = Lower(p.root_name().string());
        if (rn.empty()) return nullptr;
        for (const Drive& d : m_drives) if (Lower(d.letter) == rn) return &d;
        return nullptr;
    }

    // user pins first, then the shell list, then the standard library folders
    void BuildNav() {
        m_nav.clear();
        std::unordered_set<std::string> seen;
        auto add = [&](const std::string& label, const fs::path& p, Origin o) {
            if (p.empty()) return;
            const std::string k = Key(p);
            if (seen.count(k) || m_hidden.count(k)) return;
            std::error_code ec;
            if (!fs::is_directory(p, ec)) return;
            seen.insert(k);
            NavItem n;
            n.label  = label.empty() ? p.filename().string() : label;
            n.path   = p;
            n.origin = o;
            m_nav.push_back(std::move(n));
        };

#ifdef _WIN32
        for (const std::string& s : m_pins) add(std::string(), fs::path(s), Origin::User);
        for (const fs::path& p : ShellQuick()) add(p.filename().string(), p, Origin::System);
        struct KF { const char* label; const KNOWNFOLDERID* id; };
        const KF kfs[] = {
            { "Desktop",   &FOLDERID_Desktop   },
            { "Downloads", &FOLDERID_Downloads },
            { "Documents", &FOLDERID_Documents },
            { "Pictures",  &FOLDERID_Pictures  },
            { "Music",     &FOLDERID_Music     },
            { "Videos",    &FOLDERID_Videos    },
        };
        for (const KF& kf : kfs) add(kf.label, KnownFolder(*kf.id), Origin::System);
#else
        if (const char* home = std::getenv("HOME")) {
            const fs::path h(home);
            for (const std::string& s : m_pins) add(std::string(), fs::path(s), Origin::User);
            const char* subs[] = { "Desktop", "Downloads", "Documents", "Pictures", "Music", "Videos" };
            for (const char* s : subs) add(s, h / s, Origin::System);
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
        return base / "fb" / "FileBrowser.ini";
#else
        if (const char* h = std::getenv("HOME")) return fs::path(h) / ".config" / "fb_filebrowser.ini";
        return {};
#endif
    }

    // plain text ini with one key value per line
    void LoadConfig() {
        m_pins.clear();
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
            const std::string k = line.substr(0, eq);
            const std::string v = line.substr(eq + 1);
            if      (k == "pin")    { if (!v.empty()) m_pins.push_back(v); }
            else if (k == "hide")   { if (!v.empty()) m_hidden.insert(Lower(v)); }
            else if (k == "view")   { const int n = std::atoi(v.c_str()); if (n >= 0 && n <= 3) m_mode = (View)n; }
            else if (k == "hidden") { m_showHidden = std::atoi(v.c_str()) != 0; }
            else if (k == "group")  { m_group = std::atoi(v.c_str()) != 0; }
            else if (k == "scale")  { const float f = (float)std::atof(v.c_str()); if (f >= 0.7f && f <= 2.0f) m_scale = f; }
            else if (k == "nav")    { const float f = (float)std::atof(v.c_str()); if (f >= 170.0f && f <= 900.0f) { m_navW = f; m_navUser = true; } }
        }
    }

    void Save() {
        if (m_configPath.empty()) return;
        std::error_code ec;
        fs::create_directories(m_configPath.parent_path(), ec);
        std::ofstream out(m_configPath, std::ios::trunc);
        if (!out) return;
        out << "# filebrowser.h settings\n";
        out << "view="   << (int)m_mode << "\n";
        out << "hidden=" << (m_showHidden ? 1 : 0) << "\n";
        out << "group="  << (m_group ? 1 : 0) << "\n";
        out << "scale="  << m_scale << "\n";
        if (m_navUser) out << "nav=" << m_navW << "\n";
        for (const std::string& p : m_pins)   out << "pin="  << p << "\n";
        for (const std::string& p : m_hidden) out << "hide=" << p << "\n";
    }
};

} // namespace fb
