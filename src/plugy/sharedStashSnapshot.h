#pragma once

#include "common.h"
#include "../common/SharedStashStore.h"

// Sources are parallel to the returned records, so a session can retain stable
// database identities across native actions without putting pointers on disk.
bool buildSharedStashLiveSnapshot(Unit* ptChar, SharedStash::Snapshot& out,
    std::vector<DWORD>& itemGids, std::vector<Stash*>& pages, DWORD activeCubeGid = 0);
