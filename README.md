# filebrowser.h

A single-header file browser for [Dear ImGui](https://github.com/ocornut/imgui), modelled on Windows Explorer.

![File browser](docs/screenshot-main.png)

## Features

- Sidebar tree with **Quick access** read from the real Windows shell folder, so pins made in Explorer appear here
- **This PC** section listing every drive with its volume label, type and a used/free capacity bar
- Four view modes: Details, List, Tiles and Large icons
- Drag and drop inside the browser. Drops move by default and copy while <kbd>Ctrl</kbd> is held
- Right-click menus for New folder, Cut, Copy, Paste, Rename, Delete and Pin
- Breadcrumb address bar that turns into an editable path field
- Multi-select, natural sorting with folders first, search, hidden-file toggle and optional grouping by date
- Delete moves items to the Recycle Bin on Windows
- Icons are drawn with `ImDrawList`, so no icon font is required
- Pins and preferences persist to a plain-text ini

## Requirements

| | |
| --- | --- |
| Language | C++17, for `<filesystem>` |
| Dear ImGui | 1.87 or newer |
| Windows libraries | `Shell32`, `Ole32`, `Uuid`, linked automatically through `#pragma comment` |

The header also builds on Linux and macOS. There, drives collapse to a single `/` entry, Quick access
falls back to `$HOME` and its standard subfolders, and delete removes files outright rather than using
a recycle bin.

## Installation

Copy `filebrowser.h` into your project alongside the Dear ImGui headers, then include it.

```cpp
#include "imgui.h"
#include "filebrowser.h"
```

## Usage

### As a panel

`Draw()` fills whatever window it is called in and returns the action taken this frame.

```cpp
static imex::FileExplorer browser;

ImGui::Begin("File browser");

switch (browser.Draw())
{
case imex::FileExplorer::Action::Open:
    LoadFile(browser.SelectedPath());
    break;
case imex::FileExplorer::Action::Cancel:
    showBrowser = false;
    break;
default:
    break;
}

ImGui::End();
```

`Action::Open` is returned when the user double-clicks a file or presses **Open**. Call
`browser.ShowFooter(false)` to hide the Open and Cancel row when embedding the browser as a plain
panel.

### As a modal dialog

Open the popup once, then call `DrawPickerModal` every frame.

```cpp
if (ImGui::Button("Open file..."))
    ImGui::OpenPopup("Open file");

if (browser.DrawPickerModal("Open file") == imex::FileExplorer::Action::Open)
    LoadFile(browser.SelectedPath());
```

Pass `true` as the second argument to select a folder instead of a file.

```cpp
browser.DrawPickerModal("Choose a folder", /*foldersOnly=*/true);
```

### Reading the selection

```cpp
browser.SelectedPath();     // focused item, empty if nothing is selected
browser.SelectedPaths();    // every selected item
browser.CurrentPath();      // folder currently shown
```

## View modes

Switch from the toolbar or with <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>1</kbd> through <kbd>4</kbd>.

![View modes](docs/screenshot-views.png)

## Right-click menus

File operations live in the context menus rather than the toolbar. Right-clicking an item offers Open,
Cut, Copy, Paste, Pin, New folder, Rename and Delete. Right-clicking empty space offers New folder,
Paste, Up one level, Refresh, the hidden-file toggle and Pin this folder.

![Context menu](docs/screenshot-menu.png)

## API

### Options

| Method | Description |
| --- | --- |
| `SetUiScale(float)` | Interface density, default `0.85`. Lower values fit more rows on screen. Affects padding, spacing, row heights and icon sizes. The font is never changed, so the call is safe on any Dear ImGui version. |
| `ShowFooter(bool)` | Show or hide the Open and Cancel row. |
| `SetViewMode(ViewMode)` | One of `Details`, `List`, `Tiles` or `Icons`. |
| `ShowHiddenFiles(bool)` | Include hidden and system files in listings. |
| `SetConfigPath(path)` | Location of the settings file. Pass an empty path to keep settings in memory only. |
| `Navigate(path)` | Change to another folder. |
| `Pin(path)`, `Unpin(path)`, `IsPinned(path)` | Manage Quick access pins owned by the browser. |

### Queries

| Method | Returns |
| --- | --- |
| `SelectedPath()` | The focused item, or an empty path when nothing is selected. |
| `SelectedPaths()` | All selected items. |
| `CurrentPath()` | The folder being shown. |
| `UiScale()`, `GetViewMode()`, `HiddenFilesShown()`, `FooterShown()`, `ConfigPath()` | Current settings. |

```cpp
browser.SetUiScale(0.75f);
browser.ShowFooter(false);
browser.SetViewMode(imex::FileExplorer::ViewMode::Icons);

for (const auto& path : browser.SelectedPaths())
    Process(path);
```

## Keyboard

| Shortcut | Action |
| --- | --- |
| <kbd>Backspace</kbd> | Go up one level |
| <kbd>Enter</kbd> | Open the selection |
| <kbd>F2</kbd> | Rename |
| <kbd>F5</kbd> | Refresh |
| <kbd>Delete</kbd> | Move to the Recycle Bin |
| <kbd>Ctrl</kbd>+<kbd>C</kbd>, <kbd>Ctrl</kbd>+<kbd>X</kbd>, <kbd>Ctrl</kbd>+<kbd>V</kbd> | Copy, cut, paste |
| <kbd>Ctrl</kbd>+<kbd>L</kbd> | Edit the current path |
| <kbd>Ctrl</kbd>+<kbd>H</kbd> | Toggle hidden files |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>1</kbd> to <kbd>4</kbd> | Details, List, Tiles, Large icons |

Shortcuts become active once the panel has ImGui focus, so they never take keys from the rest of the
application.

## Mouse

| Gesture | Result |
| --- | --- |
| Double-click a folder | Enter it |
| Double-click the `...` row | Go up one level |
| Drag onto a folder row or sidebar node | Move the selection there |
| <kbd>Ctrl</kbd> while dropping | Copy instead of move |
| Click the address bar | Edit the path as text |
| Drag the divider | Resize the sidebar |

## Configuration

Settings are written to `%APPDATA%\imex\FileBrowser.ini` on Windows and
`~/.config/imex_filebrowser.ini` elsewhere.

```ini
# filebrowser.h settings
view=0
hidden=0
scale=0.85
pin=C:\Users\You\Projects
hide=C:\Users\You\Videos
```

`pin` entries are folders pinned from inside the browser. `hide` entries are Quick access rows the
user chose to hide. Use `SetConfigPath()` to relocate the file, or pass an empty path to disable it.

## Limitations

**Pinned and frequent folders cannot be separated.** Windows reports `System.Home.IsPinned` as true
for every entry in the Quick access folder, including merely frequent ones. The sidebar therefore
shows the complete list, matching the behaviour of Explorer's own Quick access node.

**Entries added by Windows cannot be un-pinned.** Removing them requires the shell's own verb, which
this header does not invoke. Use **Hide from Quick access** instead. The choice is stored in the ini.
Folders pinned from inside the browser can be un-pinned normally.

**Listings are read on the calling thread.** This is not noticeable on local disks, but a slow network
share will cost a frame when its folder is opened.

**No shell integration.** Icons are drawn rather than taken from the shell, and double-clicking a file
reports it to the application rather than launching it.

## Notes

Built and tested against Dear ImGui 1.93. The header is plain ASCII, compiles clean at
`/W4 /permissive-`, and depends on nothing beyond Dear ImGui and the standard library.
