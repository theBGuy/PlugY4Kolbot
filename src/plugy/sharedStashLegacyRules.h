#pragma once

#include <windows.h>
#include <string.h>

// Missing means not migrated. Existing, inaccessible, or invalid paths must not trigger .sss fallback.
inline bool sharedStashLegacyPathAvailable(const char* databasePath)
{
	if (!databasePath || !databasePath[0])
		return false;
	// Win32 can report malformed drive paths as merely missing. They are not evidence of an absent database.
	if (strpbrk(databasePath, "*?<>|\"") != NULL)
		return false;
	if (GetFileAttributesA(databasePath) != INVALID_FILE_ATTRIBUTES)
		return false;
	DWORD error = GetLastError();
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
