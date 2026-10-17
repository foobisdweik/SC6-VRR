/* Read CPU counters for a Windows thread. This does not suspend the thread. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static double value(FILETIME time) {
  ULARGE_INTEGER integer;
  integer.LowPart = time.dwLowDateTime;
  integer.HighPart = time.dwHighDateTime;
  return integer.QuadPart / 10000000.0;
}

static int read_times(HANDLE thread, double *stamp, double *cpu) {
  FILETIME created, exited, kernel, user, now;
  GetSystemTimeAsFileTime(&now);
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user))
    return 0;
  *stamp = value(now) - 11644473600.0;
  *cpu = value(kernel) + value(user);
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  DWORD tid = strtoul(argv[1], NULL, 0);
  HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
  double start, end, before, after;
  if (!thread || !read_times(thread, &start, &before)) {
    fprintf(stderr, "Cannot read Windows thread %lu: %lu\n", (unsigned long)tid, (unsigned long)GetLastError());
    return 1;
  }
  Sleep(5000);
  if (!read_times(thread, &end, &after))
    return 1;
  CloseHandle(thread);
  printf("{\"win_tid\":%lu,\"start_unix\":%.6f,\"end_unix\":%.6f,"
         "\"cpu_before_s\":%.9f,\"cpu_after_s\":%.9f,\"cpu_percent\":%.6f}\n",
         (unsigned long)tid, start, end, before, after, 100 * (after - before) / (end - start));
  return 0;
}
