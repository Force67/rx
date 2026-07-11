// Case shim for the mingw cross build: MSVC-targeted sources include
// <Windows.h>, mingw ships lowercase <windows.h> on a case-sensitive fs.
#include <windows.h>
