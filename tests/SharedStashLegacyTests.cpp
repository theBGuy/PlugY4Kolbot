#include "../src/plugy/sharedStashLegacyRules.h"
#include <stdio.h>

static int failed = 0;
#define CHECK(value) do { if (!(value)) { printf("FAIL line %d: %s\n", __LINE__, #value); ++failed; } } while (0)

int main()
{
	char folder[MAX_PATH], path[MAX_PATH];
	DWORD size = GetTempPathA(MAX_PATH, folder);
	if (!size || size >= MAX_PATH || !GetTempFileNameA(folder, "pys", 0, path))
		return 1;
	// An empty database left by an interrupted import still prevents stale-file fallback.
	CHECK(!sharedStashLegacyPathAvailable(path));
	CHECK(SetFileAttributesA(path, FILE_ATTRIBUTE_READONLY) != 0);
	CHECK(!sharedStashLegacyPathAvailable(path));
	CHECK(SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL) != 0);
	HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	CHECK(file != INVALID_HANDLE_VALUE);
	CHECK(!sharedStashLegacyPathAvailable(path));
	if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
	CHECK(DeleteFileA(path) != 0);
	CHECK(sharedStashLegacyPathAvailable(path));
	CHECK(CreateDirectoryA(path, NULL) != 0);
	CHECK(!sharedStashLegacyPathAvailable(path));
	CHECK(RemoveDirectoryA(path) != 0);
	CHECK(!sharedStashLegacyPathAvailable(NULL));
	CHECK(!sharedStashLegacyPathAvailable(""));
	CHECK(!sharedStashLegacyPathAvailable("?:\\invalid.sqlite3"));
	printf("Shared stash legacy guards: %s\n", failed ? "FAILED" : "passed");
	return failed ? 1 : 0;
}
