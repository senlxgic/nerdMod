// Services the camera application (main.cpp) offers to the album and the player.
#pragma once

#include <nds.h>

#include "camera.h"

bool appPowerExitRequested();		// power button / START+SELECT+L+R
void appLidSleep(Camera resume);	// blocks while the lid is closed
void appWaitFrames(int frames);
