#pragma once
#ifndef NERDMOD_PHOTO_POLICY_H
#define NERDMOD_PHOTO_POLICY_H

// Home photo precedence (Phase 2C.1). Pure logic, host tested.
//
//   nerdMod "Show Photo" OFF                      -> no photo
//   nerdMod "Show Photo" ON                       -> nerdMod photo is drawn
//        theme RenderPhoto=1                      -> into the theme's photo frame
//        theme RenderPhoto=0                      -> still drawn (runtime override); the theme's own
//                                                    graphics are kept, nothing is written to the theme
//   macro mode, or a theme with no photo area     -> no photo
namespace nmphoto {

struct Decision {
	bool show;		  // draw the nerdMod photo
	bool overridden;  // shown although the theme says RenderPhoto=0
	const char *result;
};

inline Decision decide(bool macroMode, bool showPhotoSetting, bool themeRenderPhoto, bool themeCanHostPhoto) {
	if (macroMode)
		return {false, false, "SKIPPED_MACRO_MODE"};
	if (!showPhotoSetting)
		return {false, false, "SKIPPED_SETTING_OFF"};
	if (themeRenderPhoto)
		return {true, false, "DISPLAYED"};
	if (themeCanHostPhoto)
		return {true, true, "DISPLAYED_BY_NERDMOD_OVERRIDE"};
	return {false, false, "SKIPPED_THEME_HAS_NO_PHOTO_AREA"};
}

} // namespace nmphoto

#endif
