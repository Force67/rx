// Case shim for the mingw cross build: zetanet includes <WinSock2.h>, mingw
// ships lowercase <winsock2.h> on a case-sensitive fs.
#include <winsock2.h>
