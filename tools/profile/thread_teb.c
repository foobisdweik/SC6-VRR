/* Read a Windows thread record. This does not suspend or change the thread. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  NTSTATUS status;
  PVOID teb;
  CLIENT_ID id;
  ULONG_PTR affinity;
  LONG priority;
  LONG base_priority;
} basic_thread_info;

typedef NTSTATUS (WINAPI *query_thread_fn)(HANDLE, ULONG, PVOID, ULONG, PULONG);

int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  DWORD tid = strtoul(argv[1], NULL, 0);
  HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
  union { FARPROC raw; query_thread_fn typed; } entry;
  entry.raw = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
  query_thread_fn query = entry.typed;
  basic_thread_info info = {0};
  if (!thread || !query || query(thread, 0, &info, sizeof(info), NULL))
    return 1;
  HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, (DWORD)(ULONG_PTR)info.id.UniqueProcess);
  unsigned char data[0x2000];
  SIZE_T read = 0;
  if (!process || !ReadProcessMemory(process, info.teb, data, sizeof(data), &read))
    return 1;
  FILE *out = fopen(argv[2], "wb");
  if (!out)
    return 1;
  fwrite(data, 1, read, out);
  fclose(out);
  printf("{\"win_tid\":%lu,\"win_pid\":%lu,\"teb\":\"%p\",\"bytes\":%llu}\n",
         (unsigned long)tid, (unsigned long)(ULONG_PTR)info.id.UniqueProcess, info.teb, (unsigned long long)read);
  CloseHandle(process);
  CloseHandle(thread);
  return 0;
}
