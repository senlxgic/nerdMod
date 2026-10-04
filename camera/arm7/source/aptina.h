#ifndef NM_APTINA_H
#define NM_APTINA_H

#include <nds.h>

#ifdef __cplusplus
extern "C" {
#endif

// DSi camera I2C device addresses (inner = user facing, outer = rear).
#define NM_CAM_INNER 0x7A
#define NM_CAM_OUTER 0x78

// All of these return true on success. Every wait inside is bounded, so a
// dead/unresponsive sensor makes them fail instead of hanging the ARM7.
bool aptInit(u8 device);
bool aptActivate(u8 device);
bool aptDeactivate(u8 device);
bool aptSetMode(u8 device, u16 mode); // 1 = preview (A), 2 = capture (B)
u16 aptReadChipId(u8 device);

#ifdef __cplusplus
}
#endif

#endif
