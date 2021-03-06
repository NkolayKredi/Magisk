/* proc_monitor.cpp - Monitor am_proc_start events and unmount
 *
 * We monitor the logcat am_proc_start events. When a target starts up,
 * we pause it ASAP, and fork a new process to join its mount namespace
 * and do all the unmounting/mocking
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <vector>
#include <string>

#include <magisk.h>
#include <utils.h>
#include <logcat.h>

#include "magiskhide.h"

using namespace std;

extern char *system_block, *vendor_block, *data_block;

// Workaround for the lack of pthread_cancel
static void term_thread(int) {
	LOGD("proc_monitor: running cleanup\n");
	stop_logging(HIDE_EVENT);
	hide_list.clear();
	hide_enabled = false;
	pthread_mutex_destroy(&list_lock);
	LOGD("proc_monitor: terminating\n");
	pthread_exit(nullptr);
}

static int read_ns(const int pid, struct stat *st) {
	char path[32];
	sprintf(path, "/proc/%d/ns/mnt", pid);
	return stat(path, st);
}

static inline void lazy_unmount(const char* mountpoint) {
	if (umount2(mountpoint, MNT_DETACH) != -1)
		LOGD("hide_daemon: Unmounted (%s)\n", mountpoint);
}

static int parse_ppid(int pid) {
	char path[32];
	int ppid;
	sprintf(path, "/proc/%d/stat", pid);
	FILE *stat = fopen(path, "re");
	if (stat == nullptr)
		return -1;
	/* PID COMM STATE PPID ..... */
	fscanf(stat, "%*d %*s %*c %d", &ppid);
	fclose(stat);
	return ppid;
}

static void hide_daemon(int pid) {
	LOGD("hide_daemon: handling pid=[%d]\n", pid);

	char buffer[4096];
	vector<string> mounts;

	manage_selinux();
	clean_magisk_props();

	if (switch_mnt_ns(pid))
		goto exit;

	snprintf(buffer, sizeof(buffer), "/proc/%d", pid);
	chdir(buffer);

	mounts = file_to_vector("mounts");
	// Unmount dummy skeletons and /sbin links
	for (auto &s : mounts) {
		if (str_contains(s, "tmpfs /system/") || str_contains(s, "tmpfs /vendor/") ||
			str_contains(s, "tmpfs /sbin")) {
			sscanf(s.c_str(), "%*s %4096s", buffer);
			lazy_unmount(buffer);
		}
	}

	// Re-read mount infos
	mounts = file_to_vector("mounts");

	// Unmount everything under /system, /vendor, and data mounts
	for (auto &s : mounts) {
		if ((str_contains(s, " /system/") || str_contains(s, " /vendor/")) &&
			(str_contains(s, system_block) || str_contains(s, vendor_block) || str_contains(s, data_block))) {
			sscanf(s.c_str(), "%*s %4096s", buffer);
			lazy_unmount(buffer);
		}
	}

exit:
	// Send resume signal
	kill(pid, SIGCONT);
	_exit(0);
}

void proc_monitor() {
	// Unblock user signals
	sigset_t block_set;
	sigemptyset(&block_set);
	sigaddset(&block_set, TERM_THREAD);
	pthread_sigmask(SIG_UNBLOCK, &block_set, nullptr);

	// Register the cancel signal
	struct sigaction act{};
	act.sa_handler = term_thread;
	sigaction(TERM_THREAD, &act, nullptr);

	if (access("/proc/1/ns/mnt", F_OK) != 0) {
		LOGE("proc_monitor: Your kernel doesn't support mount namespace :(\n");
		term_thread(TERM_THREAD);
	}

	auto &queue = start_logging(HIDE_EVENT);
	while (true) {
		char *log;
		int pid, ppid;
		struct stat ns, pns;

		string line = queue.take();
		if ((log = strchr(&line[0], '[')) == nullptr)
			continue;

		// Extract pid
		if (sscanf(log, "[%*d,%d", &pid) != 1)
			continue;

		// Extract last token (component name)
		const char *tok, *cpnt = "";
		while ((tok = strtok_r(nullptr, ",[]\n", &log)))
			cpnt = tok;
		if (cpnt[0] == '\0')
			continue;

		// Make sure our target is alive
		if ((ppid = parse_ppid(pid)) < 0 || read_ns(ppid, &pns))
			continue;

		bool hide = false;
		pthread_mutex_lock(&list_lock);
		for (auto &s : hide_list) {
			if (strncmp(cpnt, s.c_str(), s.size() - 1) == 0) {
				hide = true;
				break;
			}
		}
		pthread_mutex_unlock(&list_lock);

		if (!hide)
			continue;

		while (read_ns(pid, &ns) == 0 && ns.st_dev == pns.st_dev && ns.st_ino == pns.st_ino)
			usleep(500);

		// Send pause signal ASAP
		if (kill(pid, SIGSTOP) == -1)
			continue;

		/*
		 * The setns system call do not support multithread processes
		 * We have to fork a new process, setns, then do the unmounts
		 */
		LOGI("proc_monitor: %s PID=[%d] ns=[%llu]\n", cpnt, pid, ns.st_ino);
		if (fork_dont_care() == 0)
			hide_daemon(pid);
	}
}
