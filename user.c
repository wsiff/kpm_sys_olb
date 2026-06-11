/*
 * user.c — Userspace control utility for the KPM kernel module.
 *
 * Usage:
 *   ./user --off
 *   ./user --log  --syscall read
 *   ./user --block --syscall write --pid 1234
 *   ./user --log  --file sample_fsm.json
 *   ./user --log  --file sample_fsm.json --syscall read   (--syscall ignored; FSM drives it)
 *
 * The --file flag loads a simple JSON finite-state machine.  Each state
 * names a syscall (open, read, write).  The module is set to LOG mode,
 * and the utility polls the observation counter to detect when the
 * current syscall has been observed, then advances to the next state.
 * After the last state it loops back to the first.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

#include "kpm_ioctl.h"

#define MAX_STATES      64
#define MAX_SYSCALL_LEN 16

/* ================================================================
 * Minimal JSON parser
 *
 * Extracts the string array from:  { "states": ["open", "read", ...] }
 * No external dependencies.
 * ================================================================ */

static int parse_fsm_json(const char *path,
			  char states[][MAX_SYSCALL_LEN], int max_states)
{
	FILE *fp;
	long fsize;
	char *buf, *p, *end;
	int count = 0;
	size_t nread;
	int len;

	fp = fopen(path, "r");
	if (!fp) {
		perror("Failed to open FSM file");
		return -1;
	}

	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (fsize <= 0) {
		fprintf(stderr, "FSM file is empty\n");
		fclose(fp);
		return -1;
	}

	buf = malloc((size_t)fsize + 1);
	if (!buf) {
		fprintf(stderr, "Out of memory\n");
		fclose(fp);
		return -1;
	}

	nread = fread(buf, 1, (size_t)fsize, fp);
	buf[nread] = '\0';
	fclose(fp);

	/* Locate "states" key */
	p = strstr(buf, "\"states\"");
	if (!p) {
		fprintf(stderr, "JSON: missing \"states\" key\n");
		free(buf);
		return -1;
	}
	p += strlen("\"states\"");

	/* Skip whitespace and colon */
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n' ||
		      *p == '\r' || *p == ':'))
		p++;

	if (*p != '[') {
		fprintf(stderr, "JSON: expected '[' after \"states\":\n");
		free(buf);
		return -1;
	}
	p++; /* skip '[' */

	/* Extract quoted strings */
	while (*p && count < max_states) {
		/* Skip whitespace and commas */
		while (*p && (*p == ' ' || *p == '\t' || *p == '\n' ||
			      *p == '\r' || *p == ','))
			p++;

		if (*p == ']')
			break;

		if (*p != '"') {
			fprintf(stderr, "JSON: expected '\"' in states array\n");
			free(buf);
			return -1;
		}
		p++; /* skip opening '"' */

		end = strchr(p, '"');
		if (!end) {
			fprintf(stderr, "JSON: unterminated string\n");
			free(buf);
			return -1;
		}

		len = (int)(end - p);
		if (len >= MAX_SYSCALL_LEN)
			len = MAX_SYSCALL_LEN - 1;
		memcpy(states[count], p, (size_t)len);
		states[count][len] = '\0';
		count++;

		p = end + 1; /* skip closing '"' */
	}

	free(buf);
	return count;
}

/* ================================================================
 * Helpers — syscall / mode name conversion
 * ================================================================ */

static int syscall_name_to_id(const char *name)
{
	if (strcmp(name, "open")  == 0) return KPM_SYSCALL_OPEN;
	if (strcmp(name, "read")  == 0) return KPM_SYSCALL_READ;
	if (strcmp(name, "write") == 0) return KPM_SYSCALL_WRITE;
	return -1;
}

static const char *syscall_id_to_name(int id)
{
	switch (id) {
	case KPM_SYSCALL_OPEN:  return "open";
	case KPM_SYSCALL_READ:  return "read";
	case KPM_SYSCALL_WRITE: return "write";
	default:                return "unknown";
	}
}

static const char *mode_to_name(int mode)
{
	switch (mode) {
	case KPM_MODE_OFF:   return "off";
	case KPM_MODE_LOG:   return "log";
	case KPM_MODE_BLOCK: return "block";
	default:             return "unknown";
	}
}

/* ================================================================
 * ioctl wrappers — each one also prints to the console
 * ================================================================ */

static int kpm_set_mode(int fd, int mode)
{
	if (ioctl(fd, KPM_IOCTL_SET_MODE, &mode) < 0) {
		perror("ioctl SET_MODE");
		return -1;
	}
	printf("[user] Mode set to: %s\n", mode_to_name(mode));
	return 0;
}

static int kpm_set_syscall(int fd, int sc)
{
	if (ioctl(fd, KPM_IOCTL_SET_SYSCALL, &sc) < 0) {
		perror("ioctl SET_SYSCALL");
		return -1;
	}
	printf("[user] Target syscall set to: %s\n", syscall_id_to_name(sc));
	return 0;
}

static int kpm_set_pid(int fd, int pid)
{
	if (ioctl(fd, KPM_IOCTL_SET_PID, &pid) < 0) {
		perror("ioctl SET_PID");
		return -1;
	}
	printf("[user] Target PID set to: %d\n", pid);
	return 0;
}

static int kpm_get_observe(int fd)
{
	int val = 0;

	if (ioctl(fd, KPM_IOCTL_GET_OBSERVE, &val) < 0) {
		perror("ioctl GET_OBSERVE");
		return -1;
	}
	return val;
}

static int kpm_reset_observe(int fd)
{
	if (ioctl(fd, KPM_IOCTL_RESET_OBSERVE) < 0) {
		perror("ioctl RESET_OBSERVE");
		return -1;
	}
	return 0;
}

/* ================================================================
 * FSM runner
 *
 * Continuously cycles through the states array.  In each state the
 * module is told to log a particular syscall.  When the observation
 * counter increments the FSM transitions to the next state.  After
 * the last state it wraps back to the first.
 * ================================================================ */

static int run_fsm(int fd, const char *json_path)
{
	char states[MAX_STATES][MAX_SYSCALL_LEN];
	int nstates, cur, sc_id, next, count;
	int i;

	nstates = parse_fsm_json(json_path, states, MAX_STATES);
	if (nstates <= 0) {
		fprintf(stderr, "Failed to parse FSM from %s\n", json_path);
		return -1;
	}

	printf("[fsm] Loaded %d states:", nstates);
	for (i = 0; i < nstates; i++)
		printf(" %s", states[i]);
	printf("\n");

	/* Ensure we are in LOG mode */
	if (kpm_set_mode(fd, KPM_MODE_LOG) < 0)
		return -1;

	cur = 0;
	while (1) {
		sc_id = syscall_name_to_id(states[cur]);
		if (sc_id < 0) {
			fprintf(stderr, "[fsm] Unknown syscall name: %s\n",
				states[cur]);
			return -1;
		}

		printf("[fsm] === State %d/%d: watching for '%s' ===\n",
		       cur + 1, nstates, states[cur]);

		if (kpm_set_syscall(fd, sc_id) < 0)
			return -1;
		if (kpm_reset_observe(fd) < 0)
			return -1;

		/* Poll until the syscall is observed at least once */
		while (1) {
			usleep(100000); /* 100 ms */
			count = kpm_get_observe(fd);
			if (count < 0)
				return -1;
			if (count > 0) {
				printf("[fsm] Observed '%s' (count=%d)\n",
				       states[cur], count);
				break;
			}
		}

		/* Transition */
		next = (cur + 1) % nstates;
		printf("[fsm] Transition: state %d (%s) -> state %d (%s)\n",
		       cur + 1, states[cur], next + 1, states[next]);
		cur = next;
	}

	/* unreachable — FSM loops forever (Ctrl-C to stop) */
	return 0;
}

/* ================================================================
 * Usage / main
 * ================================================================ */

static void print_usage(const char *prog)
{
	printf(
"Usage: %s [OPTIONS]\n"
"\n"
"Mode (choose one):\n"
"  --off                 Disable the module\n"
"  --log                 Enable logging mode\n"
"  --block               Enable blocking mode\n"
"\n"
"Options:\n"
"  --syscall <string>    Set target syscall (open, read, write)\n"
"  --pid <pid>           Set target PID (required for --block)\n"
"  --file <path.json>    Run FSM from JSON file (log mode only)\n"
"  --help                Show this help message\n"
"\n"
"Examples:\n"
"  %s --log --syscall read\n"
"  %s --block --syscall write --pid 1234\n"
"  %s --log --file sample_fsm.json\n"
"  %s --off\n",
	prog, prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
	int fd;
	int mode = -1;
	int syscall_id = -1;
	int pid = -1;
	const char *fsm_file = NULL;
	int opt;

	static struct option long_opts[] = {
		{ "off",     no_argument,       NULL, 'O' },
		{ "log",     no_argument,       NULL, 'L' },
		{ "block",   no_argument,       NULL, 'B' },
		{ "syscall", required_argument, NULL, 's' },
		{ "pid",     required_argument, NULL, 'p' },
		{ "file",    required_argument, NULL, 'f' },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL,      0,                 NULL,  0  }
	};

	if (argc < 2) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	while ((opt = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'O':
			mode = KPM_MODE_OFF;
			break;
		case 'L':
			mode = KPM_MODE_LOG;
			break;
		case 'B':
			mode = KPM_MODE_BLOCK;
			break;
		case 's':
			syscall_id = syscall_name_to_id(optarg);
			if (syscall_id < 0) {
				fprintf(stderr,
					"Invalid syscall: '%s' "
					"(use open, read, write)\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'p':
			pid = atoi(optarg);
			if (pid <= 0) {
				fprintf(stderr, "Invalid PID: %s\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'f':
			fsm_file = optarg;
			break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* ---- Validate flag combinations ---- */

	if (mode < 0 && !fsm_file) {
		fprintf(stderr,
			"Error: specify a mode (--off, --log, --block) "
			"or --file <json>.\n");
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	/* --file implies --log */
	if (fsm_file) {
		if (mode >= 0 && mode != KPM_MODE_LOG) {
			fprintf(stderr,
				"Error: --file can only be used with "
				"--log mode.\n");
			return EXIT_FAILURE;
		}
		mode = KPM_MODE_LOG;
	}

	if (mode == KPM_MODE_BLOCK && syscall_id < 0) {
		fprintf(stderr,
			"Error: --block requires --syscall <name>.\n");
		return EXIT_FAILURE;
	}

	if (mode == KPM_MODE_BLOCK && pid <= 0) {
		fprintf(stderr,
			"Error: --block requires --pid <pid>.\n");
		return EXIT_FAILURE;
	}

	/* ---- Open device ---- */

	fd = open(KPM_DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		perror("Failed to open " KPM_DEVICE_PATH);
		fprintf(stderr,
			"Ensure the module is loaded and the device exists:\n"
			"  sudo insmod kmodule.ko\n"
			"  sudo mknod %s c %d 0\n"
			"  sudo chmod 666 %s\n",
			KPM_DEVICE_PATH, KPM_MAJOR, KPM_DEVICE_PATH);
		return EXIT_FAILURE;
	}

	printf("[user] Opened device %s\n", KPM_DEVICE_PATH);

	/* ---- FSM path ---- */

	if (fsm_file) {
		printf("[user] Starting FSM from: %s\n", fsm_file);
		int ret = run_fsm(fd, fsm_file);
		close(fd);
		return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
	}

	/* ---- Normal (non-FSM) path ---- */

	/* Set syscall first (order matters for block mode) */
	if (syscall_id >= 0) {
		if (kpm_set_syscall(fd, syscall_id) < 0) {
			close(fd);
			return EXIT_FAILURE;
		}
	}

	/* Set PID if provided */
	if (pid > 0) {
		if (kpm_set_pid(fd, pid) < 0) {
			close(fd);
			return EXIT_FAILURE;
		}
	}

	/* Set mode last so everything is configured before it takes effect */
	if (mode >= 0) {
		if (kpm_set_mode(fd, mode) < 0) {
			close(fd);
			return EXIT_FAILURE;
		}
	}

	printf("[user] Done. Check 'sudo dmesg | grep kpm' for kernel logs.\n");
	close(fd);
	return EXIT_SUCCESS;
}
