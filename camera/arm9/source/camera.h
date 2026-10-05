#ifndef NM_CAMERA_H
#define NM_CAMERA_H

#include <nds/ndstypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CAM_NONE = 0, CAM_INNER, CAM_OUTER } Camera;

typedef enum {
	CAPTURE_MODE_PREVIEW = 1, // 256x192, converted to RGB555 by the camera interface
	CAPTURE_MODE_CAPTURE = 2  // 640x480, raw YUV422
} CaptureMode;

#define CAM_CAPTURE_WIDTH 640
#define CAM_CAPTURE_HEIGHT 480
#define CAM_CAPTURE_BYTES (CAM_CAPTURE_WIDTH * CAM_CAPTURE_HEIGHT * 2)

// Why cameraInit() failed (valid after it returned false).
typedef enum {
	CAM_ERR_NONE = 0,
	CAM_ERR_NO_ACCESS, // not in DSi mode / SCFG_EXT does not expose NDMA + camera registers
	CAM_ERR_NO_SENSOR  // the ARM7 could not bring up any sensor (I2C timeout, wrong chip id)
} CameraError;

// Sends one command word to the camera ARM7 service and waits (vblank units) for its reply word. Used by the
// microphone capture (see camera_protocol.h). Returns true when the reply has the success bit.
bool cameraRawCommand(u32 cmd, u32 timeoutFrames, u32 *reply);

// True if SCFG_EXT currently gives the ARM9 access to SCFG, NDMA and the camera
// interface (bits 31, 16 and 17). Touches no camera hardware.
bool cameraHardwareAccessible(void);

// Powers up and initialises the camera sensors. Returns true if at least one
// sensor is usable (see cameraAvailable); otherwise false and cameraLastError().
// Nothing is touched unless cameraHardwareAccessible() is true.
bool cameraInit(void);
CameraError cameraLastError(void);

// Whether a given sensor passed init (valid after a successful cameraInit()).
bool cameraAvailable(Camera cam);

// Activates a camera; the previously active one is deactivated first.
bool cameraActivate(Camera cam);

// Deactivates the active camera (turns its LED off) if there is one.
void cameraDeactivateActive(void);

Camera cameraActive(void);

// Starts a transfer from the camera into dst using NDMA 1.
// dst must be 4-byte aligned: VRAM for preview (256*192*2 bytes) or a buffer of
// CAM_CAPTURE_BYTES for capture. A RAM buffer should be 32-byte aligned (the
// data cache is flushed over it first, and an unaligned edge would also write
// back / drop neighbouring data); main.cpp allocates it with memalign(32, ...).
bool cameraTransferStart(u16 *dst, CaptureMode mode);
// Stops the camera interface AND the NDMA channel (safe to call at any time).
void cameraTransferStop(void);
bool cameraTransferActive(void);

// Stops everything, switches the sensors off and puts the camera clocks back to
// the state they had before cameraInit().
void cameraShutdown(void);

#ifdef __cplusplus
}
#endif

#endif
