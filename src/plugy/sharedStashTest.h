#pragma once

struct Unit;

// Called on the client game thread. A release build has no command/file access.
void SharedStashTestClientFrame();
// Called only on the server game thread with its own character unit.
void SharedStashTestServerFrame(Unit* character);
