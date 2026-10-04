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

// Powers up and initialises both camera sensors. Returns false if the camera
// hardware did not answer (not a DSi, SCFG locked, sensor dead...).
bool cameraInit(void);

// Activates a camera; the previously active one is deactivated first.
bool cameraActivate(Camera cam);

// Deactivates the active camera (turns its LED off) if there is one.
void cameraDeactivateActive(void);

Camera cameraActive(void);

// Starts a transfer from the camera into dst using NDMA 1.
// dst must be 4-byte aligned: VRAM for preview (256*192*2 bytes) or a buffer of
// CAM_CAPTURE_BYTES for capture.
bool cameraTransferStart(u16 *dst, CaptureMode mode);
void cameraTransferStop(void);
bool cameraTransferActive(void);

// Stops everything and switches the camera clocks off again.
void cameraShutdown(void);

#ifdef __cplusplus
}
#endif

#endif
