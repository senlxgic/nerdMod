/*
	nerdMod Camera: microphone capture on the ARM7 without per-sample interrupts (Phase 2D).

	Why: libnds' microphone service samples one value per TIMER IRQ and, on the DSi, re-enables the microphone
	controller and busy-waits for every single sample inside a critical section. The DSi SD driver runs on this same
	ARM7 and works with interrupts masked, so while the ARM9 writes video to the card the microphone gets (almost) no
	service: the owner's real recording delivered 4096 samples in 9 s.

	How: the DSi microphone controller (REG_MICCNT 0x04004600 / REG_MICDATA 0x04004604) has its own sample clock and a
	16-word FIFO that holds two mono samples per word. NDMA start mode 12 on the ARM7 is the microphone request
	(BlocksDS libnds ndma.h: NDMA_START_MIC; physical block 8 words = the FIFO "half full" level). One NDMA channel
	with src = REG_MICDATA (fixed), dst = the ARM9's ring, logical block = whole ring, infinite repeat and destination
	reload therefore keeps copying for as long as it is enabled, with no CPU involvement.

	STATUS: written from the BlocksDS headers and the melonDS DSi_I2S model (which implements the microphone FIFO but
	not NDMA mode 12), NOT verified on real hardware. The ARM9 checks that data actually arrives and falls back to the
	libnds service otherwise (audioRecorder.cpp).
*/
#include <nds.h>
#include "camera_protocol.h"

#define NMMIC_NDMA 3
#define NDMA_SAD(n)  (*(vu32 *)(0x04004104 + 0x1C * (n)))
#define NDMA_DAD(n)  (*(vu32 *)(0x04004108 + 0x1C * (n)))
#define NDMA_TCNT(n) (*(vu32 *)(0x0400410C + 0x1C * (n)))
#define NDMA_WCNT(n) (*(vu32 *)(0x04004110 + 0x1C * (n)))
#define NDMA_BCNT(n) (*(vu32 *)(0x04004114 + 0x1C * (n)))
#define NDMA_CNT(n)  (*(vu32 *)(0x0400411C + 0x1C * (n)))

#define MIC_CNT (*(vu16 *)0x04004600)
#define MIC_DATA_ADDR 0x04004604u

#define MIC_FORMAT_NORMAL 2u
#define MIC_FREQ_DIV(n) (((n) & 3u) << 2) // sample rate = codec rate / (n + 1)
#define MIC_EMPTY BIT(8)
#define MIC_NOT_EMPTY BIT(9)
#define MIC_FULL BIT(10)
#define MIC_OVERRUN BIT(11)
#define MIC_CLEAR_FIFO BIT(12)
#define MIC_ENABLE BIT(15)

#define NDMA_ENABLE BIT(31)
#define NDMA_REPEAT BIT(29)
#define NDMA_START_MIC (12u << 24)
#define NDMA_SRC_FIX (2u << 13)
#define NDMA_DST_RELOAD BIT(12)
#define NDMA_BLOCK_8_WORDS (3u << 16)

static bool micRunning = false;

u32 nmMicCommand(u32 cmd) {
	if ((cmd & NMCAM_CMD_MIC_START_MASK) == NMCAM_CMD_MIC_START_BASE) {
		const u32 addr = (cmd & ~NMCAM_CMD_MIC_START_MASK) << 5;
		if (addr < 0x02000000 || addr >= 0x0D000000 || (addr & 31))
			return 0x0000FFFE;
		if (micRunning) {
			NDMA_CNT(NMMIC_NDMA) = 0;
			MIC_CNT = 0;
		}
		const bool khz47 = (REG_SNDEXTCNT & BIT(13)) != 0;
		micOn(); // codec ADC + bias (libnds); safe to call from the main loop
		MIC_CNT = 0; // format / rate can only change while disabled
		MIC_CNT = MIC_FORMAT_NORMAL | MIC_FREQ_DIV(1) | MIC_CLEAR_FIFO;
		NDMA_CNT(NMMIC_NDMA) = 0;
		NDMA_SAD(NMMIC_NDMA) = MIC_DATA_ADDR;
		NDMA_DAD(NMMIC_NDMA) = addr;
		NDMA_TCNT(NMMIC_NDMA) = NMMIC_RING_BYTES / 4;
		NDMA_WCNT(NMMIC_NDMA) = NMMIC_RING_BYTES / 4;
		NDMA_BCNT(NMMIC_NDMA) = 0;
		NDMA_CNT(NMMIC_NDMA) = NDMA_ENABLE | NDMA_REPEAT | NDMA_START_MIC | NDMA_SRC_FIX | NDMA_DST_RELOAD | NDMA_BLOCK_8_WORDS;
		MIC_CNT |= MIC_ENABLE;
		micRunning = true;
		return NMCAM_REPLY_OK | (khz47 ? 1u : 0u);
	}
	switch (cmd) {
		case NMCAM_CMD_MIC_STOP:
			NDMA_CNT(NMMIC_NDMA) = 0;
			MIC_CNT = 0;
			if (micRunning)
				micOff();
			micRunning = false;
			return NMCAM_REPLY_OK;
		case NMCAM_CMD_MIC_STATUS: {
			const u32 c = MIC_CNT;
			return NMCAM_REPLY_OK | ((c >> 8) & 0xF) | (micRunning ? 0x10u : 0u);
		}
		default:
			return 0x0000FFFF;
	}
}
