/*
 * veyron_sdlog.c - kernel log that survives a power cycle, written to the
 *                  microSD card through the MMC core (no block layer).
 *
 * Why not the block device
 * ------------------------
 * The microSD card is fully initialised at ~1.0 s (mmc_sd_init_card runs from
 * the mmc_rescan worker), but /dev/mmcblk0 does not appear until
 * mmc_blk_init(), which is a level-6 device_initcall (~4.5 s).  On this board
 * a bring-up kernel can BUG() well before that, so a block-device based
 * writer is useless exactly when it is needed.  Moving mmc_blk_init to level
 * 4 hangs the machine (it needs the block core's later initcalls).
 *
 * So this driver talks to the card directly:
 *
 *   mmc_host_class -> struct mmc_host -> ->card (an SD card)
 *   mmc_claim_host(); MMC_WRITE_BLOCK (CMD24); mmc_wait_for_req();
 *   mmc_release_host();
 *
 * Single-block writes only: no CMD12/stop state machine, no block layer, no
 * filesystem.  A 24 KiB tail is 48 commands, ~tens of milliseconds, which is
 * fine for a 300 ms period.
 *
 * On-card layout (same as the block-based version, so sdlog_decode.py works):
 *   sector N + 0        struct sdlog_hdr { magic, seq, len, jiffies, tag }
 *   sector N + 1 ..     the last sdlog_bytes of the kernel log, as a stream
 *   N = card->csd.capacity - 16384          (last 8 MiB of the card)
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kmsg_dump.h>
#include <linux/workqueue.h>
#include <linux/scatterlist.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/mmc.h>
#include <linux/module.h>

#define SDLOG_MAGIC		0x474c4453U	/* "SDLG" */
#define SDLOG_TAG_LEN		16
#define SDLOG_RESERVE_SECTORS	16384UL		/* 8 MiB at the end of the card */
#define SDLOG_BLOCK_SIZE	512
#define SDLOG_MAX_BYTES		(16 * 1024)	/* 32 single-block writes */
#define SDLOG_ALIGN		64

struct sdlog_hdr {
	u32 magic;
	u32 seq;
	u32 len;
	u32 period_ms;
	u64 jiffies;
	char tag[SDLOG_TAG_LEN];
};

static int sdlog_enable = 1;
static int sdlog_period_ms = 300;
static int sdlog_verbose;
core_param(sdlog_enable, sdlog_enable, int, 0644);
core_param(sdlog_period_ms, sdlog_period_ms, int, 0644);
core_param(sdlog_verbose, sdlog_verbose, int, 0644);

static char sdlog_tag[SDLOG_TAG_LEN] = "boot";
static u8 sdlog_buf[SDLOG_MAX_BYTES] __attribute__((aligned(SDLOG_ALIGN)));
static u8 sdlog_hdr_blk[SDLOG_BLOCK_SIZE] __attribute__((aligned(SDLOG_ALIGN)));
static u8 sdlog_pad_blk[SDLOG_BLOCK_SIZE] __attribute__((aligned(SDLOG_ALIGN)));
static size_t sdlog_len;
static unsigned int sdlog_attempts;
static unsigned int sdlog_seq;
static unsigned long sdlog_lba;		/* first sector of the log region */
static unsigned long sdlog_writes;
static bool sdlog_self_dump;
static bool sdlog_reported_ready;
static bool sdlog_cmd_logged;
static struct kmsg_dumper sdlog_dumper;
static struct delayed_work sdlog_dwork;

static struct mmc_card *sdlog_card(void)
{
	return mmc_first_sd_card();
}

/* Driver-private helpers, but this is built in, so declare them here. */
extern int mmc_send_status(struct mmc_card *card, u32 *status);
extern int mmc_set_blocklen(struct mmc_card *card, unsigned int blocklen);

/*
 * An SD card stays busy (DAT0 low) while it programs a block.  Issuing the
 * next CMD24 before R1_READY_FOR_DATA comes back makes it time out with
 * -ETIMEDOUT, which is exactly what the first version hit.
 */
static int sdlog_wait_ready(struct mmc_card *card)
{
	u32 status;
	int i, err;

	for (i = 0; i < 1000; i++) {
		err = mmc_send_status(card, &status);
		if (err)
			return err;
		if (status & R1_READY_FOR_DATA)
			return 0;
		udelay(50);
	}
	return -ETIMEDOUT;
}

/* One CMD24.  Must be called with the host claimed by the caller. */
static int sdlog_write_block(struct mmc_card *card, unsigned long lba,
			     const void *buf)
{
	struct mmc_host *host = card->host;
	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	struct scatterlist sg;
	int err;

	memset(&mrq, 0, sizeof(mrq));
	memset(&cmd, 0, sizeof(cmd));
	memset(&data, 0, sizeof(data));

	sg_init_one(&sg, buf, SDLOG_BLOCK_SIZE);

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = MMC_WRITE_BLOCK;
	cmd.arg = mmc_card_blockaddr(card) ? lba : (lba << 9);
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = SDLOG_BLOCK_SIZE;
	data.blocks = 1;
	data.flags = MMC_DATA_WRITE;
	data.sg = &sg;
	data.sg_len = 1;

	/* Without this the host programs a zero data timeout -> -ETIMEDOUT. */
	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(host, &mrq);

	if (!sdlog_cmd_logged) {
		sdlog_cmd_logged = true;
		pr_info("sdlog: first cmd24 ok, clock %u Hz, timing %u, state %u\n",
			host->ios.clock, host->ios.timing, card->state);
	}

	err = cmd.error;
	if (!err)
		err = data.error;
	if (!err)
		err = sdlog_wait_ready(card);
	return err;
}

static void sdlog_flush(void)
{
	struct mmc_card *card = sdlog_card();
	struct sdlog_hdr hdr;
	struct mmc_host *host;
	size_t off;
	unsigned long lba;
	int err = 0;

	if (!card) {
		if (sdlog_attempts < 5)
			pr_info("sdlog: no SD card on the mmc bus yet (%u)\n",
				++sdlog_attempts);
		return;
	}

	if (!sdlog_lba) {
		if (card->csd.capacity <= SDLOG_RESERVE_SECTORS)
			return;
		sdlog_lba = (unsigned long)card->csd.capacity -
			    SDLOG_RESERVE_SECTORS;

		/*
		 * mmc_blk_probe() does this too, but it has not run yet.  An
		 * SDSC card's default block length is often 1024; writing 512
		 * bytes without CMD16 makes the card wait for the rest of the
		 * block and the transfer times out (-ETIMEDOUT).
		 */
		mmc_claim_host(card->host);
		mmc_set_blocklen(card, SDLOG_BLOCK_SIZE);
		mmc_release_host(card->host);

		pr_info("sdlog: region %lu, blockaddr %d, capacity %u\n",
			sdlog_lba, !!mmc_card_blockaddr(card),
			card->csd.capacity);
	}

	/* snapshot the log ring (kmsg_dump() sets up the iterator) */
	sdlog_self_dump = true;
	kmsg_dump(KMSG_DUMP_OOPS);
	sdlog_self_dump = false;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = SDLOG_MAGIC;
	hdr.seq = ++sdlog_seq;
	hdr.len = (u32)sdlog_len;
	hdr.period_ms = sdlog_period_ms;
	hdr.jiffies = (u64)jiffies;
	memcpy(hdr.tag, sdlog_tag, SDLOG_TAG_LEN);

	memset(sdlog_hdr_blk, 0, sizeof(sdlog_hdr_blk));
	memcpy(sdlog_hdr_blk, &hdr, sizeof(hdr));

	host = card->host;
	mmc_claim_host(host);

	/* header, then the log tail */
	lba = sdlog_lba;
	err = sdlog_write_block(card, lba++, sdlog_hdr_blk);

	for (off = 0; !err && off < sdlog_len; off += SDLOG_BLOCK_SIZE, lba++) {
		if (off + SDLOG_BLOCK_SIZE <= sdlog_len) {
			err = sdlog_write_block(card, lba, sdlog_buf + off);
		} else {
			size_t n = sdlog_len - off;

			memset(sdlog_pad_blk, 0, sizeof(sdlog_pad_blk));
			memcpy(sdlog_pad_blk, sdlog_buf + off, n);
			err = sdlog_write_block(card, lba, sdlog_pad_blk);
		}
	}

	mmc_release_host(host);

	if (!err)
		sdlog_writes++;

	if (!err) {
		if (!sdlog_reported_ready) {
			sdlog_reported_ready = true;
			pr_info("sdlog: host clock %u Hz, bus_mode %u, timing %u\n",
				card->host->ios.clock, card->host->ios.bus_mode,
				card->host->ios.timing);
			pr_info("sdlog: raw mmc ok, capacity %u sectors, region %lu, %zu bytes\n",
				card->csd.capacity, sdlog_lba, sdlog_len);
		} else if (sdlog_verbose) {
			pr_info("sdlog: wrote %zu bytes (#%lu)\n", sdlog_len,
				sdlog_writes);
		}
	} else if (sdlog_attempts < 20) {
		sdlog_attempts++;
		pr_info("sdlog: raw mmc write failed: %d\n", err);
	}
}

/*
 * kmsg_dump() initialises dumper->cur_seq/next_seq and then calls this.
 * Copy only -- we may be inside panic(), so no I/O and no sleeping here.
 */
static void sdlog_capture(struct kmsg_dumper *dumper,
			  enum kmsg_dump_reason reason)
{
	size_t len = 0;

	if (!kmsg_dump_get_buffer(dumper, false, sdlog_buf,
				  SDLOG_MAX_BYTES, &len))
		len = 0;
	sdlog_len = len;

	if (reason == KMSG_DUMP_PANIC)
		strncpy(sdlog_tag, "panic", SDLOG_TAG_LEN);
	else if (reason == KMSG_DUMP_OOPS && !sdlog_self_dump)
		strncpy(sdlog_tag, "oops", SDLOG_TAG_LEN);

	/*
	 * Get it on the card as soon as the scheduler runs again.  This is a
	 * no-op for our own periodic snapshot; it matters for a real oops
	 * when panic_on_oops=0 (the box keeps running) and for panic() after
	 * the panic notifiers have run.
	 */
	if (reason == KMSG_DUMP_PANIC || reason == KMSG_DUMP_OOPS)
		schedule_delayed_work(&sdlog_dwork, 0);
}

static void sdlog_workfn(struct work_struct *w)
{
	strncpy(sdlog_tag, "periodic", SDLOG_TAG_LEN);
	sdlog_flush();
	schedule_delayed_work(&sdlog_dwork,
			      msecs_to_jiffies(sdlog_period_ms));
}

static int __init sdlog_init(void)
{
	if (!sdlog_enable)
		return 0;

	sdlog_dumper.dump = sdlog_capture;
	kmsg_dump_register(&sdlog_dumper);

	INIT_DELAYED_WORK(&sdlog_dwork, sdlog_workfn);
	schedule_delayed_work(&sdlog_dwork, msecs_to_jiffies(200));
	pr_info("sdlog: armed (raw mmc, no block layer), period %d ms, %d bytes\n",
		sdlog_period_ms, SDLOG_MAX_BYTES);
	return 0;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Veyron kernel log to microSD via the MMC core");
core_initcall(sdlog_init);
