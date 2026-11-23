# Explorer Undo Preview

A [Windhawk](https://windhawk.net) mod. When you press Ctrl+Z in File Explorer,
it shows exactly what will be undone and asks before doing it:

```
Undo move of 8 items?
Move 8 items back
    from:    X:\Some\Folder
    back to: C:\Users\you\Downloads
    • file1.cbz
    • file2.cbz
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
3. Settings: confirm Undo (on by default), confirm Redo, make Cancel the
   default button, and the maximum number of names to list.

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
- Menu features: `shell32!GetUndoRedoText` builds the "Undo Move" label
  (and the ribbon tooltip). The background menu (Undo/Redo are IDs `0x701B`
  and `0x7020`) is wrapped by `CContextMenuOnHMENU(HMENU, HWND)`; the mod
  adds the submenu there (IDs `0x7E00`–`0x7E7F`). That wrapper renumbers
  items and sends `WM_COMMAND` with the original ID back to the view, where
  `CDefView::_OnCommand` catches the history clicks. (`_InitEditUndoRedo` is
  a static function whose public symbol has no decorated name, so Windhawk
  can't hook it.)
  Multi-step undo calls the original `_UndoThread` once per step, and before
  each step checks that the top entry is still the one that was shown.

The format was worked out from `windows.storage.dll` and `shell32.dll` public
symbols, version 10.0.19041.7802 (Windows 10 22H2). It could change in other
builds. If decoding fails, the dialog says so and still lets you choose.

## Verified

| Operation | Decoded | Tested |
|---|---|---|
| Move | ✅ | real Explorer drag/drop entries and a test move |
| Rename | ✅ | test rename |
| Delete (Recycle Bin) | ✅ | real entry and a test delete |
| Redo of move/rename/delete | ✅ | test entries |
| New folder/file | ✅ | real Explorer entries |
| Copy | text written, not yet exercised | – |

## Test tool

`tools/undo-peek.exe` reads the stack from outside Explorer, with no Windhawk needed:

```
undo-peek            # list the undo stack (index 0 = what Ctrl+Z undoes next)
undo-peek --redo     # list the redo stack
undo-peek --items    # raw decoded fields per item
undo-peek --raw      # hex dump of each serialized entry
undo-peek --dialog   # show the confirmation dialog for the top entry (doesn't undo)
undo-peek --menu <folder>   # dump that folder's background context menu items
```

Build it with `tools/build.ps1 -Clang <path to llvm-mingw clang++.exe>`.
