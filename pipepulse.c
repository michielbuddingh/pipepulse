#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <poll.h>

const char command_version[] = "pipepulse version 0.1";

const char command_help[] = \
    " ... | pipepulse -f heartbeat.file [--per 64k] [--every 60s] | ... \n"
    "pipepulse periodically updates the timestamp of a file as data\n"
    "flows through it.  This can be used for rate/liveness monitoring.\n"
    "\n"
    "-f|--file                 the filename to update.  Mandatory.\n"
    "-p|--per <size<unit>>     update the file every <size> k.  Sizes\n"
    "                          b, k, M and G may be used.\n"
    "-e|--every <time<unit>>   update the file every <time> seconds.\n"
    "                          periods s, m, h and d can be used.\n"
    "-V|--version              version info\n"
    "-h|--help                 this help\n"
    "\n"
    "By default, the file is updated if 60 seconds and 128k have passed.\n"
    "\n"
    "--per may be 0; in this case, the timestamp is updated periodically\n"
    "  as long as the pipe remains open.\n"
    "--every may be 0.  If this is the case, the file will be updated\n"
    "  whenever the specified size has been read.\n";

typedef struct {
    char suffix;
    int multiplier;
} modifier;

static const modifier intervals[] = {
    { 's', 1 },
    { 'm', 60 },
    { 'h', 60 * 60 },
    { 'd', 24 * 60 * 60 },
};

static const modifier sizes[] = {
    { 'b', 1 },
    { 'k', 1024 },
    { 'M', 1024 * 1024 },
    { 'G', 1024 * 1024 * 1024 },
};

static bool parse_suffix(const char * arg, long * out, int n, const modifier table[n]) {
    char * endptr;
    long value;
    value = strtol(arg, &endptr, 10);

    if (arg == endptr) {
	return false;
    }

    int i = 0;
    for (; i < n; i++) {
	if (*endptr == table[i].suffix && *(endptr + 1) == 0) {
	    value *= table[i].multiplier;
	    break;
	}
    }

    if (i == n) {
	return false;
    }

    *out = value;

    return true;
}

static const struct option long_options[] = {
    {"help",      no_argument,       0, 'h'},
    {"version",   no_argument,       0, 'V'},
    {"file",      required_argument, 0, 'f'},
    {"per",       required_argument, 0, 'p'},
    {"every",     required_argument, 0, 'e'},
    {0,           0,                 0, 0  }
};

static struct {
    long every;
    ssize_t per;
    const char * path;
} options = {
    .every = 60,
    .per = 1024 * 128,
    .path = NULL
};


static bool parse_options(int argc, char * argv[argc]) {
    while (true) {
	int opt = getopt_long(argc, argv,
			      "hVf:p:e:",
			      long_options, NULL);
	switch(opt) {
	case -1:
	    if (options.path == NULL) {
		fprintf(stderr, "Must specify path\n");
		fprintf(stderr, " ... | pipepulse -f heartbeat.file [--per 64k] [--every 60s] | ... \n");
		return false;
	    }
	    return true;
	case 'V':
	    puts(command_version);
	    return false;
	case 'f':
	    options.path = optarg;
	    break;
	case 'p':
	    if (!parse_suffix(optarg, &options.per, sizeof sizes / sizeof(modifier), sizes)) {
		fprintf(stderr, "invalid size specification '%s'\n", optarg);
		return false;
	    }
	    break;
	case 'e':
	    if (!parse_suffix(optarg, &options.every, sizeof intervals / sizeof(modifier), intervals)) {
		fprintf(stderr, "invalid time specification '%s'\n", optarg);
		return false;
	    }
	    break;
	default:
	case 'h':
	    puts(command_help);
	    return false;
	}
    }
}

static void touch(const char * path) {
    if (utimensat(AT_FDCWD, path, NULL, 0) == -1) {
	if (ENOENT == errno) {
	    int fd = open(path, O_CREAT|O_WRONLY);
	    if (-1 == fd) {
		error(0, errno, "%ld Cannot create file %s", time(NULL), path);
	    } else {
		close(fd);
	    }
	} else {
	    error(0, errno, "%ld Cannot touch file %s", time(NULL), path);
	}
    }
}

#define STRIDE (128 * 1024)

static void pipe_loop() {
    struct pollfd fds[] = {
	{ .fd = STDIN_FILENO, .events = POLLIN | POLLERR | POLLHUP },
	{ .fd = STDOUT_FILENO, .events = POLLOUT | POLLERR | POLLHUP },
    };

    // file descriptors can point at objects (or a combination of
    // objects) that don't support splice.
    bool use_splice = true;
    ssize_t piped = 0;

    while (0 >= poll(fds, sizeof fds / sizeof fds[0], -1)) {
	if ((fds[0].revents & POLLIN) && (fds[1].revents & POLLOUT)) {
	    if (use_splice) {
		ssize_t spliced = splice(STDIN_FILENO, NULL,
					 STDOUT_FILENO, NULL,
					 STRIDE,
					 SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

		if (0 == spliced) {
		    // input was closed
		    break;
		} else if (-1 == spliced) {
		    if (EINVAL == errno) {
			use_splice = false;
		    } else if (EAGAIN == errno) {
			continue;
		    } else if (EPIPE == errno) {
			// output was closed
			break;
		    } else {
			// undocumented error.  Exit.
			error(1, errno, "Error sending data from stdin to stdout");
			break;
		    }
		}
	    } else {
		static uint8_t buffer[STRIDE];
		static uint8_t * rcursor = buffer, * wcursor = buffer;

		size_t readsize = STRIDE - (rcursor - buffer);
		ssize_t rbytes = read(STDIN_FILENO, rcursor, readsize);
		if (0 == rbytes && readsize > 0) {
		    // input was closed
		    break;
		} else if (-1 == rbytes) {
		    if (EAGAIN == errno || EWOULDBLOCK == errno) {
			// no data at the input, but
			// we can still try to write to the output
		    } else if (EINTR == errno) {
			continue;
		    } else {
			// not something we can handle or ignore
			error(1, errno, "Error reading from input pipe");
		    }
		} else {
		    rcursor += rbytes;
		}

		size_t writesize = STRIDE - (wcursor - buffer);
		ssize_t wbytes = write(STDOUT_FILENO, wcursor, writesize);

		if (-1 == wbytes) {
		    if (EAGAIN == errno || EWOULDBLOCK == errno) {
			// file not ready for writing yet.
			continue;
		    } else if (EINTR == errno) {
			continue;
		    } else if (EPIPE == errno) {
			break;
		    } else {
			// not something we can handle or ignore
			error(1, errno, "Error writing to input pipe");
		    }
		} else {
		    wcursor += wbytes;
		    piped += wbytes;
		}

		if (wcursor - buffer >= STRIDE) {
		    wcursor = buffer;
		    rcursor = buffer;
		}
	    }
	}
    }

    printf("%ld\n", piped);
    printf("%s\n", strerror(errno));

}



int main(int argc, char * argv[argc]) {
    if (!parse_options(argc, argv)) {
	return EXIT_FAILURE;
    }

    {
    	int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    	int stdout_flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    	fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
    	fcntl(STDOUT_FILENO, F_SETFL, stdout_flags | O_NONBLOCK);
    }


    pipe_loop();


    return EXIT_SUCCESS;
}
