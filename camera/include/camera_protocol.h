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
	NMCAM_CMD_MODE_CAPTURE,    // Sensor context B: 640x480
	NMCAM_CMD_MIC_STOP,        // Phase 2D: stop the hardware microphone capture (see nmmic.c)
	NMCAM_CMD_MIC_STATUS       // reply data: MICCNT status bits (EMPTY/NOT_EMPTY/FULL/OVERRUN) in bits 0-3, 1 = ARM7 NDMA mic active in bit 4
};

// Phase 2D microphone capture without per-sample interrupts: the ARM7 enables the DSi microphone FIFO and an NDMA
// channel (start mode "microphone") that copies it into a ring in main RAM owned by the ARM9, so SD transfers that
// keep the ARM7 busy with interrupts masked can no longer starve the microphone.
// NMCAM_CMD_MIC_START carries the ring address in the low 28 bits (address >> 5, ring 32-byte aligned).
// Reply on success: bit 0 of the data = 1 when the codec runs at 47.6 kHz (microphone rate 23.8 kHz), else 32.7 kHz
// (microphone rate 16.364 kHz).
#define NMCAM_CMD_MIC_START_BASE 0x10000000u
#define NMCAM_CMD_MIC_START_MASK 0xF0000000u
#define NMMIC_RING_BYTES (128u * 1024u)
#define NMMIC_SENTINEL 0xA5A5A5A5u

#endif
