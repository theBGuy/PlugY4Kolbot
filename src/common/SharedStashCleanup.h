#pragma once

#include "SharedStashStore.h"

namespace SharedStash
{
    // Native units must be destroyed first. True means cleanup ownership was transferred or finished.
    bool retireSession(Store& store, const std::string& token);
    void retryRetiredSessions();

    // The menu thread owns the timer and its message loop.
    bool startCleanupTimer();
    void stopCleanupTimer();
}
