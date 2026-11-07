# Sourced by each game's build.sh after it sets CC and GAME. Virus scanners distrust DLLs without version info, so
# every DLL gets it, filled from the tag in VERSION; a build without a tag gets 0.0.0.0.
# WinLibs names its windres without the target prefix.
WINDRES=$(command -v "${CC%gcc}windres" || echo "$(dirname "$(command -v "$CC")")/windres")
VERSION_TEXT=${VERSION:-local build}
VERSION_TEXT=${VERSION_TEXT#v}
VERSION_NUMBERS=$(echo "$VERSION" | sed -nE 's/^v([0-9]+)\.([0-9]+)\.([0-9]+)(-pre-release)?$/\1,\2,\3,0/p')
VERSION_NUMBERS=${VERSION_NUMBERS:-0,0,0,0}
case $VERSION in *-pre-release) VERSION_FLAGS=VS_FF_PRERELEASE ;; *) VERSION_FLAGS=0 ;; esac

# versioninfo <dll> <description> writes <dll without .dll>.version.o, to link into it.
versioninfo() {
    name=$(basename "$1" .dll)
    "$WINDRES" -J rc -O coff -o "${1%.dll}.version.o" <<EOF
#include <winver.h>
VS_VERSION_INFO VERSIONINFO
FILEVERSION $VERSION_NUMBERS
PRODUCTVERSION $VERSION_NUMBERS
FILEFLAGSMASK VS_FFI_FILEFLAGSMASK
FILEFLAGS $VERSION_FLAGS
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_DLL
FILESUBTYPE VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "DudeCmonMan"
            VALUE "FileDescription", "$2"
            VALUE "FileVersion", "$VERSION_TEXT"
            VALUE "InternalName", "$name"
            VALUE "LegalCopyright", "Copyright (C) DudeCmonMan. GNU GPL v3.0"
            VALUE "OriginalFilename", "$name.dll"
            VALUE "ProductName", "s2fix"
            VALUE "ProductVersion", "$VERSION_TEXT"
            VALUE "Comments", "https://github.com/DudeCmonMan/s2fix"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
EOF
}
