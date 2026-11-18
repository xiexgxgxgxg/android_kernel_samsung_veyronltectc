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
 * filesystem.  A 16 KiB tail is 33 commands, ~tens of milliseconds, which is
 * fine for a 300 ms period.
 *
 * On-card layout (last 8 MiB of the card)
 * ---------------------------------------
 *   sector 0 .. 16381      ring of records, appended and wrapped
 *   sector 16382, 16383    two alternating superblock copies
 *
 * A record is:
 *   +0      struct sdlog_rec_hdr (48 bytes, magic "2DLG")
 *   +512    len bytes of kernel log, padded to whole sectors
 *
 * The superblock stores boot_count, cursor and seq, so a new boot continues
 * where the previous one stopped instead of starting at sector 0.  This is
 * what makes a boot loop readable: the ring keeps the last ~150 s of writes
 * (roughly 500 records), so several failed boots survive a power cycle and
 * even a TWRP boot (whose older kernel writes one legacy record at sector 0)
 * cannot wipe them.
 *
 * Records written by the previous (single-slot) driver use magic "SDLG" and
 * are still decoded by sdlog_decode.py.
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

#define SDLOG_MAGIC_V1		0x474c4453U	/* "SDLG", old single-slot records */
#define SDLOG_MAGIC_V2		0x474c4432U	/* "2DLG", this driver */
#define SDLOG_SB_MAGIC		0x534c4453U	/* "SDLS" */
#define SDLOG_SB_VERSION	1
#define SDLOG_TAG_LEN		16
#define SDLOG_RESERVE_SECTORS	16384UL		/* 8 MiB at the end of the card */
#define SDLOG_RING_SECTORS	(SDLOG_RESERVE_SECTORS - 2)
#define SDLOG_BLOCK_SIZE	512
#define SDLOG_MAX_BYTES		(64 * 1024)	/* 128 data blocks + header */
#define SDLOG_ALIGN		64

struct sdlog_rec_hdr {
	u32 magic;
	u32 seq;
	u32 len;
	u32 period_ms;
	u32 boot;
	u32 reserved;
	u64 jiffies;
	char tag[SDLOG_TAG_LEN];
};

struct sdlog_sb {
	u32 magic;
	u32 version;
	u32 boot;
	u32 cursor;
	u32 seq;
	u32 sum;
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
static u8 sdlog_sb_blk[SDLOG_BLOCK_SIZE] __attribute__((aligned(SDLOG_ALIGN)));
static size_t sdlog_len;
static unsigned int sdlog_attempts;
static unsigned int sdlog_seq;
static unsigned int sdlog_boot;
static unsigned int sdlog_cursor;
static unsigned int sdlog_sb_slot;
static unsigned long sdlog_lba;		/* first sector of the log region */
static unsigned long sdlog_writes;
static bool sdlog_state_loaded;
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

/* One CMD17.  Must be called with the host claimed by the caller. */
static int sdlog_read_block(struct mmc_card *card, unsigned long lba,
			    void *buf)
{
	struct mmc_host *host = card->host;
	struct mmc_request mrq;
	struct mmc_command cmd;
	struct mmc_data data;
	struct scatterlist sg;

	memset(&mrq, 0, sizeof(mrq));
	memset(&cmd, 0, sizeof(cmd));
	memset(&data, 0, sizeof(data));

	sg_init_one(&sg, buf, SDLOG_BLOCK_SIZE);

	mrq.cmd = &cmd;
	mrq.data = &data;

	cmd.opcode = MMC_READ_SINGLE_BLOCK;
	cmd.arg = mmc_card_blockaddr(card) ? lba : (lba << 9);
	cmd.flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC;

	data.blksz = SDLOG_BLOCK_SIZE;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;
	data.sg = &sg;
	data.sg_len = 1;

	mmc_set_data_timeout(&data, card);

	mmc_wait_for_req(host, &mrq);

	if (cmd.error)
		return cmd.error;
	return data.error;
}

static u32 sdlog_sb_sum(const struct sdlog_sb *sb)
{
	return sb->magic ^ sb->version ^ sb->boot ^ sb->cursor ^ sb->seq ^
	       0x5a5a5a5aU;
}

/*
 * Read one superblock copy.  Returns 0 and fills sb on success.
 * Must be called with the host claimed.
 */
static int sdlog_sb_read(struct mmc_card *card, unsigned int slot,
			 struct sdlog_sb *sb)
{
	int err;

	memset(sdlog_sb_blk, 0, SDLOG_BLOCK_SIZE);
	err = sdlog_read_block(card, sdlog_lba + SDLOG_RING_SECTORS + slot,
			       sdlog_sb_blk);
	if (err)
		return err;

	memcpy(sb, sdlog_sb_blk, sizeof(*sb));
	if (sb->magic != SDLOG_SB_MAGIC || sb->version != SDLOG_SB_VERSION)
		return -EINVAL;
	if (sb->sum != sdlog_sb_sum(sb))
		return -EINVAL;
	if (sb->cursor >= SDLOG_RING_SECTORS)
		return -EINVAL;
	return 0;
}

/* Must be called with the host claimed. */
static int sdlog_sb_write(struct mmc_card *card)
{
	struct sdlog_sb sb;

	memset(&sb, 0, sizeof(sb));
	sb.magic = SDLOG_SB_MAGIC;
	sb.version = SDLOG_SB_VERSION;
	sb.boot = sdlog_boot;
	sb.cursor = sdlog_cursor;
	sb.seq = sdlog_seq;
	sb.sum = sdlog_sb_sum(&sb);

	memset(sdlog_sb_blk, 0, SDLOG_BLOCK_SIZE);
	memcpy(sdlog_sb_blk, &sb, sizeof(sb));

	return sdlog_write_block(card,
				 sdlog_lba + SDLOG_RING_SECTORS + sdlog_sb_slot,
				 sdlog_sb_blk);
}

/*
 * Pick up the ring position from the previous boot.  Two alternating copies
 * mean a power cut in the middle of a superblock write still leaves a valid
 * one behind.  Must be called with the host claimed.
 */
static void sdlog_load_state(struct mmc_card *card)
{
	struct sdlog_sb a, b;
	bool va, vb;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	va = sdlog_sb_read(card, 0, &a) == 0;
	vb = sdlog_sb_read(card, 1, &b) == 0;

	if (!va && !vb) {
		sdlog_boot = 1;
		sdlog_cursor = 0;
		sdlog_seq = 0;
		pr_info("sdlog: no superblock, starting a new ring\n");
	} else {
		if (vb && (!va || b.boot > a.boot ||
			   (b.boot == a.boot && b.cursor > a.cursor))) {
			a = b;
			va = true;
		}
		sdlog_boot = a.boot + 1;
		sdlog_cursor = a.cursor;
		sdlog_seq = a.seq;
		pr_info("sdlog: boot %u, resuming at sector %u, seq %u\n",
			sdlog_boot, sdlog_cursor, sdlog_seq);
	}

	/* Alternate the copy we write next. */
	sdlog_sb_slot = sdlog_boot & 1;
}

static void sdlog_flush(void)
{
	struct mmc_card *card = sdlog_card();
	struct sdlog_rec_hdr hdr;
	struct mmc_host *host;
	size_t off;
	unsigned long lba;
	unsigned int blocks;
	int err = 0;

	if (!card) {
		if (sdlog_attempts < 5)
			pr_info("sdlog: no SD card on the mmc bus yet (%u)\n",
				++sdlog_attempts);
		return;
	}

	host = card->host;

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
		mmc_claim_host(host);
		mmc_set_blocklen(card, SDLOG_BLOCK_SIZE);
		if (!sdlog_state_loaded) {
			sdlog_load_state(card);
			sdlog_state_loaded = true;
		}
		mmc_release_host(host);

		pr_info("sdlog: region %lu, blockaddr %d, capacity %u\n",
			sdlog_lba, !!mmc_card_blockaddr(card),
			card->csd.capacity);
	}

	/* snapshot the log ring (kmsg_dump() sets up the iterator) */
	sdlog_self_dump = true;
	kmsg_dump(KMSG_DUMP_OOPS);
	sdlog_self_dump = false;

	blocks = 1 + DIV_ROUND_UP(sdlog_len, SDLOG_BLOCK_SIZE);
	if (blocks > SDLOG_RING_SECTORS) {
		blocks = SDLOG_RING_SECTORS;
		sdlog_len = (blocks - 1) * SDLOG_BLOCK_SIZE;
	}

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = SDLOG_MAGIC_V2;
	hdr.seq = sdlog_seq + 1;
	hdr.len = (u32)sdlog_len;
	hdr.period_ms = sdlog_period_ms;
	hdr.boot = sdlog_boot;
	hdr.jiffies = (u64)jiffies;
	memcpy(hdr.tag, sdlog_tag, SDLOG_TAG_LEN);

	memset(sdlog_hdr_blk, 0, sizeof(sdlog_hdr_blk));
	memcpy(sdlog_hdr_blk, &hdr, sizeof(hdr));

	mmc_claim_host(host);

	/* Wrap before the record, never through it. */
	if (sdlog_cursor + blocks > SDLOG_RING_SECTORS)
		sdlog_cursor = 0;

	/* header, then the log tail */
	lba = sdlog_lba + sdlog_cursor;
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

	if (!err) {
		sdlog_cursor += blocks;
		if (sdlog_cursor >= SDLOG_RING_SECTORS)
			sdlog_cursor = 0;
		sdlog_seq++;
		/* Keep the next boot's cursor fresh; alternate the copies. */
		sdlog_sb_slot ^= 1;
		err = sdlog_sb_write(card);
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
			pr_info("sdlog: raw mmc ok, capacity %u sectors, region %lu, ring %lu sectors\n",
				card->csd.capacity, sdlog_lba,
				SDLOG_RING_SECTORS);
		} else if (sdlog_verbose) {
			pr_info("sdlog: wrote %zu bytes (#%lu, boot %u, seq %u)\n",
				sdlog_len, sdlog_writes, sdlog_boot, sdlog_seq);
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
	if (reason == KMSG_DUMP_PANIC) {
		/*
		 * The box is going down right now; a workqueue flush is too
		 * late (the scheduler may never run again).  Do it here, on
		 * the panic CPU, while the buffer still has the oops text.
		 */
		sdlog_self_dump = false;
		if (sdlog_state_loaded || sdlog_card())
			sdlog_flush();
	}

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
	pr_info("sdlog: armed (raw mmc ring, no block layer), period %d ms, %d bytes\n",
		sdlog_period_ms, SDLOG_MAX_BYTES);
	return 0;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Veyron kernel log ring on microSD via the MMC core");
core_initcall(sdlog_init);
