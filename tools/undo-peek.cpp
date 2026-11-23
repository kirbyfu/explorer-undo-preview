// Standalone test tool for the Explorer Undo Preview decoder.
// Reads Explorer's undo/redo stack from outside Explorer and prints what each
// entry would do. Build with tools/build.ps1.
//
//   undo-peek            list the undo stack
//   undo-peek --redo     list the redo stack
//   undo-peek --raw      also hex-dump each serialized entry
//   undo-peek --dialog   show the confirmation dialog for the top entry

#define UNDO_PREVIEW_STANDALONE
#include "../explorer-undo-preview.wh.cpp"

#include <cstdio>
#include <io.h>
#include <fcntl.h>

static void HexDump(const std::vector<BYTE>& b) {
    for (size_t i = 0; i < b.size(); i += 16) {
        wprintf(L"      %06zx  ", i);
        for (size_t j = i; j < i + 16; j++)
            j < b.size() ? wprintf(L"%02x ", b[j]) : wprintf(L"   ");
        wprintf(L"\n");
    }
}

static void DumpMenu(HMENU m, int depth) {
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        WCHAR text[256] = L"";
        MENUITEMINFOW mii = {sizeof(mii)};
        mii.fMask = MIIM_ID | MIIM_STRING | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STATE;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text);
        GetMenuItemInfoW(m, i, TRUE, &mii);
        for (WCHAR* p = text; *p; p++)
            if (*p == L'\t') *p = L'|';
        wprintf(L"%*ls[%2d] id=0x%04x %ls%ls%ls\n", depth * 4, L"", i, mii.wID,
                (mii.fType & MFT_SEPARATOR) ? L"----" : text,
                (mii.fState & MFS_DISABLED) ? L"  (disabled)" : L"",
                mii.hSubMenu ? L"  >" : L"");
        if (mii.hSubMenu)
            DumpMenu(mii.hSubMenu, depth + 1);
    }
}

// Builds a folder's background context menu the way Explorer's view does
// (IShellFolder::CreateViewObject -> CDefFolderMenu) and prints it.
static int DumpBackgroundMenu(const wchar_t* folder) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    IShellFolder* sf = nullptr;
    IContextMenu* cm = nullptr;
    HRESULT hr = SHParseDisplayName(folder, nullptr, &pidl, 0, nullptr);
    if (SUCCEEDED(hr))
        hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&sf));
    if (SUCCEEDED(hr))
        hr = sf->CreateViewObject(nullptr, IID_PPV_ARGS(&cm));
    if (FAILED(hr)) {
        wprintf(L"failed: 0x%08X\n", (unsigned)hr);
        return 2;
    }
    HMENU m = CreatePopupMenu();
    hr = cm->QueryContextMenu(m, 0, 1, 0x7fff, CMF_NORMAL);
    wprintf(L"QueryContextMenu: 0x%08X\n", (unsigned)hr);
    DumpMenu(m, 0);
    DestroyMenu(m);
    cm->Release();
    sf->Release();
    CoTaskMemFree(pidl);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    bool redo = false, raw = false, dialog = false, items = false;
    for (int i = 1; i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--redo")) redo = true;
        else if (!lstrcmpiW(argv[i], L"--raw")) raw = true;
        else if (!lstrcmpiW(argv[i], L"--dialog")) dialog = true;
        else if (!lstrcmpiW(argv[i], L"--items")) items = true;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    for (int i = 1; i + 1 < argc; i++)
        if (!lstrcmpiW(argv[i], L"--menu"))
            return DumpBackgroundMenu(argv[i + 1]);

    if (dialog) {
        LoadLibraryW(L"comctl32.dll");  // v6 via manifest -> TaskDialogIndirect
        undopreview::DialogOptions opt;
        std::wstring log;
        bool ok = undopreview::ConfirmTop(redo, nullptr, opt, &log);
        wprintf(L"%ls\n=> %ls\n", log.c_str(), ok ? L"proceed" : L"cancelled");
        return ok ? 0 : 1;
    }

    std::vector<undopreview::Entry> entries;
    HRESULT hr = undopreview::ReadStack(redo, 50, entries);
    if (FAILED(hr)) {
        wprintf(L"Could not open the undo manager: 0x%08X\n", (unsigned)hr);
        return 2;
    }
    wprintf(L"%ls stack: %zu entr%ls (in enumeration order)\n\n",
            redo ? L"Redo" : L"Undo", entries.size(),
            entries.size() == 1 ? L"y" : L"ies");
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        auto p = undopreview::Describe(e, redo, 12);
        wprintf(L"[%zu] %ls  (%zu bytes, %ls)\n    menu: %ls %ls\n%ls\n", i,
                p.headline.c_str(), e.raw.size(),
                e.decoded ? L"decoded" : L"PARTIAL", redo ? L"Redo" : L"Undo",
                undopreview::ShortSummary(e, redo).c_str(), p.details.c_str());
        if (items)
            for (const auto& it : e.items)
                wprintf(L"      op=%lu flags=0x%lx children=%lu src='%ls' dst='%ls' srcParent='%ls' dstParent='%ls'\n",
                        it.op, it.flags, it.childCount, it.srcName.c_str(),
                        it.dstName.c_str(), it.srcParent.c_str(),
                        it.dstParent.c_str());
        if (raw)
            HexDump(e.raw);
        wprintf(L"\n");
    }
    CoUninitialize();
    return 0;
}
