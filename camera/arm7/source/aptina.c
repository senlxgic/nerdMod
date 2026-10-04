/*
	nerdMod Camera - Aptina (MT9V113-class) camera sensor driver for the DSi, ARM7 side.

	Register sequences and I2C framing are adapted from "dsi-camera" by Epicpkmn11
	(Pk11), which is released into the public domain (Unlicense), itself based on
	nocash's GBATEK ("DSi Aptina Camera Initialization") and on libnds' I2C code
	(Dave Murphy, zlib license).

	nerdMod changes: every wait is bounded (a dead sensor makes the call fail
	instead of hanging the ARM7), failures are reported to the caller, and the
	register sequences are otherwise kept identical to the proven reference.
*/

#include "aptina.h"

#include <nds/arm7/i2c.h>

#define NM_I2CDATA (*(vu8 *)0x04004500)
#define NM_I2CCNT (*(vu8 *)0x04004501)

enum {
	I2CF_NONE = 0x00,
	I2CF_STOP = 0x01,
	I2CF_START = 0x02,
	I2CF_ACK = 0x10,
	I2CF_READ = 0x20
};

// Upper bound for polling a sensor register, in vblanks (~1 second).
#define APT_WAIT_FRAMES 60

static bool i2cBusyWait(void) {
	for (u32 i = 0; i < 200000; i++) {
		if (!(NM_I2CCNT & 0x80))
			return true;
	}
	return false;
}

static u8 i2cResult(void) {
	i2cBusyWait();
	return (NM_I2CCNT >> 4) & 0x01;
}

static u8 aptGetData(u8 flags) {
	NM_I2CCNT = 0xC0 | flags;
	i2cBusyWait();
	return NM_I2CDATA;
}

static u8 aptSetData(u8 data, u8 flags) {
	NM_I2CDATA = data;
	NM_I2CCNT = 0xC0 | flags;
	return i2cResult();
}

static u8 aptSelectDevice(u8 device, u8 flags) {
	i2cBusyWait();
	NM_I2CDATA = device;
	NM_I2CCNT = 0xC0 | flags;
	return i2cResult();
}

static u8 aptSelectRegister(u8 reg, u8 flags) {
	NM_I2CDATA = reg;
	NM_I2CCNT = 0xC0 | flags;
	return i2cResult();
}

static u8 aptWriteRegister(u8 device, u16 reg, u16 data) {
	for (int i = 0; i < 8; i++) {
		if (aptSelectDevice(device, I2CF_START) && aptSelectRegister(reg >> 8, I2CF_NONE) && aptSelectRegister(reg & 0xFF, I2CF_NONE)) {
			if (aptSetData(data >> 8, I2CF_NONE) && aptSetData(data & 0xFF, I2CF_STOP))
				return 1;
		}
		NM_I2CCNT = 0xC5;
	}
	return 0;
}

// Returns 0xFFFF if the transfer failed.
static u16 aptReadRegister(u8 device, u16 reg) {
	for (int i = 0; i < 8; i++) {
		if (aptSelectDevice(device, I2CF_START) && aptSelectRegister(reg >> 8, I2CF_NONE) && aptSelectRegister(reg & 0xFF, I2CF_STOP)) {
			if (aptSelectDevice(device | 1, I2CF_START)) {
				const u16 hi = aptGetData(I2CF_READ | I2CF_ACK);
				const u16 lo = aptGetData(I2CF_STOP | I2CF_READ);
				return (hi << 8) | lo;
			}
		}
		NM_I2CCNT = 0xC5;
	}
	return 0xFFFF;
}

// Waits until the bits of the register are clear.
static bool aptWaitClr(u8 device, u16 reg, u16 mask) {
	for (int i = 0; i < APT_WAIT_FRAMES; i++) {
		const u16 v = aptReadRegister(device, reg);
		if (v != 0xFFFF && !(v & mask))
			return true;
		swiWaitForVBlank();
	}
	return false;
}

// Waits until the bits of the register are set.
static bool aptWaitSet(u8 device, u16 reg, u16 mask) {
	for (int i = 0; i < APT_WAIT_FRAMES; i++) {
		const u16 v = aptReadRegister(device, reg);
		if (v != 0xFFFF && (v & mask) == mask)
			return true;
		swiWaitForVBlank();
	}
	return false;
}

static void aptClr(u8 device, u16 reg, u16 mask) {
	const u16 temp = aptReadRegister(device, reg);
	aptWriteRegister(device, reg, temp & ~mask);
}

static void aptSet(u8 device, u16 reg, u16 mask) {
	const u16 temp = aptReadRegister(device, reg);
	aptWriteRegister(device, reg, temp | mask);
}

static u16 aptReadMcu(u8 device, u16 reg) {
	aptWriteRegister(device, 0x098C, reg);
	return aptReadRegister(device, 0x0990);
}

static void aptWriteMcu(u8 device, u16 reg, u16 data) {
	aptWriteRegister(device, 0x098C, reg);
	aptWriteRegister(device, 0x0990, data);
}

static bool aptWaitMcuClr(u8 device, u16 reg, u16 mask) {
	for (int i = 0; i < APT_WAIT_FRAMES; i++) {
		const u16 v = aptReadMcu(device, reg);
		if (v != 0xFFFF && !(v & mask))
			return true;
		swiWaitForVBlank();
	}
	return false;
}

static void aptSetMcu(u8 device, u16 reg, u16 mask) {
	const u16 temp = aptReadMcu(device, reg);
	aptWriteMcu(device, reg, temp | mask);
}

u16 aptReadChipId(u8 device) {
	return aptReadRegister(device, 0x0000);
}

// https://problemkaputt.de/gbatek.htm#dsiaptinacamerainitialization
bool aptInit(u8 device) {
	aptWriteRegister(device, 0x001A, 0x0003); // RESET_AND_MISC_CONTROL (issue reset)
	aptWriteRegister(device, 0x001A, 0x0000); // RESET_AND_MISC_CONTROL (release reset)
	aptWriteRegister(device, 0x0018, 0x4028); // STANDBY_CONTROL (wakeup)
	aptWriteRegister(device, 0x001E, 0x0201); // PAD_SLEW
	aptWriteRegister(device, 0x0016, 0x42DF); // CLOCKS_CONTROL
	if (!aptWaitClr(device, 0x0018, 0x4000))  // STANDBY_CONTROL (wait for WakeupDone)
		return false;
	if (!aptWaitSet(device, 0x301A, 0x0004))  // UNDOC_CORE_301A (wait for WakeupDone)
		return false;
	aptWriteMcu(device, 0x02F0, 0x0000);      // UNDOC! RAM?
	aptWriteMcu(device, 0x02F2, 0x0210);      // UNDOC! RAM?
	aptWriteMcu(device, 0x02F4, 0x001A);      // UNDOC! RAM?
	aptWriteMcu(device, 0x2145, 0x02F4);      // UNDOC! SEQ?
	aptWriteMcu(device, 0xA134, 0x0001);      // UNDOC! SEQ?
	aptSetMcu(device, 0xA115, 0x0002);        // SEQ_CAP_MODE (set bit1=video)
	aptWriteMcu(device, 0x2755, 0x0002);      // MODE_OUTPUT_FORMAT_A (bit5=0=YUV)
	aptWriteMcu(device, 0x2757, 0x0002);      // MODE_OUTPUT_FORMAT_B
	aptWriteRegister(device, 0x0014, 0x2145); // PLL_CONTROL
	aptWriteRegister(device, 0x0010, 0x0111); // PLL_DIVIDERS
	aptWriteRegister(device, 0x0012, 0x0000); // PLL_P_DIVIDERS
	aptWriteRegister(device, 0x0014, 0x244B); // PLL_CONTROL
	aptWriteRegister(device, 0x0014, 0x304B); // PLL_CONTROL
	if (!aptWaitSet(device, 0x0014, 0x8000))  // PLL_CONTROL (wait for PLL Lock okay)
		return false;
	aptClr(device, 0x0014, 0x0001);           // PLL_CONTROL (disable PLL Bypass)
	aptWriteMcu(device, 0x2703, 0x0100);      // MODE_OUTPUT_WIDTH_A   \ Size A
	aptWriteMcu(device, 0x2705, 0x00C0);      // MODE_OUTPUT_HEIGHT_A  / 256x192
	aptWriteMcu(device, 0x2707, 0x0280);      // MODE_OUTPUT_WIDTH_B   \ Size B
	aptWriteMcu(device, 0x2709, 0x01E0);      // MODE_OUTPUT_HEIGHT_B  / 640x480
	aptWriteMcu(device, 0x2715, 0x0001);      // MODE_SENSOR_ROW_SPEED_A
	aptWriteMcu(device, 0x2719, 0x001A);      // MODE_SENSOR_FINE_CORRECTION_A
	aptWriteMcu(device, 0x271B, 0x006B);      // MODE_SENSOR_FINE_IT_MIN_A
	aptWriteMcu(device, 0x271D, 0x006B);      // MODE_SENSOR_FINE_IT_MAX_MARGIN_A
	aptWriteMcu(device, 0x271F, 0x02C0);      // MODE_SENSOR_FRAME_LENGTH_A
	aptWriteMcu(device, 0x2721, 0x034B);      // MODE_SENSOR_LINE_LENGTH_PCK_A
	aptWriteMcu(device, 0xA20B, 0x0000);      // AE_MIN_INDEX
	aptWriteMcu(device, 0xA20C, 0x0006);      // AE_MAX_INDEX
	aptWriteMcu(device, 0x272B, 0x0001);      // MODE_SENSOR_ROW_SPEED_B
	aptWriteMcu(device, 0x272F, 0x001A);      // MODE_SENSOR_FINE_CORRECTION_B
	aptWriteMcu(device, 0x2731, 0x006B);      // MODE_SENSOR_FINE_IT_MIN_B
	aptWriteMcu(device, 0x2733, 0x006B);      // MODE_SENSOR_FINE_IT_MAX_MARGIN_B
	aptWriteMcu(device, 0x2735, 0x02C0);      // MODE_SENSOR_FRAME_LENGTH_B
	aptWriteMcu(device, 0x2737, 0x034B);      // MODE_SENSOR_LINE_LENGTH_PCK_B
	aptSet(device, 0x3210, 0x0008);           // COLOR_PIPELINE_CONTROL (PGA pixel shading..)
	aptWriteMcu(device, 0xA208, 0x0000);      // UNDOC! RESERVED_AE_08
	aptWriteMcu(device, 0xA24C, 0x0020);      // AE_TARGETBUFFERSPEED
	aptWriteMcu(device, 0xA24F, 0x0070);      // AE_BASETARGET
	if (device == NM_CAM_INNER) {
		aptWriteMcu(device, 0x2717, 0x0024);  // MODE_SENSOR_READ_MODE_A (read mode with x-flip)
		aptWriteMcu(device, 0x272D, 0x0024);  // MODE_SENSOR_READ_MODE_B
		aptWriteMcu(device, 0xA202, 0x0022);  // AE_WINDOW_POS
		aptWriteMcu(device, 0xA203, 0x00BB);  // AE_WINDOW_SIZE
	} else {
		aptWriteMcu(device, 0x2717, 0x0025);  // MODE_SENSOR_READ_MODE_A
		aptWriteMcu(device, 0x272D, 0x0025);  // MODE_SENSOR_READ_MODE_B
		aptWriteMcu(device, 0xA202, 0x0000);  // AE_WINDOW_POS
		aptWriteMcu(device, 0xA203, 0x00FF);  // AE_WINDOW_SIZE
	}
	aptSet(device, 0x0016, 0x0020);           // CLOCKS_CONTROL (set bit5=1, reserved)
	aptWriteMcu(device, 0xA115, 0x0072);      // SEQ_CAP_MODE (was already manipulated above)
	aptWriteMcu(device, 0xA11F, 0x0001);      // SEQ_PREVIEW_1_AWB
	if (device == NM_CAM_INNER) {
		aptWriteRegister(device, 0x326C, 0x0900); // APERTURE_PARAMETERS
		aptWriteMcu(device, 0xAB22, 0x0001);      // HG_LL_APCORR1
	} else {
		aptWriteRegister(device, 0x326C, 0x1000); // APERTURE_PARAMETERS
		aptWriteMcu(device, 0xAB22, 0x0002);      // HG_LL_APCORR1
	}
	aptWriteMcu(device, 0xA103, 0x0006);      // SEQ_CMD (06h=RefreshMode)
	if (!aptWaitMcuClr(device, 0xA103, 0x000F)) // wait for the above to become ZERO
		return false;
	aptWriteMcu(device, 0xA103, 0x0005);      // SEQ_CMD (05h=Refresh)
	if (!aptWaitMcuClr(device, 0xA103, 0x000F))
		return false;
	return true;
}

bool aptActivate(u8 device) {
	aptClr(device, 0x0018, 0x0001);           // STANDBY_CONTROL (bit0=0=wakeup)
	if (!aptWaitClr(device, 0x0018, 0x4000))  // wait for WakeupDone
		return false;
	if (!aptWaitSet(device, 0x301A, 0x0004))  // UNDOC_CORE_301A (wait for WakeupDone)
		return false;
	aptWriteRegister(device, 0x3012, 0x0010); // COARSE_INTEGRATION_TIME (Y Time)
	aptSet(device, 0x001A, 0x0200);           // RESET_AND_MISC_CONTROL (Parallel On = data on)
	if (device == NM_CAM_OUTER)
		i2cWriteRegister(0x4A, 0x31, 0x01);   // camera LED (as in the reference implementation)
	return true;
}

bool aptDeactivate(u8 device) {
	aptClr(device, 0x001A, 0x0200);           // RESET_AND_MISC_CONTROL (Parallel Off = data off)
	aptSet(device, 0x0018, 0x0001);           // STANDBY_CONTROL (set bit0=1=Standby)
	bool ok = aptWaitSet(device, 0x0018, 0x4000); // wait for StandbyDone
	ok = aptWaitClr(device, 0x301A, 0x0004) && ok;
	if (device == NM_CAM_OUTER)
		i2cWriteRegister(0x4A, 0x31, 0x00);   // camera LED off
	return ok;
}

bool aptSetMode(u8 device, u16 mode) {
	aptWriteMcu(device, 0xA103, mode);
	return aptWaitMcuClr(device, 0xA103, 0xFFFF);
}
