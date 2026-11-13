#include "../src/plugy/sharedStashCubeSwapRules.h"
#include <stdio.h>

using namespace SharedStash;
static unsigned checks = 0, failures = 0;
#define CHECK(value) do { ++checks; if (!(value)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #value); } } while (0)

int main()
{
	CHECK(cubePageTransfer(0, 0, 0, false) == CUBE_PAGE_NO_CONTENTS);
	CHECK(cubePageTransfer(0, 0, 0, true) == CUBE_PAGE_NO_CONTENTS); // pinned roots do not move
	CHECK(cubePageTransfer(1, 0, 0, false) == CUBE_PAGE_NO_CONTENTS);
	CHECK(cubePageTransfer(1, 1, 0, false) == CUBE_PAGE_TO_PERSONAL);
	CHECK(cubePageTransfer(0, 0, 1, true) == CUBE_PAGE_TO_SHARED);
	CHECK(cubePageTransfer(1, 0, 1, true) == CUBE_PAGE_TO_SHARED);
	CHECK(cubePageTransfer(1, 1, 1, false) == CUBE_PAGE_TO_PERSONAL);
	CHECK(cubePageTransfer(1, 1, 1, true) == CUBE_PAGE_NO_ROOM);
	CHECK(cubePageTransfer(1, 0, 0, true) == CUBE_PAGE_NO_ROOM);
	CHECK(cubePageTransfer(2, 1, 0, false) == CUBE_PAGE_NO_ROOM);
	CHECK(cubePageTransfer(2, 2, 0, false) == CUBE_PAGE_NO_ROOM);
	CHECK(cubePageTransfer(0, 0, 2, true) == CUBE_PAGE_NO_ROOM);
	CHECK(cubePageTransfer(2, 0, 1, true) == CUBE_PAGE_TO_SHARED);
	CHECK(cubePageTransfer(1, 1, 2, false) == CUBE_PAGE_TO_PERSONAL);
	CHECK(cubePageTransfer(2, 0, 2, false) == CUBE_PAGE_NO_CONTENTS);
	printf("Shared stash cube page swaps: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
