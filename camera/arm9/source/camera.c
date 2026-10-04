/*
	nerdMod Camera - DSi camera interface driver, ARM9 side.

	Register usage and the transfer sequence follow "dsi-camera" by Epicpkmn11
	(Pk11), released into the public domain (Unlicense), which in turn follows
	nocash's GBATEK ("DSi Cameras"). The sensors themselves are driven by the
	ARM7 (see arm7/source/aptina.c); this file talks to it over FIFO.

	nerdMod changes: the PXI transport is replaced by libnds FIFO value32
	messages, every ARM7 command has a timeout, and RAM buffers are cache
	invalidated before the camera DMA writes into them.
*/

#include "camera.h"
#include "camera_protocol.h"

#include <nds.h>

#define NM_REG_CAM_MCNT (*(vu16 *)0x04004200)
#define NM_REG_CAM_CNT (*(vu16 *)0x04004202)
#define NM_REG_CAM_DAT (*(vu16 *)0x04004204)
#define NM_REG_NDMA1SAD (*(vu32 *)0x04004120)
#define NM_REG_NDMA1DAD (*(vu32 *)0x04004124)
#define NM_REG_NDMA1TCNT (*(vu32 *)0x04004128)
#define NM_REG_NDMA1WCNT (*(vu32 *)0x0400412C)
#define NM_REG_NDMA1BCNT (*(vu32 *)0x04004130)
#define NM_REG_NDMA1CNT (*(vu32 *)0x04004138)

// Frames to wait for an ARM7 reply before giving up.
#define CAM_INIT_TIMEOUT 900
#define CAM_CMD_TIMEOUT 300

static Camera activeCamera = CAM_NONE;
static CaptureMode activeMode = CAPTURE_MODE_PREVIEW;
static bool cameraClocksOn = false;

static bool armCommand(u32 cmd, u32 timeoutFrames, u32 *reply) {
	// Drop anything stale (e.g. a reply that arrived after an earlier timeout)
	while (fifoCheckValue32(NMCAM_FIFO_CHANNEL))
		fifoGetValue32(NMCAM_FIFO_CHANNEL);

	fifoSendValue32(NMCAM_FIFO_CHANNEL, cmd);

	for (u32 i = 0; i < timeoutFrames; i++) {
		if (fifoCheckValue32(NMCAM_FIFO_CHANNEL)) {
			const u32 r = fifoGetValue32(NMCAM_FIFO_CHANNEL);
			if (reply)
				*reply = r;
			return (r & NMCAM_REPLY_OK) != 0;
		}
		swiWaitForVBlank();
	}
	return false;
}

bool cameraInit(void) {
	if (REG_SCFG_EXT == 0)
		return false; // SCFG locked / not DSi hardware: camera registers are not reachable

	REG_SCFG_CLK |= BIT(2); // CamInterfaceClock = ON
	cameraClocksOn = true;
	NM_REG_CAM_MCNT = 0;    // Camera Module Control
	swiDelay(0x1E);
	REG_SCFG_CLK |= BIT(8); // CamExternal Clock = ON
	swiDelay(0x1E);
	NM_REG_CAM_MCNT = BIT(1) | BIT(5); // Camera Module Control
	swiDelay(0x2008);
	REG_SCFG_CLK &= ~BIT(8); // CamExternal Clock = OFF
	NM_REG_CAM_CNT &= ~BIT(15); // allow changing params
	NM_REG_CAM_CNT |= BIT(5);   // flush data fifo
	NM_REG_CAM_CNT = (NM_REG_CAM_CNT & ~(0x0300)) | 0x0200;
	NM_REG_CAM_CNT |= BIT(10);
	NM_REG_CAM_CNT |= BIT(11); // irq enable
	REG_SCFG_CLK |= BIT(8);    // CamExternal Clock = ON
	swiDelay(0x14);

	// Run the Aptina init sequence over the ARM7's I2C bus
	u32 reply = 0;
	const bool ok = armCommand(NMCAM_CMD_INIT, CAM_INIT_TIMEOUT, &reply);

	REG_SCFG_CLK &= ~BIT(8); // CamExternal Clock = OFF
	REG_SCFG_CLK |= BIT(8);  // CamExternal Clock = ON
	swiDelay(0x14);

	activeCamera = CAM_NONE;
	activeMode = CAPTURE_MODE_PREVIEW;
	return ok;
}

static void deactivate(Camera cam) {
	const u32 cmd = (cam == CAM_INNER) ? NMCAM_CMD_DEACTIVATE_INNER : NMCAM_CMD_DEACTIVATE_OUTER;
	armCommand(cmd, CAM_CMD_TIMEOUT, NULL);
}

bool cameraActivate(Camera cam) {
	if (cam == CAM_NONE)
		return false;

	if (activeCamera != CAM_NONE)
		cameraDeactivateActive();

	const u32 cmd = (cam == CAM_INNER) ? NMCAM_CMD_ACTIVATE_INNER : NMCAM_CMD_ACTIVATE_OUTER;
	if (!armCommand(cmd, CAM_CMD_TIMEOUT, NULL))
		return false;

	activeCamera = cam;
	// A freshly woken sensor is in preview (context A) mode
	activeMode = CAPTURE_MODE_PREVIEW;
	return true;
}

void cameraDeactivateActive(void) {
	if (activeCamera == CAM_NONE)
		return;

	cameraTransferStop();
	deactivate(activeCamera);
	activeCamera = CAM_NONE;
}

Camera cameraActive(void) { return activeCamera; }

bool cameraTransferStart(u16 *dst, CaptureMode mode) {
	if (activeCamera == CAM_NONE)
		return false;

	const bool preview = (mode == CAPTURE_MODE_PREVIEW);

	if (mode != activeMode) {
		if (!armCommand(preview ? NMCAM_CMD_MODE_PREVIEW : NMCAM_CMD_MODE_CAPTURE, CAM_CMD_TIMEOUT, NULL))
			return false;
		activeMode = mode;
	}

	if (NM_REG_CAM_CNT & BIT(15))
		cameraTransferStop();

	const u32 bytes = preview ? (256 * 192 * 2) : CAM_CAPTURE_BYTES;
	if ((u32)dst >= 0x02000000 && (u32)dst < 0x03000000) {
		// The DMA writes behind the CPU's back; make sure no stale cache lines are left.
		DC_InvalidateRange(dst, bytes);
	}

	if (preview) // enable YUV-to-RGB555 and set "scanline count - 1" to 3
		NM_REG_CAM_CNT |= 0x2003;
	else // raw YUV and "scanline count - 1" to 0
		NM_REG_CAM_CNT &= ~0x2003;
	NM_REG_CAM_CNT |= BIT(5);                 // flush data fifo
	NM_REG_CAM_CNT |= BIT(15);                // start transfer
	NM_REG_NDMA1SAD = (u32)&NM_REG_CAM_DAT;   // source CAM_DAT
	NM_REG_NDMA1DAD = (u32)dst;               // dest RAM/VRAM
	NM_REG_NDMA1TCNT = bytes / 4;             // total length in words
	NM_REG_NDMA1WCNT = preview ? 512 : 320;   // block length in words
	NM_REG_NDMA1BCNT = 2;                     // timing interval
	NM_REG_NDMA1CNT = 0x8B044000;             // start camera DMA
	return true;
}

void cameraTransferStop(void) { NM_REG_CAM_CNT &= ~BIT(15); }

bool cameraTransferActive(void) { return (NM_REG_NDMA1CNT & BIT(31)) != 0; }

void cameraShutdown(void) {
	if (!cameraClocksOn)
		return;

	cameraTransferStop();
	NM_REG_NDMA1CNT &= ~BIT(31); // make sure the camera DMA is not running any more
	cameraDeactivateActive();

	NM_REG_CAM_MCNT = 0;
	REG_SCFG_CLK &= ~BIT(8); // CamExternal Clock = OFF
	REG_SCFG_CLK &= ~BIT(2); // CamInterfaceClock = OFF
	cameraClocksOn = false;
}
