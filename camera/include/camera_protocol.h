// nerdMod Camera: ARM9 <-> ARM7 command protocol (carried over FIFO_USER_04).
//
// The ARM9 sends one 32-bit command value; the ARM7 performs the (slow) I2C
// work from its main loop and answers with exactly one 32-bit reply.
//
// Reply format: bit 31 set = success, bits 0-15 = command specific data.
//               bit 31 clear = failure (bits 0-15 = detail).
//
// NMCAM_CMD_INIT reply: success if at least one sensor answered. Bit 0 = inner
// sensor (I2C 0x7A) ready, bit 1 = outer sensor (I2C 0x78) ready. A sensor is
// ready only if its init sequence finished and its chip id reads 0x2280.
// bit 15 set in the data = the ARM7's SCFG_EXT does not give access to the
// camera I2C bus (locked / not running in DSi mode).
#ifndef NM_CAMERA_PROTOCOL_H
#define NM_CAMERA_PROTOCOL_H

#define NMCAM_FIFO_CHANNEL FIFO_USER_04

#define NMCAM_REPLY_OK 0x80000000u
#define NMCAM_INIT_INNER_OK 0x0001u
#define NMCAM_INIT_OUTER_OK 0x0002u
#define NMCAM_INIT_NO_ACCESS 0x8000u
#define NMCAM_CHIP_ID 0x2280u

enum {
	NMCAM_CMD_INIT = 1,        // Init both sensors (see above)
	NMCAM_CMD_ACTIVATE_INNER,  // Wake inner camera, enable its parallel output
	NMCAM_CMD_DEACTIVATE_INNER,
	NMCAM_CMD_ACTIVATE_OUTER,
	NMCAM_CMD_DEACTIVATE_OUTER,
	NMCAM_CMD_MODE_PREVIEW,    // Sensor context A: 256x192
	NMCAM_CMD_MODE_CAPTURE     // Sensor context B: 640x480
};

#endif
