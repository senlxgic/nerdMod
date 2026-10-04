// nerdMod Camera settings that persist between runs (sd:/_nds/nerdMod/camera.ini).
#pragma once

namespace camsettings {

int videoFps();			// 10, 15, 20 or 30 (loaded on first use)
void setVideoFps(int fps);	// validates and saves

} // namespace camsettings
