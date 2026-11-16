/* Win32 sync semantics under Wine on device. Built for x86_64 and i686 so the
 * same source covers ARM64EC and WoW64. Emits JSONL; exit code is 1 on any
 * failure. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(const char *name, int ok)
{
    checks++;
    if (!ok) failures++;
    printf("{\"test\":\"win_sync\",\"name\":\"%s\",\"ok\":%s}\n", name, ok ? "true" : "false");
}

static unsigned long long now_ns(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (unsigned long long)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

static int manual_event(void)
{
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD a, b;
    int ok;
    if (!ev) return 0;
    SetEvent(ev);
    a = WaitForSingleObject(ev, 1000);
    b = WaitForSingleObject(ev, 1000);
    ok = a == WAIT_OBJECT_0 && b == WAIT_OBJECT_0;
    ResetEvent(ev);
    ok = ok && WaitForSingleObject(ev, 0) == WAIT_TIMEOUT;
    CloseHandle(ev);
    return ok;
}

static int auto_reset_event(void)
{
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    DWORD a, b;
    if (!ev) return 0;
    SetEvent(ev);
    a = WaitForSingleObject(ev, 1000);
    b = WaitForSingleObject(ev, 0);
    CloseHandle(ev);
    return a == WAIT_OBJECT_0 && b == WAIT_TIMEOUT;
}

static int wait_any_lowest_index(void)
{
    HANDLE ev[2];
    DWORD r;
    ev[0] = CreateEventA(NULL, TRUE, FALSE, NULL);
    ev[1] = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ev[0] || !ev[1]) return 0;
    SetEvent(ev[1]);
    SetEvent(ev[0]);
    r = WaitForMultipleObjects(2, ev, FALSE, 1000);
    CloseHandle(ev[0]);
    CloseHandle(ev[1]);
    return r == WAIT_OBJECT_0;
}

/* WaitAll must not consume the signalled event while the semaphore is held,
 * then acquire both in one pass once the count is released. */
struct all_arg { HANDLE sem; HANDLE ev; DWORD result; };

static DWORD WINAPI all_thread(void *p)
{
    struct all_arg *a = p;
    Sleep(100);
    ReleaseSemaphore(a->sem, 1, NULL);
    return 0;
}

static int wait_all_atomic(void)
{
    struct all_arg a;
    HANDLE both[2], th;
    int ok;

    a.sem = CreateSemaphoreA(NULL, 1, 2, NULL);
    a.ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!a.sem || !a.ev) return 0;
    WaitForSingleObject(a.sem, 1000);
    SetEvent(a.ev);

    both[0] = a.sem;
    both[1] = a.ev;
    a.result = 0xffffffff;
    th = CreateThread(NULL, 0, all_thread, &a, 0, NULL);
    a.result = WaitForMultipleObjects(2, both, TRUE, 5000);
    WaitForSingleObject(th, 2000);
    CloseHandle(th);

    ok = a.result == WAIT_OBJECT_0;
    ok = ok && WaitForSingleObject(a.ev, 0) == WAIT_OBJECT_0;
    CloseHandle(a.sem);
    CloseHandle(a.ev);
    return ok;
}

/* Two entries of the same semaphore demand two counts. */
static int semaphore_demand(void)
{
    HANDLE sem = CreateSemaphoreA(NULL, 2, 4, NULL);
    HANDLE both[2];
    LONG prev = -1;
    DWORD r;
    int ok;
    if (!sem) return 0;
    both[0] = sem;
    both[1] = sem;
    r = WaitForMultipleObjects(2, both, TRUE, 0);
    ok = r == WAIT_OBJECT_0;
    ok = ok && ReleaseSemaphore(sem, 2, &prev) && prev == 0;
    CloseHandle(sem);
    return ok;
}

static int mutex_basic(void)
{
    HANDLE m = CreateMutexA(NULL, FALSE, NULL);
    DWORD a, b;
    int ok;
    if (!m) return 0;
    a = WaitForSingleObject(m, 1000);
    b = WaitForSingleObject(m, 0);
    /* A mutex is recursive: the owning thread re-acquires it and the recursion
     * count rises, so both waits succeed and both need a release. */
    ok = a == WAIT_OBJECT_0 && b == WAIT_OBJECT_0;
    ReleaseMutex(m);
    ReleaseMutex(m);
    CloseHandle(m);
    return ok;
}

static int abandoned_mutex(void)
{
    char name[64];
    wchar_t wname[64], cmd[1024], self[MAX_PATH];
    HANDLE m;
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;
    DWORD r;

    sprintf(name, "titan_m2_abandoned_%lu", (unsigned long)GetCurrentProcessId());
    MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 64);
    GetModuleFileNameW(NULL, self, MAX_PATH);

    m = CreateMutexA(NULL, FALSE, name);
    if (!m) return 0;

    wsprintfW(cmd, L"\"%s\" --child \"%s\"", self, wname);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(m);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    r = WaitForSingleObject(m, 2000);
    if (r == WAIT_ABANDONED) ReleaseMutex(m);
    CloseHandle(m);
    return r == WAIT_ABANDONED;
}

struct ping { HANDLE a, b; volatile LONG stop; };

static DWORD WINAPI ping_thread(void *p)
{
    struct ping *s = p;
    while (!s->stop) {
        if (WaitForSingleObject(s->a, 5000) != WAIT_OBJECT_0) break;
        SetEvent(s->b);
    }
    return 0;
}

static int thread_pingpong(unsigned long rounds)
{
    struct ping s;
    HANDLE th[4];
    unsigned long i;
    int ok = 1;

    s.a = CreateEventA(NULL, FALSE, FALSE, NULL);
    s.b = CreateEventA(NULL, FALSE, FALSE, NULL);
    s.stop = 0;
    if (!s.a || !s.b) return 0;
    for (i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, ping_thread, &s, 0, NULL);
    for (i = 0; i < rounds; i++) {
        SetEvent(s.a);
        if (WaitForSingleObject(s.b, 5000) != WAIT_OBJECT_0) { ok = 0; break; }
    }
    s.stop = 1;
    for (i = 0; i < 4; i++) SetEvent(s.a);
    WaitForMultipleObjects(4, th, TRUE, 5000);
    for (i = 0; i < 4; i++) CloseHandle(th[i]);
    CloseHandle(s.a);
    CloseHandle(s.b);
    return ok;
}

static int max_objects(void)
{
    HANDLE ev[MAXIMUM_WAIT_OBJECTS];
    DWORD r;
    int i, ok;
    for (i = 0; i < MAXIMUM_WAIT_OBJECTS; i++)
        ev[i] = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetEvent(ev[MAXIMUM_WAIT_OBJECTS - 1]);
    r = WaitForMultipleObjects(MAXIMUM_WAIT_OBJECTS, ev, FALSE, 2000);
    ok = r == WAIT_OBJECT_0 + MAXIMUM_WAIT_OBJECTS - 1;
    for (i = 0; i < MAXIMUM_WAIT_OBJECTS; i++) CloseHandle(ev[i]);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned long long t0, t1, i;
    HANDLE ev;

    if (argc == 3 && !strcmp(argv[1], "--child")) {
        HANDLE m = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, argv[2]);
        if (!m) return 2;
        return WaitForSingleObject(m, 2000) == WAIT_OBJECT_0 ? 0 : 3;
    }

    check("manual_event", manual_event());
    check("auto_reset_event", auto_reset_event());
    check("wait_any_lowest_index", wait_any_lowest_index());
    check("wait_all_atomic", wait_all_atomic());
    check("semaphore_demand", semaphore_demand());
    check("mutex_basic", mutex_basic());
    check("abandoned_mutex", abandoned_mutex());
    check("max_objects", max_objects());
    check("thread_pingpong_20k", thread_pingpong(20000));

    ev = CreateEventA(NULL, TRUE, TRUE, NULL);
    t0 = now_ns();
    for (i = 0; i < 200000; i++) WaitForSingleObject(ev, 0);
    t1 = now_ns();
    CloseHandle(ev);
    printf("{\"test\":\"win_sync\",\"name\":\"signalled_wait_ns\",\"value\":%llu,\"rounds\":200000}\n",
           (t1 - t0) / 200000);
    printf("{\"test\":\"win_sync_summary\",\"checks\":%d,\"failures\":%d,\"bits\":%u}\n",
           checks, failures, (unsigned)(sizeof(void *) * 8));
    return failures ? 1 : 0;
}
