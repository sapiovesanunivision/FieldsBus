# FieldsBus in Dev.3

FieldsBus is built inside the Dev.3 tree (`D:\Univision\Dev.3`, `main3.sln`, VS 2026 v145 / VS 2022 v143) as an
ordinary UvX module, the same way `libmodbus` is. **This GitHub repo is the master copy.** Dev.3 holds a mirror of
the library sources plus Dev.3-only project files.

## The rule

**Never edit the mirrored files in Dev.3.** Fix them here, commit, push, and run the sync. The sync script stops
when a mirrored file was changed in Dev.3 since the last sync.

## What lives where

| Dev.3 path | Content | Owner |
|---|---|---|
| `Dev\Sdk\UvX\include\softeip\`, `softmb\`, `softfb\` | FieldsBus headers | **mirror** (sync script) |
| `Dev\Sdk\UvX\sources\SoftFieldbus\src\*.cpp` | FieldsBus sources | **mirror** (sync script) |
| `Dev\Sdk\UvX\sources\SoftFieldbus\FIELDSBUS_SYNC.txt` | commit + file hashes of the last sync | sync script |
| `Dev\Sdk\UvX\sources\SoftFieldbus\SoftFieldbus1x.vcxproj`, `.rc` | builds `SoftFieldbus145_x64(d).dll` | Dev.3 |
| `Dev\Sdk\UvX\include\SoftFieldbus\SoftFieldbus.h` | wrapper: defines `SOFTFIELDBUS_SHARED`, includes the API, auto-links the `.lib` | Dev.3 |
| `Dev\Sdk\Uvc\...\UvcIOSoftModBus\` | IODevice plugin replacing `UvcIOModBus` | Dev.3 |
| `...\UvpProcessImage\ProInspectProcessImageSoftFieldbusService\` | process-image service replacing the Hilscher one | Dev.3 |

Dev.3 code includes only `<SoftFieldbus/SoftFieldbus.h>`, never `softeip/socket_compat.hpp` or
`softeip/periodic_timer.hpp`: those pull in `winsock2.h` / `windows.h` and break MFC translation units that include
`afxwin.h` first.

## Sync

```bat
cd /d D:\Univision\Misc\FieldsBus
git pull & rem commit and push your FieldsBus changes first: the script copies the committed HEAD
pwsh tools\sync-to-dev3.ps1 -Dev3Root D:\Univision\Dev.3 -WhatIf   & rem preview
pwsh tools\sync-to-dev3.ps1 -Dev3Root D:\Univision\Dev.3
```

- Copies `include\softeip|softmb|softfb` and `src\*.cpp` from **HEAD** (not the working tree).
- Writes only changed files (CRLF); removes mirror files deleted upstream; never touches Dev.3-only files.
- Prints the Dev.3 commit message (`Sync FieldsBus <hash>`) and warns when a `.cpp` was added or removed: then add
  it to `SoftFieldbus1x.vcxproj` and `.filters` by hand.
- `-AllowDirty` syncs although `include/` or `src/` has uncommitted changes (they are not copied). `-Force`
  overwrites mirror files edited in Dev.3.
- Works with Windows PowerShell 5.1 and PowerShell 7.

Dev.3 is an Azure DevOps Git repo; its pre-push hook wants a branch named `dev/<story>/<task>` or `bug/<n>`.

## Building the same way here

Dev.3 builds the DLL with Level 3 + warnings as errors, and code analysis in Debug. Before syncing, check here:

```bat
cmake --preset vs2026-analyze
cmake --build --preset vs2026-analyze-debug
```

This builds `SoftFieldbus.dll` (CMake option `SOFTFIELDBUS_SHARED`) with `/analyze /WX`, and links `mb_client_test`
against the DLL, so a missing export fails the link.

## Export macros

`include/softeip/export.hpp` defines `SOFTEIP_API`, `SOFTMB_API` and `SOFTFB_API`.

| Define | Effect |
|---|---|
| none | static libraries (CMake default) |
| `SOFTFIELDBUS_SHARED` | consumer of the single DLL (Dev.3: set by `SoftFieldbus.h`) |
| `SOFTFIELDBUS_SHARED` + `SOFTFIELDBUS_BUILDING_DLL` | building the single DLL |
| `SOFTEIP_SHARED` (+ `SOFTEIP_BUILDING_DLL`) | the older `softeip.dll`, EtherNet/IP only |

Exported classes are pimpl, and `C4251` is disabled around them, so consumers build cleanly at Level 3 + /WX. The
DLL and its consumers must use the same toolset and CRT: Dev.3 names them `...143...` / `...145...` and `...d` for
Debug, so they never mix.
