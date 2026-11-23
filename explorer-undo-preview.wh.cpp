// ==WindhawkMod==
// @id              explorer-undo-preview
// @name            Explorer Undo Preview
// @description     Shows exactly what Ctrl+Z (Undo) will do in File Explorer and asks before doing it
// @version         0.1.0
// @author          kfu
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lshlwapi -luuid
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Explorer Undo Preview

Explorer's Ctrl+Z silently undoes the *last file operation anywhere in
Explorer*: a move, rename, copy, delete or "new folder", possibly done in a
different window minutes ago. The right-click menu only says "Undo Move".

This mod intercepts Explorer's undo and redo (Ctrl+Z, Ctrl+Y, the right-click
menu, and the ribbon) and first shows what will happen, for example:

> **Undo: move 3 items back?**
> From: `D:\Archive\Old`
> Back to: `C:\Work\Reports`
> • Q1.xlsx
> • Q2.xlsx
> • Notes

Choose **Undo** to go ahead or **Cancel** to leave everything as it is.

## How it works
Explorer keeps its undo history in the shell's shared "Desktop Undo Manager"
(a standard `IOleUndoManager`). The mod reads the top entry. Each entry
serializes itself by value, and the mod decodes that data to get the
operation type, item names, and source and destination folders.

It hooks one function, `shell32!_UndoThread`, which every undo and redo
path in Explorer goes through.

Developed and tested on Windows 10 22H2 (build 19045).
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- confirmUndo: true
  $name: Confirm before Undo (Ctrl+Z)
- confirmRedo: false
  $name: Confirm before Redo (Ctrl+Y)
- cancelIsDefault: false
  $name: Make "Cancel" the default button
  $description: When enabled, pressing Enter or Space in the dialog cancels instead of undoing
- maxItems: 12
  $name: Maximum item names to list
*/
// ==/WindhawkModSettings==

#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2  // EnumProcessModules -> kernel32!K32EnumProcessModules
#endif
#include <windows.h>
#include <psapi.h>
#include <objbase.h>
#include <ocidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>

#include <string>
#include <vector>

// ============================================================================
// Core: read and decode Explorer's undo stack. No Windhawk dependency, so the
// standalone test tool (tools/undo-peek.cpp) can include this file directly.
// ============================================================================
namespace undopreview {

// Shell's session-wide undo manager (LocalServer32, hosted by Explorer).
static const CLSID CLSID_DesktopUndoManager = {
    0x3eef301f, 0xb596, 0x4c0b, {0xbd, 0x92, 0x01, 0x3b, 0xea, 0xfc, 0xe7, 0x93}};
// shell32!CCommonParentUndoUnit: groups child units into one Ctrl+Z step.
static const CLSID CLSID_CommonParentUndoUnit = {
    0x078759d3, 0x423b, 0x48ad, {0xab, 0x6a, 0x56, 0x38, 0xc2, 0x88, 0x4d, 0xbe}};
// windows.storage!CFileOperationUndoUnit: one IFileOperation batch.
static const CLSID CLSID_FileOperationUndoUnit = {
    0x72c57034, 0x02c4, 0x4e9f, {0xbf, 0x9c, 0xca, 0x71, 0x10, 0x31, 0x75, 0x7e}};
static const IID IID_IOleUndoUnit_ = {
    0x894ad3b0, 0xef97, 0x11ce, {0x9b, 0xc9, 0x00, 0xaa, 0x00, 0x60, 0x8e, 0x01}};
static const IID IID_IOleUndoManager_ = {
    0xd001f200, 0xef97, 0x11ce, {0x9b, 0xc9, 0x00, 0xaa, 0x00, 0x60, 0x8e, 0x01}};

// Operation codes (string table windows.storage.dll 0x1060 + op:
// "Move of '%1'", "Copy of '%1'", "Delete of '%1'", "Rename of '%1' to '%2'",
// "Apply Properties to '%1'", "Creation of '%2'").
enum : DWORD {
    kOpMove = 1,
    kOpCopy = 2,
    kOpDelete = 3,
    kOpRename = 4,
    kOpApplyProperties = 6,
    kOpNew = 7,
};

// CUndoNode / CUndoRoot persisted-field flags (from CUndoNode::Save and
// CUndoRoot::Save in windows.storage.dll).
enum : DWORD {
    kNodeIdl40 = 0x40,
    kNodeIdl80 = 0x80,
    kNodeIdl100 = 0x100,
    kNodeIdl200 = 0x200,
    kNodeSrcName = 0x400,
    kNodeDstName = 0x800,
    kRootNewInfo = 0x1000,
    kRootSrcParent = 0x2000,
    kRootDstParent = 0x4000,
    kRootConnected = 0x8000,
};

struct Item {
    DWORD op = 0;
    DWORD flags = 0;
    DWORD childCount = 0;
    std::wstring srcName;    // name before the operation
    std::wstring dstName;    // name after the operation
    std::wstring srcParent;  // folder before the operation
    std::wstring dstParent;  // folder after the operation
};

struct Entry {
    std::vector<Item> items;
    std::wstring description;  // IOleUndoUnit::GetDescription (often empty)
    bool decoded = false;      // the whole blob was understood
    std::wstring problem;      // why decoding stopped, if it did
    std::vector<BYTE> raw;     // serialized unit (for debugging)
};

class Reader {
   public:
    Reader(const BYTE* p, size_t n) : p_(p), end_(p + n) {}

    bool ok() const { return ok_; }
    const std::wstring& problem() const { return problem_; }
    size_t remaining() const { return (size_t)(end_ - p_); }

    void Fail(const std::wstring& why) {
        if (ok_) {
            ok_ = false;
            problem_ = why;
        }
    }

    bool Bytes(void* out, size_t n) {
        if (!ok_ || remaining() < n) {
            Fail(L"unexpected end of data");
            if (out && n)
                memset(out, 0, n);
            return false;
        }
        if (out && n)
            memcpy(out, p_, n);
        p_ += n;
        return true;
    }
    BYTE U8() {
        BYTE v = 0;
        Bytes(&v, 1);
        return v;
    }
    WORD U16() {
        WORD v = 0;
        Bytes(&v, 2);
        return v;
    }
    DWORD U32() {
        DWORD v = 0;
        Bytes(&v, 4);
        return v;
    }
    GUID Guid() {
        GUID g = {};
        Bytes(&g, sizeof(g));
        return g;
    }

    // shlwapi!IStream_WriteStr: WORD character count, then UTF-16 (no NUL).
    std::wstring Str() {
        WORD n = U16();
        std::wstring s(n, L'\0');
        if (n)
            Bytes(&s[0], n * sizeof(WCHAR));
        if (!ok_)
            s.clear();
        return s;
    }

    // shell32!ILSaveToStream: WORD byte size (including the 2-byte
    // terminator), then the ITEMIDLIST.
    std::vector<BYTE> IdList() {
        WORD n = U16();
        std::vector<BYTE> v(n);
        if (n)
            Bytes(v.data(), n);
        if (ok_ && n && !ValidIdList(v))
            Fail(L"malformed item ID list");
        return v;
    }

   private:
    static bool ValidIdList(const std::vector<BYTE>& v) {
        size_t i = 0;
        while (i + 2 <= v.size()) {
            WORD cb = (WORD)(v[i] | (v[i + 1] << 8));
            if (cb == 0)
                return i + 2 == v.size();
            if (cb < 2)
                return false;
            i += cb;
        }
        return false;
    }

    const BYTE* p_;
    const BYTE* end_;
    bool ok_ = true;
    std::wstring problem_;
};

inline std::wstring IdListToPath(const std::vector<BYTE>& v) {
    if (v.size() < 2)
        return L"";
    auto pidl = (PIDLIST_ABSOLUTE)CoTaskMemAlloc(v.size());
    if (!pidl)
        return L"";
    memcpy(pidl, v.data(), v.size());
    std::wstring result;
    PWSTR name = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &name)) ||
        SUCCEEDED(
            SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEEDITING, &name))) {
        result = name;
        CoTaskMemFree(name);
    }
    CoTaskMemFree(pidl);
    return result;
}

// CUndoNode::Save
inline void ParseNode(Reader& r, Item* out, int depth) {
    if (depth > 256) {
        r.Fail(L"node tree too deep");
        return;
    }
    DWORD flags = r.U32();
    DWORD op = r.U32();
    if (flags & kNodeIdl40)
        r.IdList();
    if (flags & kNodeIdl100)
        r.IdList();
    if (flags & kNodeIdl80)
        r.IdList();
    if (flags & kNodeIdl200)
        r.IdList();
    std::wstring srcName, dstName;
    if (flags & kNodeSrcName)
        srcName = r.Str();
    if (flags & kNodeDstName)
        dstName = r.Str();
    DWORD children = r.U32();
    if (r.ok() && children > r.remaining() / 12) {
        r.Fail(L"implausible child count");
        return;
    }
    for (DWORD i = 0; i < children && r.ok(); i++)
        ParseNode(r, nullptr, depth + 1);
    if (out) {
        out->flags = flags;
        out->op = op;
        out->childCount = children;
        out->srcName = std::move(srcName);
        out->dstName = std::move(dstName);
    }
}

// CUndoRoot::Save
inline void ParseRoot(Reader& r, std::vector<Item>& items, int depth) {
    if (depth > 4) {
        r.Fail(L"connected roots nested too deep");
        return;
    }
    Item item;
    ParseNode(r, &item, 0);
    r.U32();  // "new information" flags
    if (item.flags & kRootNewInfo)
        r.Str();  // e.g. template name for "New > ..." items
    if (item.flags & kRootSrcParent)
        item.srcParent = IdListToPath(r.IdList());
    if (item.flags & kRootDstParent)
        item.dstParent = IdListToPath(r.IdList());
    if (item.flags & kRootConnected) {
        std::vector<Item> connected;  // companion root, not shown separately
        ParseRoot(r, connected, depth + 1);
    }
    if (r.ok())
        items.push_back(std::move(item));
}

// A by-value (OBJREF_CUSTOM) marshaled undo unit, as written by
// CoMarshalInterface -> IMarshal::MarshalInterface.
inline void ParseObjRef(Reader& r, Entry& entry, int depth) {
    if (depth > 16) {
        r.Fail(L"undo units nested too deep");
        return;
    }
    DWORD signature = r.U32();
    DWORD objrefFlags = r.U32();
    r.Guid();  // IID
    if (!r.ok())
        return;
    if (signature != 0x574f454d /* 'MEOW' */ || objrefFlags != 4 /* CUSTOM */) {
        r.Fail(L"undo unit is not marshaled by value");
        return;
    }
    GUID clsid = r.Guid();
    DWORD cbExtension = r.U32();
    r.U32();  // size hint (an upper bound, not exact)
    if (cbExtension) {
        r.Fail(L"unexpected OBJREF extension");
        return;
    }
    if (IsEqualGUID(clsid, CLSID_CommonParentUndoUnit)) {
        // CCommonParentUndoUnit::MarshalInterface: DWORD count, then each
        // child via CoMarshalInterface.
        DWORD count = r.U32();
        for (DWORD i = 0; i < count && r.ok(); i++)
            ParseObjRef(r, entry, depth + 1);
    } else if (IsEqualGUID(clsid, CLSID_FileOperationUndoUnit)) {
        // CFileOperationUndoUnit::_Save: DWORD root count, BYTE flag, roots.
        DWORD roots = r.U32();
        r.U8();
        for (DWORD i = 0; i < roots && r.ok(); i++)
            ParseRoot(r, entry.items, 0);
    } else {
        WCHAR s[64];
        StringFromGUID2(clsid, s, ARRAYSIZE(s));
        r.Fail(std::wstring(L"unsupported undo unit type ") + s);
    }
}

inline HRESULT SerializeUnit(IOleUndoUnit* unit, std::vector<BYTE>& out) {
    out.clear();
    IStream* stream = SHCreateMemStream(nullptr, 0);
    if (!stream)
        return E_OUTOFMEMORY;
    HRESULT hr = CoMarshalInterface(stream, IID_IOleUndoUnit_, unit,
                                    MSHCTX_LOCAL, nullptr, MSHLFLAGS_NORMAL);
    if (SUCCEEDED(hr)) {
        ULARGE_INTEGER size = {};
        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_CUR, &size);
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        out.resize((size_t)size.QuadPart);
        ULONG got = 0;
        if (!out.empty())
            stream->Read(out.data(), (ULONG)out.size(), &got);
        out.resize(got);
        // Standard (by-reference) marshaling would have registered a stub;
        // release it. By-value units have nothing to release.
        if (out.size() < 8 || *(DWORD*)(out.data() + 4) != 4) {
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            CoReleaseMarshalData(stream);
        }
    }
    stream->Release();
    return hr;
}

inline void DecodeUnit(IOleUndoUnit* unit, Entry& entry) {
    BSTR desc = nullptr;
    if (SUCCEEDED(unit->GetDescription(&desc)) && desc) {
        entry.description = desc;
        SysFreeString(desc);
    }
    HRESULT hr = SerializeUnit(unit, entry.raw);
    if (FAILED(hr)) {
        WCHAR s[64];
        wsprintfW(s, L"could not serialize undo unit (0x%08X)", (unsigned)hr);
        entry.problem = s;
        return;
    }
    Reader r(entry.raw.data(), entry.raw.size());
    ParseObjRef(r, entry, 0);
    entry.decoded = r.ok();
    if (!r.ok())
        entry.problem = r.problem();
}

inline HRESULT GetUndoManager(IOleUndoManager** manager) {
    return CoCreateInstance(CLSID_DesktopUndoManager, nullptr,
                            CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                            IID_IOleUndoManager_, (void**)manager);
}

// Reads up to maxEntries units from the undo (or redo) stack, in the order
// the undo manager enumerates them.
inline HRESULT ReadStack(bool redo, size_t maxEntries,
                         std::vector<Entry>& entries) {
    entries.clear();
    IOleUndoManager* manager = nullptr;
    HRESULT hr = GetUndoManager(&manager);
    if (FAILED(hr))
        return hr;
    IEnumOleUndoUnits* e = nullptr;
    hr = redo ? manager->EnumRedoable(&e) : manager->EnumUndoable(&e);
    if (SUCCEEDED(hr) && e) {
        IOleUndoUnit* unit = nullptr;
        ULONG fetched = 0;
        while (entries.size() < maxEntries &&
               e->Next(1, &unit, &fetched) == S_OK && fetched == 1) {
            entries.emplace_back();
            DecodeUnit(unit, entries.back());
            unit->Release();
        }
        e->Release();
    }
    manager->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// Turning decoded entries into text
// ---------------------------------------------------------------------------

inline std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

inline const std::wstring& ShownName(const Item& item, bool preferNew) {
    if (preferNew)
        return item.dstName.empty() ? item.srcName : item.dstName;
    return item.srcName.empty() ? item.dstName : item.srcName;
}

struct Group {
    DWORD op;
    std::wstring srcParent, dstParent;
    std::vector<const Item*> items;
};

inline std::vector<Group> GroupItems(const Entry& entry) {
    std::vector<Group> groups;
    for (const Item& item : entry.items) {
        Group* g = nullptr;
        for (Group& existing : groups) {
            if (existing.op == item.op &&
                existing.srcParent == item.srcParent &&
                existing.dstParent == item.dstParent) {
                g = &existing;
                break;
            }
        }
        if (!g) {
            groups.push_back({item.op, item.srcParent, item.dstParent, {}});
            g = &groups.back();
        }
        g->items.push_back(&item);
    }
    return groups;
}

struct Preview {
    std::wstring headline;  // short, e.g. "Undo: move 3 items back?"
    std::wstring details;   // multi-line body
};

inline void AppendNames(std::wstring& out, const Group& g, bool preferNew,
                        size_t maxNames, size_t& namesShown) {
    for (const Item* item : g.items) {
        if (namesShown >= maxNames)
            break;
        out += L"\n    \u2022 " + ShownName(*item, preferNew);
        namesShown++;
    }
}

// What reversing one group will do, phrased for the undo (or redo) action.
inline std::wstring DescribeGroup(const Group& g, bool redo, size_t maxNames,
                                  size_t& namesShown) {
    size_t n = g.items.size();
    const std::wstring& src = g.srcParent;
    const std::wstring& dst = g.dstParent;
    std::wstring s;
    auto line = [&](const wchar_t* label, const std::wstring& path) {
        if (!path.empty())
            s += std::wstring(L"\n    ") + label + path;
    };
    switch (g.op) {
        case kOpMove:
            if (!redo) {
                s = L"Move " + Plural(n, L"item", L"items") + L" back";
                line(L"from:      ", dst);
                line(L"back to: ", src);
            } else {
                s = L"Move " + Plural(n, L"item", L"items") + L" again";
                line(L"from: ", src);
                line(L"to:     ", dst);
            }
            AppendNames(s, g, !redo, maxNames, namesShown);
            break;
        case kOpCopy:
            if (!redo) {
                s = L"Delete " + Plural(n, L"copy", L"copies") +
                    L" (the originals stay)";
                line(L"in: ", dst);
            } else {
                s = L"Copy " + Plural(n, L"item", L"items") + L" again";
                line(L"from: ", src);
                line(L"to:     ", dst);
            }
            AppendNames(s, g, true, maxNames, namesShown);
            break;
        case kOpDelete:
            if (!redo) {
                s = L"Restore " + Plural(n, L"item", L"items") +
                    L" from the Recycle Bin";
                line(L"to: ", src);
            } else {
                s = L"Delete " + Plural(n, L"item", L"items") + L" again";
                line(L"in: ", src);
            }
            AppendNames(s, g, false, maxNames, namesShown);
            break;
        case kOpRename:
            for (const Item* item : g.items) {
                if (!s.empty())
                    s += L"\n";
                if (!redo)
                    s += L"Rename \"" + item->dstName + L"\" back to \"" +
                         item->srcName + L"\"";
                else
                    s += L"Rename \"" + item->srcName + L"\" to \"" +
                         item->dstName + L"\" again";
            }
            line(L"in: ", src.empty() ? dst : src);
            break;
        case kOpNew:
            if (!redo) {
                s = L"Delete " + Plural(n, L"newly created item",
                                        L"newly created items");
                line(L"in: ", dst.empty() ? src : dst);
            } else {
                s = L"Create " + Plural(n, L"item", L"items") + L" again";
                line(L"in: ", dst.empty() ? src : dst);
            }
            AppendNames(s, g, true, maxNames, namesShown);
            break;
        case kOpApplyProperties:
            s = (redo ? L"Re-apply property changes to "
                      : L"Revert property changes on ") +
                Plural(n, L"item", L"items");
            AppendNames(s, g, false, maxNames, namesShown);
            break;
        default:
            s = L"Operation type " + std::to_wstring(g.op) + L" on " +
                Plural(n, L"item", L"items");
            line(L"from: ", src);
            line(L"to:     ", dst);
            AppendNames(s, g, true, maxNames, namesShown);
            break;
    }
    return s;
}

inline const wchar_t* OpVerb(DWORD op) {
    switch (op) {
        case kOpMove: return L"move";
        case kOpCopy: return L"copy";
        case kOpDelete: return L"delete";
        case kOpRename: return L"rename";
        case kOpNew: return L"new item";
        case kOpApplyProperties: return L"property change";
        default: return L"file operation";
    }
}

inline Preview Describe(const Entry& entry, bool redo, size_t maxNames) {
    Preview p;
    const wchar_t* action = redo ? L"Redo" : L"Undo";
    std::vector<Group> groups = GroupItems(entry);
    if (groups.empty()) {
        p.headline = std::wstring(action) + L" the last file operation?";
        p.details = entry.description.empty()
                        ? L"Explorer didn't record details for this operation."
                        : entry.description;
    } else {
        size_t total = entry.items.size();
        if (groups.size() == 1)
            p.headline = std::wstring(action) + L" " + OpVerb(groups[0].op) +
                         L" of " + Plural(total, L"item", L"items") + L"?";
        else
            p.headline = std::wstring(action) + L" " +
                         Plural(groups.size(), L"operation", L"operations") +
                         L" on " + Plural(total, L"item", L"items") + L"?";
        size_t namesShown = 0;
        for (const Group& g : groups) {
            if (!p.details.empty())
                p.details += L"\n\n";
            p.details += DescribeGroup(g, redo, maxNames, namesShown);
        }
        if (namesShown < total && groups[0].op != kOpRename)
            p.details += L"\n    \u2026 and " +
                         std::to_wstring(total - namesShown) + L" more";
    }
    if (!entry.decoded && !entry.problem.empty())
        p.details += L"\n\n(Some details couldn't be read: " + entry.problem +
                     L")";
    return p;
}

// ---------------------------------------------------------------------------
// Confirmation dialog
// ---------------------------------------------------------------------------

using TaskDialogIndirect_t = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*,
                                              int*, int*, BOOL*);

// TaskDialogIndirect only exists in comctl32 v6. Explorer has it loaded, but
// a plain import from this DLL could bind to v5, so look it up at runtime.
inline TaskDialogIndirect_t FindTaskDialogIndirect() {
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
                            &needed))
        return nullptr;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) {
        WCHAR path[MAX_PATH];
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH))
            continue;
        if (lstrcmpiW(PathFindFileNameW(path), L"comctl32.dll") != 0)
            continue;
        if (auto fn = (TaskDialogIndirect_t)GetProcAddress(
                modules[i], "TaskDialogIndirect"))
            return fn;
    }
    return nullptr;
}

struct DialogOptions {
    bool cancelIsDefault = false;
    size_t maxNames = 12;
};

// Returns true if the undo/redo should go ahead.
inline bool ConfirmTop(bool redo, HWND owner, const DialogOptions& opt,
                       std::wstring* logLine = nullptr) {
    std::vector<Entry> entries;
    HRESULT hr = ReadStack(redo, 1, entries);
    if (FAILED(hr) || entries.empty()) {
        if (logLine)
            *logLine = L"nothing to preview (stack empty or unavailable)";
        return true;  // let Explorer handle it (it beeps when empty)
    }
    Preview p = Describe(entries[0], redo, opt.maxNames);
    if (logLine)
        *logLine = p.headline + L"\n" + p.details;

    const wchar_t* actionText = redo ? L"&Redo" : L"&Undo";
    const int kActionId = 100;
    int pressed = IDCANCEL;
    if (auto taskDialog = FindTaskDialogIndirect()) {
        TASKDIALOG_BUTTON buttons[] = {{kActionId, actionText}};
        TASKDIALOGCONFIG cfg = {sizeof(cfg)};
        cfg.hwndParent = owner;
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        cfg.pszWindowTitle = redo ? L"Redo" : L"Undo";
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszMainInstruction = p.headline.c_str();
        cfg.pszContent = p.details.c_str();
        cfg.cButtons = ARRAYSIZE(buttons);
        cfg.pButtons = buttons;
        cfg.nDefaultButton = opt.cancelIsDefault ? IDCANCEL : kActionId;
        if (FAILED(taskDialog(&cfg, &pressed, nullptr, nullptr)))
            pressed = IDCANCEL;
    } else {
        std::wstring text = p.headline + L"\n\n" + p.details;
        int r = MessageBoxW(owner, text.c_str(), redo ? L"Redo" : L"Undo",
                            MB_OKCANCEL | MB_ICONQUESTION |
                                (opt.cancelIsDefault ? MB_DEFBUTTON2 : 0));
        pressed = r == IDOK ? kActionId : IDCANCEL;
    }
    return pressed == kActionId;
}

}  // namespace undopreview

#ifndef UNDO_PREVIEW_STANDALONE
// ============================================================================
// Windhawk mod: hook shell32!_UndoThread
// ============================================================================

#include <windhawk_utils.h>

struct {
    bool confirmUndo;
    bool confirmRedo;
    bool cancelIsDefault;
    int maxItems;
} g_settings;

// shell32!_UndoThread(void* undo): thread proc started by UndoFromSite /
// RedoFromSite / CUndoRedoCommand::Undo|Redo. Non-null param = undo,
// null = redo. It calls IOleUndoManager::UndoTo/RedoTo(NULL).
using UndoThread_t = DWORD(__cdecl*)(void*);
UndoThread_t UndoThread_Original;

static HWND OwnerWindow() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg && GetWindowThreadProcessId(fg, &pid) &&
        pid == GetCurrentProcessId())
        return fg;
    return nullptr;
}

DWORD __cdecl UndoThread_Hook(void* param) {
    bool redo = param == nullptr;
    bool confirm = redo ? g_settings.confirmRedo : g_settings.confirmUndo;
    if (confirm) {
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        undopreview::DialogOptions opt;
        opt.cancelIsDefault = g_settings.cancelIsDefault;
        opt.maxNames = g_settings.maxItems > 0 ? (size_t)g_settings.maxItems : 1;
        std::wstring log;
        bool proceed = undopreview::ConfirmTop(redo, OwnerWindow(), opt, &log);
        if (SUCCEEDED(hrInit))
            CoUninitialize();
        Wh_Log(L"%s: %s -> %s", redo ? L"Redo" : L"Undo", log.c_str(),
               proceed ? L"proceed" : L"cancelled");
        if (!proceed)
            return 0;
    }
    return UndoThread_Original(param);
}

static void LoadSettings() {
    g_settings.confirmUndo = Wh_GetIntSetting(L"confirmUndo");
    g_settings.confirmRedo = Wh_GetIntSetting(L"confirmRedo");
    g_settings.cancelIsDefault = Wh_GetIntSetting(L"cancelIsDefault");
    g_settings.maxItems = Wh_GetIntSetting(L"maxItems");
}

BOOL Wh_ModInit() {
    Wh_Log(L"Init");
    LoadSettings();

    HMODULE shell32 = LoadLibraryW(L"shell32.dll");
    if (!shell32) {
        Wh_Log(L"shell32.dll not loaded");
        return FALSE;
    }

    WindhawkUtils::SYMBOL_HOOK shell32Hooks[] = {
        {
            {L"unsigned long __cdecl _UndoThread(void *)"},
            (void**)&UndoThread_Original,
            (void*)UndoThread_Hook,
            false,
        },
    };
    if (!WindhawkUtils::HookSymbols(shell32, shell32Hooks,
                                    ARRAYSIZE(shell32Hooks))) {
        Wh_Log(L"Failed to hook shell32!_UndoThread");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
}
#endif  // UNDO_PREVIEW_STANDALONE
