/*
 * veyron_diag.c - report stuck tasks without depending on printk.
 *
 * The bring-up freezes at ~9 s: printk stops mid-line, the system workqueues
 * stop running (so the sdlog writer stops too) and there is no adb.  A
 * workqueue based dumper is useless in that state, so this is a plain kthread
 * that talks to the SD card through sdlog_emit() -- no printk, no logbuf_lock,
 * no workqueue.  It writes:
 *
 *   tag "hb"    a tiny heartbeat record every two seconds, so the last
 *               heartbeat tells us exactly when the kernel stopped
 *               scheduling this thread (or when the card write wedged)
 *   tag "diag"  at selected uptimes, the D-state task list, init, the task
 *               inside misc_open() and the kernel stack of each of them
 *               (raw PCs; resolve with System.map, the kernel is not KASLR'd)
 *
 * Read with sdlog_decode.py --boot N.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>

#define DIAG_BUF		(16 * 1024)
#define DIAG_MAX_TASKS		12
#define DIAG_MAX_FRAMES		12

/* implemented in drivers/char/misc.c */
void misc_open_snapshot(pid_t *pid, char *name, size_t namesz);
/* implemented in drivers/misc/veyron_sdlog.c */
extern int sdlog_emit(const char *tag, const char *text, unsigned int len);

static struct task_struct *diag_task;


static int diag_one(char *buf, size_t room, pid_t pid, const char *why)
{
	struct task_struct *p;
	struct stack_trace trace;
	unsigned long addrs[DIAG_MAX_FRAMES];
	int off = 0, i;

	if (room < 256)
		return 0;

	rcu_read_lock();
	p = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (p)
		get_task_struct(p);
	rcu_read_unlock();
	if (!p)
		return 0;

	off += scnprintf(buf + off, room - off, "T %d %s state=%lu comm=%s\n",
			 pid, why, (unsigned long)p->state, p->comm);

	memset(&trace, 0, sizeof(trace));
	trace.nr_entries = 0;
	trace.max_entries = DIAG_MAX_FRAMES;
	trace.entries = addrs;
	trace.skip = 0;
	save_stack_trace_tsk(p, &trace);

	for (i = 0; i < trace.nr_entries && off < room - 32; i++)
		off += scnprintf(buf + off, room - off, "  0x%lx\n", addrs[i]);

	put_task_struct(p);
	return off;
}

static int diag_report(char *buf, size_t bufsz, unsigned int rep)
{
	pid_t pids[DIAG_MAX_TASKS];
	pid_t mpid = 0;
	char mname[32] = "";
	struct task_struct *p;
	int n = 0, i, total = 0, off = 0;

	misc_open_snapshot(&mpid, mname, sizeof(mname));

	rcu_read_lock();
	for_each_process(p) {
		total++;
		if ((READ_ONCE(p->state) & TASK_UNINTERRUPTIBLE) && n < DIAG_MAX_TASKS)
			pids[n++] = p->pid;
	}
	rcu_read_unlock();

	off += scnprintf(buf + off, bufsz - off,
			 "DIAG rep=%u jiffies=%llu total=%d dstate=%d misc_open=%d:%s\n",
			 rep, (unsigned long long)jiffies, total, n, mpid,
			 mname[0] ? mname : "-");

	for (i = 0; i < n; i++)
		off += diag_one(buf + off, bufsz - off, pids[i], "D");

	if (mpid)
		off += diag_one(buf + off, bufsz - off, mpid, "misc_holder");
	off += diag_one(buf + off, bufsz - off, 1, "init");

	return off;
}

static int diag_thread(void *data)
{
	char *buf = kmalloc(DIAG_BUF, GFP_KERNEL);
	unsigned int rep = 0;
	int n;

	if (!buf)
		return -ENOMEM;

	/*
	 * Report for the whole lifetime, not just the first seconds: the boot
	 * stops at a random point (init in the USB gadget setup, composer in
	 * the display stack), and only the most recent records survive the
	 * ring wrap.  Every record is also a heartbeat: when they stop, that
	 * is when the kernel stopped scheduling this thread.
	 */
	while (!kthread_should_stop()) {
		msleep(2000);
		n = diag_report(buf, DIAG_BUF, ++rep);
		sdlog_emit("diag", buf, n);
	}

	kfree(buf);
	return 0;
}

static int __init veyron_diag_init(void)
{
	diag_task = kthread_run(diag_thread, NULL, "vdiag");
	if (IS_ERR(diag_task))
		return PTR_ERR(diag_task);
	return 0;
}
late_initcall(veyron_diag_init);
