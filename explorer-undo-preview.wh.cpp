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

> **Undo move of 3 items?**
> Move 3 items back
> from: `D:\Archive\Old`
> back to: `C:\Work\Reports`
> • Q1.xlsx
> • Q2.xlsx
> • Notes

Choose **Undo** to go ahead or **Cancel** to leave everything as it is.

In the folder background menu (right-click empty space):

- **Undo/Redo say what they'll do**: "Undo Move: 8 items back to Downloads"
  instead of "Undo Move". The ribbon's tooltip uses the same text.
- **Undo history ▸**: lists the last entries of the undo and redo stacks.
  Picking one undoes (or redoes) it *and everything newer*, after a
  confirmation that lists every step. Explorer's undo is a stack, so an older
  step can't be undone on its own.

## How it works
Explorer keeps its undo history in the shell's shared "Desktop Undo Manager"
(a standard `IOleUndoManager`). Each entry serializes itself by value, and the
mod decodes that data to get the operation type, item names, and source and
destination folders.

Hooks in shell32: `_UndoThread` (every undo/redo path goes through it),
`GetUndoRedoText` (menu and tooltip text), the `CContextMenuOnHMENU`
constructor (adds the submenu to the background menu), and
`CDefView::_OnCommand` (handles submenu clicks). The last three are optional;
if they aren't found, only the confirmation works.

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
- richMenuText: true
  $name: Descriptive Undo/Redo menu text
  $description: 'Turns "Undo Move" in the folder background menu into e.g. "Undo Move: 8 items back to Downloads" (also used for the ribbon tooltip)'
- historyMenu: true
  $name: '"Undo history" submenu'
  $description: 'Adds an "Undo history" submenu after Undo/Redo in the folder background menu. Picking an entry undoes it and everything newer, after confirmation.'
- historyEntries: 10
  $name: Entries per list in the history submenu
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

// Everything in this namespace is independent of Windhawk; tools/undo-peek.cpp
// includes this file with UNDO_PREVIEW_STANDALONE defined.
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
    std::vector<BYTE> raw;     // serialized unit
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
    std::wstring headline;  // e.g. "Undo move of 3 items?"
    std::wstring details;
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
        case kOpNew: return L"creation";
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

inline std::wstring Ellipsize(const std::wstring& s, size_t max) {
    if (s.size() <= max || max < 2)
        return s;
    return s.substr(0, max - 1) + L"…";
}

inline std::wstring Leaf(const std::wstring& path) {
    if (path.empty())
        return L"?";
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    const wchar_t* leaf = PathFindFileNameW(p.c_str());
    return Ellipsize(*leaf ? leaf : p.c_str(), 28);
}

inline std::wstring Quoted(const std::wstring& name) {
    return L"'" + Ellipsize(name, 36) + L"'";
}

// One line for menus and tooltips, e.g. "Move: 8 items back to Downloads".
inline std::wstring ShortSummary(const Entry& entry, bool redo) {
    std::vector<Group> groups = GroupItems(entry);
    if (groups.empty())
        return entry.description.empty() ? L"last operation"
                                          : entry.description;
    if (groups.size() > 1)
        return Plural(groups.size(), L"operation", L"operations") + L" on " +
               Plural(entry.items.size(), L"item", L"items");
    const Group& g = groups[0];
    size_t n = g.items.size();
    auto what = [&](bool preferNew) {
        return n == 1 ? Quoted(ShownName(*g.items[0], preferNew))
                      : Plural(n, L"item", L"items");
    };
    switch (g.op) {
        case kOpMove:
            return redo ? L"Move: " + what(false) + L" to " + Leaf(g.dstParent)
                        : L"Move: " + what(true) + L" back to " +
                              Leaf(g.srcParent);
        case kOpCopy:
            return redo ? L"Copy: " + what(false) + L" to " + Leaf(g.dstParent)
                        : L"Copy: delete " +
                              (n == 1 ? L"copy " + what(true)
                                      : Plural(n, L"copy", L"copies")) +
                              L" in " + Leaf(g.dstParent);
        case kOpDelete:
            return redo ? L"Delete: " + what(false)
                        : L"Delete: restore " + what(false);
        case kOpRename:
            if (n == 1) {
                const Item& it = *g.items[0];
                return redo ? L"Rename: " + Quoted(it.srcName) + L" → " +
                                  Quoted(it.dstName)
                            : L"Rename: " + Quoted(it.dstName) + L" → " +
                                  Quoted(it.srcName);
            }
            return L"Rename: " + Plural(n, L"item", L"items");
        case kOpNew:
            return redo ? L"New: " + what(true) : L"New: delete " + what(true);
        case kOpApplyProperties:
            return L"Properties: " + what(false);
        default:
            return L"Operation on " + what(true);
    }
}

// Menu text treats '&' as a mnemonic marker.
inline std::wstring EscapeAmpersands(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'&')
            r += L'&';
        r += c;
    }
    return r;
}

// Two entries are the same undo step if they serialize identically.
inline bool SameEntry(const Entry& a, const Entry& b) {
    return !a.raw.empty() && a.raw == b.raw;
}

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

// Asks whether to undo (or redo) the given steps, newest first. Returns true
// if the user chose to go ahead.
inline bool ConfirmSteps(const std::vector<Entry>& steps, bool redo,
                         HWND owner, const DialogOptions& opt,
                         std::wstring* logLine = nullptr) {
    if (steps.empty())
        return true;
    Preview p;
    if (steps.size() == 1) {
        p = Describe(steps[0], redo, opt.maxNames);
    } else {
        p.headline = std::wstring(redo ? L"Redo " : L"Undo ") +
                     std::to_wstring(steps.size()) + L" steps?";
        size_t perStep = opt.maxNames / steps.size();
        if (perStep < 3)
            perStep = 3;
        for (size_t i = 0; i < steps.size(); i++) {
            if (!p.details.empty())
                p.details += L"\n\n";
            p.details += std::to_wstring(i + 1) + L". " +
                         Describe(steps[i], redo, perStep).details;
        }
    }
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

// Confirms the step Ctrl+Z (or Ctrl+Y) would perform next.
inline bool ConfirmTop(bool redo, HWND owner, const DialogOptions& opt,
                       std::wstring* logLine = nullptr) {
    std::vector<Entry> entries;
    HRESULT hr = ReadStack(redo, 1, entries);
    if (FAILED(hr) || entries.empty()) {
        if (logLine)
            *logLine = L"nothing to preview (stack empty or unavailable)";
        return true;  // let Explorer handle it (it beeps when empty)
    }
    return ConfirmSteps(entries, redo, owner, opt, logLine);
}

}  // namespace undopreview

#ifndef UNDO_PREVIEW_STANDALONE

#include <windhawk_utils.h>

struct {
    bool confirmUndo;
    bool confirmRedo;
    bool cancelIsDefault;
    int maxItems;
    bool richMenuText;
    bool historyMenu;
    int historyEntries;
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

static undopreview::DialogOptions DialogOpts() {
    undopreview::DialogOptions opt;
    opt.cancelIsDefault = g_settings.cancelIsDefault;
    opt.maxNames = g_settings.maxItems > 0 ? (size_t)g_settings.maxItems : 1;
    return opt;
}

DWORD __cdecl UndoThread_Hook(void* param) {
    bool redo = param == nullptr;
    bool confirm = redo ? g_settings.confirmRedo : g_settings.confirmUndo;
    if (confirm) {
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool proceed =
            undopreview::ConfirmTop(redo, OwnerWindow(), DialogOpts());
        if (SUCCEEDED(hrInit))
            CoUninitialize();
        if (!proceed)
            return 0;
    }
    return UndoThread_Original(param);
}

// shell32!GetUndoRedoText(buffer, cch, textType, undo) builds the label of
// the folder background menu's Undo/Redo items ("&Undo Move\tCtrl+Z"), their
// status bar help text, and the command bar tooltip.
using GetUndoRedoText_t = HRESULT(__cdecl*)(WCHAR*, UINT, int, BOOL);
GetUndoRedoText_t GetUndoRedoText_Original;

HRESULT __cdecl GetUndoRedoText_Hook(WCHAR* buffer, UINT cch, int type,
                                     BOOL undo) {
    HRESULT hr = GetUndoRedoText_Original(buffer, cch, type, undo);
    if (FAILED(hr) || !g_settings.richMenuText || !buffer || cch < 24 ||
        !buffer[0])
        return hr;

    std::vector<undopreview::Entry> entries;
    if (FAILED(undopreview::ReadStack(!undo, 1, entries)) || entries.empty() ||
        entries[0].items.empty())
        return hr;

    std::wstring original = buffer;
    size_t tab = original.find(L'\t');
    std::wstring accel = tab == std::wstring::npos ? L"" : original.substr(tab);
    bool menuText = original[0] == L'&' || !accel.empty();
    std::wstring prefix = undo ? L"Undo " : L"Redo ";
    if (original[0] == L'&')
        prefix = L"&" + prefix;

    std::wstring summary = undopreview::ShortSummary(entries[0], !undo);
    if (menuText)
        summary = undopreview::EscapeAmpersands(summary);
    size_t used = prefix.size() + accel.size() + 1;
    if (used + 8 > cch)
        return hr;
    summary = undopreview::Ellipsize(summary, cch - used);
    std::wstring text = prefix + summary + accel;
    lstrcpynW(buffer, text.c_str(), (int)cch);
    return hr;
}

// Background menu command IDs live in 0x7000-0x7FFF (shell32 menu resource
// 215 uses up to 0x7B03); this range is unused there.
constexpr UINT kHistoryParentId = 0x7E00;
constexpr UINT kHistoryFirstId = 0x7E01;
constexpr UINT kHistoryLastId = 0x7E7F;
constexpr UINT kUndoItemId = 0x701B;  // SFVIDM_EDIT_UNDO
constexpr UINT kRedoItemId = 0x7020;

struct HistoryAction {
    UINT id;
    bool redo;
    size_t steps;
};

// Snapshot taken when the menu was built, so a click acts on exactly the
// entries that were shown.
struct {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<undopreview::Entry> undo, redo;
    std::vector<HistoryAction> actions;
} g_history;

using ContextMenuOnHMENU_ctor_t = void*(__cdecl*)(void*, HMENU, HWND);
ContextMenuOnHMENU_ctor_t ContextMenuOnHMENU_ctor_Original;

using DefView_OnCommand_t = __int64(__cdecl*)(void*, unsigned __int64,
                                               __int64);
DefView_OnCommand_t DefView_OnCommand_Original;

static int FindItemPos(HMENU menu, UINT id) {
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; i++)
        if (GetMenuItemID(menu, i) == id)
            return i;
    return -1;
}

static void AddHistoryMenu(HMENU menu) {
    if (!DefView_OnCommand_Original)
        return;  // clicks couldn't be handled
    if (GetMenuState(menu, kHistoryParentId, MF_BYCOMMAND) != (UINT)-1)
        DeleteMenu(menu, kHistoryParentId, MF_BYCOMMAND);

    int pos = FindItemPos(menu, kRedoItemId);
    if (pos < 0)
        pos = FindItemPos(menu, kUndoItemId);
    if (pos < 0)
        return;

    size_t limit = g_settings.historyEntries < 1    ? 1
                   : g_settings.historyEntries > 50 ? 50
                                                    : g_settings.historyEntries;
    std::vector<undopreview::Entry> undo, redo;
    undopreview::ReadStack(false, limit, undo);
    undopreview::ReadStack(true, limit, redo);
    if (undo.empty() && redo.empty())
        return;

    UINT needed = (UINT)(undo.size() + redo.size() + 2);
    if (kHistoryFirstId + needed > kHistoryLastId)
        return;
    for (UINT id = kHistoryParentId; id <= kHistoryFirstId + needed; id++) {
        if (GetMenuState(menu, id, MF_BYCOMMAND) != (UINT)-1) {
            Wh_Log(L"Menu ID 0x%X already in use; not adding history", id);
            return;
        }
    }

    HMENU sub = CreatePopupMenu();
    std::vector<HistoryAction> actions;
    UINT id = kHistoryFirstId;
    auto addList = [&](const std::vector<undopreview::Entry>& list, bool redo) {
        AppendMenuW(sub, MF_STRING | MF_GRAYED, id++,
                    redo ? L"Redo (next first)" : L"Undo (newest first)");
        for (size_t i = 0; i < list.size(); i++) {
            std::wstring label =
                std::to_wstring(i + 1) + L".  " +
                undopreview::EscapeAmpersands(
                    undopreview::ShortSummary(list[i], redo));
            if (i == 0)
                label += redo ? L"\tCtrl+Y" : L"\tCtrl+Z";
            AppendMenuW(sub, MF_STRING, id, label.c_str());
            actions.push_back({id, redo, i + 1});
            id++;
        }
    };
    if (!undo.empty())
        addList(undo, false);
    if (!redo.empty()) {
        if (!undo.empty())
            AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        addList(redo, true);
    }

    WCHAR title[] = L"Undo &history";
    MENUITEMINFOW mii = {sizeof(mii)};
    mii.fMask = MIIM_ID | MIIM_STRING | MIIM_SUBMENU;
    mii.wID = kHistoryParentId;
    mii.hSubMenu = sub;
    mii.dwTypeData = title;
    if (!InsertMenuItemW(menu, pos + 1, TRUE, &mii)) {
        DestroyMenu(sub);
        return;
    }

    AcquireSRWLockExclusive(&g_history.lock);
    g_history.undo = std::move(undo);
    g_history.redo = std::move(redo);
    g_history.actions = std::move(actions);
    ReleaseSRWLockExclusive(&g_history.lock);
}

// CContextMenuOnHMENU(HMENU, HWND) wraps the folder view's background menu
// (built by CDefView::_Create_BackgrndHMENU, Undo/Redo already set up) in an
// IContextMenu. Its QueryContextMenu renumbers every item (submenus too) and
// InvokeCommand sends WM_COMMAND with the original ID back to the view,
// which is where DefView_OnCommand_Hook picks up the history items.
void* __cdecl ContextMenuOnHMENU_ctor_Hook(void* pThis, HMENU menu,
                                           HWND hwnd) {
    if (g_settings.historyMenu && menu)
        AddHistoryMenu(menu);  // no-op unless the menu has Undo/Redo
    return ContextMenuOnHMENU_ctor_Original(pThis, menu, hwnd);
}

struct HistoryJob {
    bool redo;
    std::vector<undopreview::Entry> steps;
};

// Performs the steps one at a time with Explorer's own undo routine, checking
// before each one that the top of the stack is the entry that was shown.
static DWORD WINAPI HistoryJobThread(void* param) {
    HistoryJob* job = (HistoryJob*)param;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (size_t i = 0; i < job->steps.size(); i++) {
        std::vector<undopreview::Entry> top;
        undopreview::ReadStack(job->redo, 1, top);
        if (top.empty() || !undopreview::SameEntry(top[0], job->steps[i])) {
            std::wstring msg =
                L"The undo history changed, so the remaining steps were not "
                L"performed.\n\nCompleted " +
                std::to_wstring(i) + L" of " +
                std::to_wstring(job->steps.size()) + L" steps.";
            MessageBoxW(nullptr, msg.c_str(), L"Undo history",
                        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            break;
        }
        UndoThread_Original(job->redo ? nullptr : (void*)1);
    }
    if (SUCCEEDED(hrInit))
        CoUninitialize();
    delete job;
    return 0;
}

static bool HandleHistoryCommand(UINT id) {
    auto job = new HistoryJob();
    bool found = false;
    AcquireSRWLockShared(&g_history.lock);
    for (const HistoryAction& a : g_history.actions) {
        if (a.id != id)
            continue;
        const auto& list = a.redo ? g_history.redo : g_history.undo;
        if (a.steps <= list.size()) {
            job->redo = a.redo;
            job->steps.assign(list.begin(), list.begin() + a.steps);
            found = true;
        }
        break;
    }
    ReleaseSRWLockShared(&g_history.lock);
    if (!found) {
        delete job;
        return false;
    }

    bool proceed = undopreview::ConfirmSteps(job->steps, job->redo,
                                             OwnerWindow(), DialogOpts());
    if (!proceed) {
        delete job;
        return true;
    }
    HANDLE thread = CreateThread(nullptr, 0, HistoryJobThread, job, 0, nullptr);
    if (thread)
        CloseHandle(thread);
    else
        delete job;
    return true;
}

// CDefView::_OnCommand(wParam, lParam): WM_COMMAND handler of the folder
// view; background menu picks arrive here with their menu IDs.
__int64 __cdecl DefView_OnCommand_Hook(void* pThis, unsigned __int64 wParam,
                                       __int64 lParam) {
    UINT id = LOWORD(wParam);
    if (id >= kHistoryFirstId && id <= kHistoryLastId &&
        HandleHistoryCommand(id))
        return 0;
    return DefView_OnCommand_Original(pThis, wParam, lParam);
}

static void LoadSettings() {
    g_settings.confirmUndo = Wh_GetIntSetting(L"confirmUndo");
    g_settings.confirmRedo = Wh_GetIntSetting(L"confirmRedo");
    g_settings.cancelIsDefault = Wh_GetIntSetting(L"cancelIsDefault");
    g_settings.maxItems = Wh_GetIntSetting(L"maxItems");
    g_settings.richMenuText = Wh_GetIntSetting(L"richMenuText");
    g_settings.historyMenu = Wh_GetIntSetting(L"historyMenu");
    g_settings.historyEntries = Wh_GetIntSetting(L"historyEntries");
}

BOOL Wh_ModInit() {
    LoadSettings();

    HMODULE shell32 = LoadLibraryW(L"shell32.dll");
    if (!shell32)
        return FALSE;

    WindhawkUtils::SYMBOL_HOOK shell32Hooks[] = {
        {
            {L"unsigned long __cdecl _UndoThread(void *)"},
            (void**)&UndoThread_Original,
            (void*)UndoThread_Hook,
            false,
        },
        // Optional: the menu features switch off if these aren't found.
        {
            {L"long __cdecl GetUndoRedoText(unsigned short *,unsigned int,int,int)"},
            (void**)&GetUndoRedoText_Original,
            (void*)GetUndoRedoText_Hook,
            true,
        },
        {
            {L"public: __cdecl CContextMenuOnHMENU::CContextMenuOnHMENU(struct HMENU__ *,struct HWND__ *)"},
            (void**)&ContextMenuOnHMENU_ctor_Original,
            (void*)ContextMenuOnHMENU_ctor_Hook,
            true,
        },
        {
            {L"private: __int64 __cdecl CDefView::_OnCommand(unsigned __int64,__int64)"},
            (void**)&DefView_OnCommand_Original,
            (void*)DefView_OnCommand_Hook,
            true,
        },
    };
    if (!WindhawkUtils::HookSymbols(shell32, shell32Hooks,
                                    ARRAYSIZE(shell32Hooks))) {
        Wh_Log(L"Failed to hook shell32!_UndoThread");
        return FALSE;
    }
    Wh_Log(L"Hooks: GetUndoRedoText=%d CContextMenuOnHMENU=%d _OnCommand=%d",
           GetUndoRedoText_Original != nullptr,
           ContextMenuOnHMENU_ctor_Original != nullptr,
           DefView_OnCommand_Original != nullptr);
    return TRUE;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
#endif  // UNDO_PREVIEW_STANDALONE
