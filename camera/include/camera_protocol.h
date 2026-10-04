// nerdMod Camera: ARM9 <-> ARM7 command protocol (carried over FIFO_USER_04).
//
// The ARM9 sends one 32-bit command value; the ARM7 performs the (slow) I2C
// work from its main loop and answers with exactly one 32-bit reply.
//
// Reply format: bit 31 set = success, bits 0-15 = command specific data.
//               bit 31 clear = failure (bits 0-15 = detail, e.g. chip id read).
#ifndef NM_CAMERA_PROTOCOL_H
#define NM_CAMERA_PROTOCOL_H

#define NMCAM_FIFO_CHANNEL FIFO_USER_04

#define NMCAM_REPLY_OK 0x80000000u

enum {
	NMCAM_CMD_INIT = 1,        // Init both sensors; data = chip id of the inner sensor (0x2280 expected)
	NMCAM_CMD_ACTIVATE_INNER,  // Wake inner camera, enable its parallel output
	NMCAM_CMD_DEACTIVATE_INNER,
	NMCAM_CMD_ACTIVATE_OUTER,
	NMCAM_CMD_DEACTIVATE_OUTER,
	NMCAM_CMD_MODE_PREVIEW,    // Sensor context A: 256x192
	NMCAM_CMD_MODE_CAPTURE     // Sensor context B: 640x480
};

#endif
