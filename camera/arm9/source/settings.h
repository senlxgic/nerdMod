/*
	Camera Settings (Phase 2D): a touch menu reached from the gear button (or START), with
	Video FPS, Recording Info (its last page is Camera Info), SD Speed Test, Audio Test and Video Quality. Every row is a touch button; the D-pad + A
	and B also work. The caller must have stopped the preview transfer first.
*/
#pragma once

namespace camset {

enum class Exit { Back, Power };

// The Camera Settings menu.
Exit run();
// VIDEO FRAME RATE chooser (10 Safe / 15 Smooth / 20 High / 30 Max), persisted. changed may be null.
Exit chooseFps(bool *changed);
// VIDEO QUALITY chooser (HIGH / BALANCED / SMALL), persisted.
Exit chooseQuality();
// Recording Info pages (A / D-pad turn pages, B closes).
Exit recordingInfo(int page = 0);
// The short summary shown right after a recording: Requested / Actual / Dropped / SD MB/s / Mic. A opens the details.
Exit recordingSummary();

} // namespace camset
