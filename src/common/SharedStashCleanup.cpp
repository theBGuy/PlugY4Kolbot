#include "SharedStashCleanup.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <utility>

namespace SharedStash
{
namespace
{
const DWORD RetryInterval = 1000;

struct RetiredSession
{
    std::string token;
    Store store;
    RetiredSession* next;

    RetiredSession(Store& source, const std::string& session)
        : token(session), store(std::move(source)), next(NULL) {}
};

struct CleanupQueue
{
    CRITICAL_SECTION lock;
    RetiredSession* first;
    DWORD lastAttempt;

    CleanupQueue() : first(NULL), lastAttempt(0) { InitializeCriticalSection(&lock); }
    ~CleanupQueue() { DeleteCriticalSection(&lock); }
} queue;

// Only the menu thread reads or changes this timer.
UINT_PTR cleanupTimer = 0;

bool finished(Status status) { return status == OK || status == COMMITTED; }

void CALLBACK cleanupTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    retryRetiredSessions();
}
}

bool retireSession(Store& store, const std::string& token)
{
    try
    {
        Status status = store.abandonSession(token);
        if (finished(status)) return true;
        if (status != BUSY && status != STORE_ERROR) return false;

        // Copy the token before moving the connection. An allocation failure leaves the caller's Store intact.
        RetiredSession* retired = new RetiredSession(store, token);
        EnterCriticalSection(&queue.lock);
        retired->next = queue.first;
        queue.first = retired;
        LeaveCriticalSection(&queue.lock);
        return true;
    }
    catch (...) { return false; }
}

void retryRetiredSessions()
{
    if (!TryEnterCriticalSection(&queue.lock)) return;
    try
    {
        DWORD now = GetTickCount();
        if (queue.first && now - queue.lastAttempt >= RetryInterval)
        {
            queue.lastAttempt = now;
            RetiredSession** link = &queue.first;
            while (*link)
            {
                RetiredSession* retired = *link;
                if (finished(retired->store.abandonSession(retired->token)))
                {
                    *link = retired->next;
                    delete retired;
                }
                else link = &retired->next;
            }
        }
    }
    catch (...) {}
    LeaveCriticalSection(&queue.lock);
}

bool startCleanupTimer()
{
    if (cleanupTimer) return true;
    cleanupTimer = SetTimer(NULL, 0, RetryInterval, cleanupTimerProc);
    return cleanupTimer != 0;
}

void stopCleanupTimer()
{
    if (cleanupTimer)
    {
        KillTimer(NULL, cleanupTimer);
        cleanupTimer = 0;
    }
    EnterCriticalSection(&queue.lock);
    while (queue.first)
    {
        RetiredSession* retired = queue.first;
        queue.first = retired->next;
        delete retired;
    }
    LeaveCriticalSection(&queue.lock);
}
}
