# Explorer Undo Preview

A [Windhawk](https://windhawk.net) mod. When you press Ctrl+Z in File Explorer,
it shows exactly what will be undone and asks before doing it:

```
Undo move of 8 items?
Move 8 items back
    from:    D:\Archive\Old
    back to: C:\Users\you\Downloads
    • file1.pdf
    • file2.pdf
    …
                                   [ Undo ]  [ Cancel ]
```

In the folder background menu (right-click empty space), Undo and Redo say
what they'll do ("Undo Move: 8 items back to Downloads"), and an
**Undo history ▸** submenu lists recent undo and redo entries. Picking an
entry undoes or redoes it and everything newer, after confirming.

## Install

1. Install Windhawk.
2. In Windhawk, choose **Create a New Mod**, replace the template with
   `explorer-undo-preview.wh.cpp`, then click **Compile Mod** and **Enable**.
3. Each feature can be turned off in the mod's **Settings** tab.

## How it works

- Explorer's undo history lives in the shell's session-wide **Desktop Undo
  Manager** (`CLSID {3EEF301F-B596-4C0B-BD92-013BEAFCE793}`), a standard
  `IOleUndoManager`. Any process can enumerate it.
- Each entry (`CCommonParentUndoUnit` containing `CFileOperationUndoUnit`s)
  marshals *by value*. `CoMarshalInterface(MSHCTX_LOCAL)` produces its
  serialized form: op code, old and new names, and source and destination
  folder ID lists. The mod decodes that; see `ParseObjRef` / `ParseRoot` /
  `ParseNode`.
- Every Explorer undo or redo path (Ctrl+Z, Ctrl+Y, context menu, ribbon) goes
  through `shell32!_UndoThread(void* undo)`. The mod hooks it and shows the
  preview before calling the original.
- `shell32!GetUndoRedoText` builds the "Undo Move" label and the ribbon
  tooltip. The folder view's background menu (Undo/Redo are IDs `0x701B` and
  `0x7020`) is wrapped by `CContextMenuOnHMENU(HMENU, HWND)`, where the mod
  adds the submenu (IDs `0x7E00`–`0x7E7F`). The wrapper sends `WM_COMMAND`
  with the original ID back to the view, and `CDefView::_OnCommand` handles
  the history clicks.
- Multi-step undo calls the original `_UndoThread` once per step, and before
  each step checks that the top entry is still the one that was shown.

The format was worked out from the public symbols of `windows.storage.dll`
and `shell32.dll` 10.0.19041.7802 (Windows 10 22H2) and may differ in other
builds. If decoding fails, the dialog says so and still lets you choose.

Tested on Windows 10 22H2 with move, rename, delete (Recycle Bin), and new
file/folder entries. Copy is decoded but untested.

## Test tool

`tools/undo-peek.exe` reads the stack from outside Explorer, without Windhawk:

```
undo-peek            # list the undo stack (first = what Ctrl+Z undoes next)
undo-peek --redo     # list the redo stack
undo-peek --items    # also print the decoded fields of each item
undo-peek --raw      # also hex-dump each serialized entry
undo-peek --dialog   # show the confirmation dialog for the top entry (doesn't undo)
```

Build it with `tools/build.ps1 -Clang <path to llvm-mingw clang++.exe>`.
