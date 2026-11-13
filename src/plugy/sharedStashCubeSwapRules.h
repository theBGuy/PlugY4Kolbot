#pragma once

namespace SharedStash {
enum CubePageTransfer {
	CUBE_PAGE_NO_CONTENTS,
	CUBE_PAGE_TO_PERSONAL,
	CUBE_PAGE_TO_SHARED,
	CUBE_PAGE_NO_ROOM
};

inline CubePageTransfer cubePageTransfer(unsigned sharedCubes, unsigned fullSharedCubes,
	unsigned personalCubes, bool privateContents)
{
	if (!sharedCubes && !personalCubes) return CUBE_PAGE_NO_CONTENTS;
	// The character has one native cube grid, not a grid for each private root.
	if ((sharedCubes > 1 && fullSharedCubes) || (personalCubes > 1 && privateContents)
		|| (fullSharedCubes && privateContents)) return CUBE_PAGE_NO_ROOM;
	if (privateContents && personalCubes == 1) return CUBE_PAGE_TO_SHARED;
	if (privateContents && sharedCubes) return CUBE_PAGE_NO_ROOM;
	if (fullSharedCubes) return CUBE_PAGE_TO_PERSONAL;
	return CUBE_PAGE_NO_CONTENTS;
}
}
