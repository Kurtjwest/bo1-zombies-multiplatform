#pragma once

void __cdecl Sys_SetupTLCallbacks(int hunkMemSize);
void __cdecl TL_Warning(const char *Text);
struct tlFileBuf;
// (the callbacks' own signatures: tl calls them through its function pointers, which WebAssembly checks)
bool __cdecl TL_ReadFile(const char *Name, tlFileBuf *File, unsigned int Align, unsigned int Flags);
void __cdecl TL_ReleaseFile(tlFileBuf *File);
void __cdecl TL_DebugPrint(const char *Text);
void *__cdecl TL_MemAlloc(unsigned int Size, unsigned int Align, unsigned int Flags);
void *__cdecl TL_MemRealloc(void *Ptr, unsigned int Size, unsigned int Align, unsigned int Flags);
void __cdecl TL_MemFree(void *Ptr);
void __cdecl TL_CriticalError(const char *msg);
