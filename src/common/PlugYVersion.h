/*=================================================================
	PlugY version: the single source for the DLL, the launcher, the
	patcher, the updater and the installer. The release step rewrites the values
	(scripts/changelog-release.mjs, run by scripts/release.mjs).

	PLUGY_VERSION is semver (X.Y.Z, or X.Y.Z-alpha.N/-beta.N/-rc.N) and
	stays within 13 characters: the main menu prints it on its own line of
	a 200 px text box, under "PlugY4Kolbot" (MainScreen.cpp). Windows
	file versions cannot carry a pre-release, so PLUGY_VERSION_FILE is
	always X.Y.Z.0.
=================================================================*/
#ifndef PLUGY_VERSION_H
#define PLUGY_VERSION_H

#define PLUGY_VERSION_MAJOR 15
#define PLUGY_VERSION_MINOR 0
#define PLUGY_VERSION_PATCH 0
#define PLUGY_VERSION "15.0.0-rc.3"
#define PLUGY_VERSION_FILE "15.0.0.0"

#endif
