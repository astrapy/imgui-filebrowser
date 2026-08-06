# filebrowser.h

A Windows 11 style file browser for [Dear ImGui](https://github.com/ocornut/imgui) in a single header.

![File browser](docs/screenshot-main.png)

## Features

- Faithful Windows 11 Explorer layout: navigation arrows, breadcrumb address bar, search box, command bar and status bar
- Sidebar with Home, pinned folders and This PC, separated by divider lines like the real Explorer
- Quick access entries read from the actual Windows shell folder, so pins made in Explorer appear automatically
- Expandable folder tree under every sidebar entry and drive
- Live drives with volume label, type and a used space bar that turns red when the drive is nearly full
- Hot plugged drives appear within two seconds
- Four view modes: Details, List, Tiles and Large icons
- Command bar with New, Cut, Copy, Paste, Rename and Delete
- Drag and drop between folders, the sidebar and the parent row. Hold <kbd>Ctrl</kbd> to copy instead of move
- Right click menus on items, empty space and sidebar entries
- Inline rename, natural sorting, date grouping, hidden file toggle and search
- Delete moves items to the Recycle Bin on Windows
- All icons are drawn with `ImDrawList`, no icon font or texture needed
- Settings and pins persist to a plain text ini

## Requirements

| | |
| --- | --- |
| Language | C++17, for `<filesystem>` |
| Dear ImGui | 1.87 or newer |
| Platform | Windows 10 or later, also builds on Linux and macOS |
| Windows libraries | `Shell32`, `Ole32`, `Uuid`, linked automatically through `#pragma comment` |

On Linux and macOS the drive list collapses to a single `/` entry, Quick access falls back to `$HOME`
and its standard subfolders, and delete removes files outright rather than using a recycle bin.

## Installation

Copy `filebrowser.h` next to your Dear ImGui headers and include it.

```cpp
#include "imgui.h"
#include "filebrowser.h"
```

For the authentic look, load Segoe UI before the first frame. The browser works with any font, but
this is what real Explorer uses.

```cpp
ImGui::GetStyle().FontSizeBase = 17.0f;
io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf");
```

## Usage

### As a panel

`Draw()` fills whatever window it is called in and returns the action taken this frame.

```cpp
static fb::FileBrowser browser;

ImGui::Begin("File browser");

switch (browser.Draw())
{
case fb::FileBrowser::Action::Open:
    LoadFile(browser.SelectedPath());
    break;
case fb::FileBrowser::Action::Cancel:
    showBrowser = false;
    break;
default:
    break;
}

ImGui::End();
```

`Action::Open` is returned when the user double clicks a file or presses **Open**. Call
`browser.ShowFooter(false)` to hide the Open and Cancel row when embedding the browser as a plain
panel.

### As a modal dialog

Open the popup once, then call `DrawModal` every frame.

```cpp
if (ImGui::Button("Open file..."))
    ImGui::OpenPopup("Open file");

if (browser.DrawModal("Open file") == fb::FileBrowser::Action::Open)
    LoadFile(browser.SelectedPath());
```

Pass `true` as the second argument to select a folder instead of a file.

```cpp
browser.DrawModal("Choose a folder", /*foldersOnly=*/true);
```

### Reading the selection

```cpp
browser.SelectedPath();     // focused item, empty if nothing is selected
browser.SelectedPaths();    // every selected item
browser.CurrentPath();      // folder currently shown
```

## View modes

Switch from the View menu in the command bar, the toggles in the status bar, or
<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>1</kbd> through <kbd>4</kbd>.

![View modes](docs/screenshot-views.png)

## Right click menus

Right clicking an item offers Open, Cut, Copy, Paste, Pin, New folder, Rename and Delete. Empty space
offers New folder, Paste, Up one level, Refresh, the hidden file toggle and Pin this folder. Sidebar
entries can be opened, unpinned or hidden.

![Context menu](docs/screenshot-menu.png)

## API

### Options

| Method | Description |
| --- | --- |
| `SetScale(float)` | Interface density from `0.7` to `2.0`, default `0.9`. Scales paddings, rows and icons. The font is never touched, so any Dear ImGui version works. |
| `SetView(View)` | One of `Details`, `List`, `Tiles` or `Icons`. |
| `ShowFooter(bool)` | Show or hide the Open and Cancel row. |
| `ShowHidden(bool)` | Include hidden and system files in listings. |
| `SetConfigPath(path)` | Location of the settings file. Pass an empty path to keep settings in memory only. |
| `Navigate(path)` | Change to another folder. |
| `Pin(path)`, `Unpin(path)`, `IsPinned(path)` | Manage sidebar pins owned by the browser. |

### Queries

| Method | Returns |
| --- | --- |
| `SelectedPath()` | The focused item, or an empty path when nothing is selected. |
| `SelectedPaths()` | All selected items. |
| `CurrentPath()` | The folder being shown. |
| `Scale()`, `ViewMode()`, `HiddenShown()`, `FooterShown()`, `ConfigPath()` | Current settings. |

## Keyboard

| Shortcut | Action |
| --- | --- |
| <kbd>Backspace</kbd> | Go up one level |
| <kbd>Enter</kbd> | Open the selection |
| <kbd>F2</kbd> | Rename |
| <kbd>F5</kbd> | Refresh |
| <kbd>Delete</kbd> | Move to the Recycle Bin |
| <kbd>Ctrl</kbd>+<kbd>C</kbd>, <kbd>Ctrl</kbd>+<kbd>X</kbd>, <kbd>Ctrl</kbd>+<kbd>V</kbd> | Copy, cut, paste |
| <kbd>Ctrl</kbd>+<kbd>A</kbd> | Select everything |
| <kbd>Ctrl</kbd>+<kbd>L</kbd> | Edit the current path |
| <kbd>Ctrl</kbd>+<kbd>H</kbd> | Toggle hidden files |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>1</kbd> to <kbd>4</kbd> | Details, List, Tiles, Large icons |

Shortcuts become active once the panel has focus, so they never take keys from the rest of the
application.

## Mouse

| Gesture | Result |
| --- | --- |
| Double click a folder | Enter it |
| Double click the `..` row | Go up one level |
| Click a breadcrumb segment | Jump to that folder |
| Click the empty part of the address bar | Edit the path as text |
| Drag onto a folder, a sidebar entry or the `..` row | Move the selection there |
| <kbd>Ctrl</kbd> while dropping | Copy instead of move |
| Drag the pane divider | Resize the sidebar |
| Drag a column edge | Resize that column |
| Click a column header | Sort by it, click again to reverse |

## Configuration

Settings are written to `%APPDATA%\fb\FileBrowser.ini` on Windows and
`~/.config/fb_filebrowser.ini` elsewhere.

```ini
# filebrowser.h settings
view=0
hidden=0
group=0
scale=0.9
nav=280
pin=C:\Users\You\Projects
hide=C:\Users\You\Videos
```

`pin` entries are folders pinned from inside the browser. `hide` entries are sidebar rows the user
chose to hide. `nav` is only written after the sidebar has been resized by hand. Use
`SetConfigPath()` to relocate the file, or pass an empty path to disable it.

## Limitations

**Pinned and frequent folders cannot be separated.** Windows reports every entry in the Quick access
folder as pinned, including merely frequent ones, so the sidebar shows the complete list. This
matches what Explorer itself displays.

**Entries added by Windows cannot be unpinned.** Removing them requires the shell verb, which this
header does not invoke. Use **Hide from sidebar** instead. Folders pinned from inside the browser
can be unpinned normally.

**Listings are read on the calling thread.** This is not noticeable on local disks, but a slow
network share will cost a frame when its folder is opened.

**No shell integration.** Icons are drawn rather than taken from the shell, and double clicking a
file reports it to the application rather than launching it.

## Notes

Built and tested against Dear ImGui 1.93. The header is plain ASCII, compiles clean at `/W4`, and
depends on nothing beyond Dear ImGui, the standard library and a few Win32 system libraries.
