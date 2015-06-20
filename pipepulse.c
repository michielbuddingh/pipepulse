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
#include <signal.h>
#include <poll.h>

const char command_version[] = "pipepulse version 0.1";

const char command_help[] = \
    " ... | pipepulse -f bytes.piped [--per 64k] [--every 60s] | ... \n"
    "pipepulse periodically writes how many bytes have been transferred\n"
    "through the pipe to a file.\n"
    "This can be used for rate/liveness monitoring.\n"
    "\n"
    "-f|--file                 the filename to update.\n"
    "-E|--stderr               don't write to file, use stderr.\n"
    "-p|--per <size<unit>>     update the file every <size> k.  Sizes\n"
    "                          b, k, M and G may be used.\n"
    "-e|--every <time<unit>>   update the file every <time> seconds.\n"
    "                          periods s, m, h and d can be used.\n"
    "-V|--version              version info\n"
    "-h|--help                 this help\n"
    "\n"
    "By default, the file is updated if 10 seconds and 128k have passed.\n"
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
    {"out",       required_argument, 0, 'o'},
    {"stderr",    no_argument,       0, 'E'},
    {"per",       required_argument, 0, 'p'},
    {"every",     required_argument, 0, 'e'},
    {0,           0,                 0, 0  }
};

static struct {
    long every;
    ssize_t per;
    const char * path;
    bool use_stderr;
} options = {
    .every = 10,
    .per = 1024 * 128,
    .path = NULL,
    .use_stderr = false
};


static bool parse_options(int argc, char * argv[argc]) {
    while (true) {
	int opt = getopt_long(argc, argv,
			      "hVEo:p:e:",
			      long_options, NULL);
	switch(opt) {
	case -1:
	    if (!((options.path != NULL) ^ options.use_stderr)) {
		fprintf(stderr, "Must specify either -o path or --stderr\n");
		fprintf(stderr, " ... | pipepulse -o bytes.piped [--per 64k] [--every 60s] | ... \n");
		return false;
	    }
	    return true;
	case 'V':
	    puts(command_version);
	    return false;
	case 'o':
	    options.path = optarg;
	    break;
	case 'E':
	    options.use_stderr = true;
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

static void touch(ssize_t total, ssize_t this_period) {
    char * log_data;
    int len = asprintf(&log_data, "%zd\t%zd\n", this_period, total);

    if (!options.use_stderr) {
	int fd = open(options.path,
		      O_CREAT | O_WRONLY | O_TRUNC,
		      S_IRWXU | S_IRWXG | S_IRWXO );
	if (-1 == fd) {
	    error(0, errno, "%ld Cannot create file %s", time(NULL), options.path);
	    return;
	}


	if (-1 == write(fd, log_data, len)) {
	    error(0, errno, "Could not write to heartbeat file");
	}

	close(fd);
    } else {
	fputs(log_data, stderr);
    }
}

#define STRIDE (256 * 1024)

#define BREAK (-1)
#define CONTINUE (-2)
#define TRY_SOMETHING_ELSE (-3)

static ssize_t splice_data(void) {
    ssize_t spliced = splice(STDIN_FILENO, NULL,
			     STDOUT_FILENO, NULL,
			     STRIDE,
			     SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

    if (0 == spliced) {
	// input was closed
	return BREAK;
    } else if (-1 == spliced) {
	if (EINVAL == errno) {
	    return TRY_SOMETHING_ELSE;
	} else if (EAGAIN == errno) {
	    return CONTINUE;
	} else if (EPIPE == errno) {
	    // output was closed
	    return BREAK;
	} else {
	    // undocumented error.  Exit.
	    error(EXIT_FAILURE, errno, "Error sending data from stdin to stdout");
	    return BREAK;
	}
    } else {
	return spliced;
    }
}

static ssize_t pipe_data(void) {
    static uint8_t buffer[STRIDE];
    static uint8_t * rcursor = buffer, * wcursor = buffer;

    size_t readsize = STRIDE - (rcursor - buffer);
    ssize_t rbytes = read(STDIN_FILENO, rcursor, readsize);
    if (0 == rbytes && readsize > 0) {
	// input was closed
	return BREAK;
    } else if (-1 == rbytes) {
	if (EAGAIN == errno || EWOULDBLOCK == errno) {
	    // no data at the input, but
	    // we can still try to write to the output
	} else if (EINTR == errno) {
	    return CONTINUE;
	} else {
	    // not something we can handle or ignore
	    error(EXIT_FAILURE, errno, "Error reading from input pipe");
	}
    } else {
	rcursor += rbytes;
    }

    size_t writesize = rcursor - wcursor;
    ssize_t wbytes = write(STDOUT_FILENO, wcursor, writesize);

    if (-1 == wbytes) {
	if (EAGAIN == errno || EWOULDBLOCK == errno) {
	    // file not ready for writing yet.
	    return CONTINUE;
	} else if (EINTR == errno) {
	    return CONTINUE;
	} else if (EPIPE == errno) {
	    return CONTINUE;
	} else {
	    // not something we can handle or ignore
	    error(EXIT_FAILURE, errno, "Error writing to input pipe");
	}
    } else {
	wcursor += wbytes;
    }

    if (wcursor - buffer >= STRIDE) {
	wcursor = buffer;
	rcursor = buffer;
    }

    return wbytes;
}

static void pipe_loop(int timer_fd) {

    struct pollfd fds[] = {
	{ .fd = STDIN_FILENO, .events = POLLIN | POLLERR | POLLHUP },
	{ .fd = STDOUT_FILENO, .events = POLLOUT | POLLERR | POLLHUP },
	{ .fd = timer_fd, .events = POLLIN }
    };

    struct pollfd * in = &(fds[0]), * out = &(fds[1]), * timer = &(fds[2]);

    // file descriptors can point at objects (or a combination of
    // objects) that don't support splice.
    bool use_splice = true;
    ssize_t total = 0, this_period = 0;

    while (0 <= poll(fds, sizeof fds / sizeof fds[0], -1)) {
	if ((in->revents) && (out->revents)) {
	    ssize_t spliced;
	    if (use_splice) {
		spliced = splice_data();

		if (TRY_SOMETHING_ELSE == spliced) {
		    use_splice = false;
		}
	    }

	    if (!use_splice) {
		spliced = pipe_data();
	    }

	    if (CONTINUE == spliced)
		continue;

	    if (BREAK == spliced)
		break;

	    total += spliced;
	    this_period += spliced;
	}
	if (timer->revents & POLLIN) {
	    uint64_t num_timeouts = 0;
	    if (sizeof num_timeouts != read(timer_fd, &num_timeouts, sizeof num_timeouts)) {
		error(0, errno, "Timer not available for reading");
	    }
	    if (num_timeouts == 0) {
		continue;
	    }

	    if (this_period >= options.per) {
		touch(total, this_period);
	    }
	    this_period = 0;
	}
    }

    if (this_period >= options.per) {
	touch(total, this_period);
    }
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

    int timer_fd = 0;
    {
	timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);

	if (timer_fd < 0) {
	    error(EXIT_FAILURE, errno, "Unable to create timer");
	}

	struct itimerspec timer = {
	    .it_interval = (struct timespec) {
		.tv_sec = options.every,
		.tv_nsec = 0
	    },
	    .it_value = (struct timespec) {
		.tv_sec = options.every,
		.tv_nsec = 0
	    }
	};
	if (timerfd_settime(timer_fd, 0, &timer, NULL) < 0) {
	    error(EXIT_FAILURE, errno, "Unable to set timer");
	}
    }

    {
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGPIPE);
	if (-1 == sigprocmask(SIG_BLOCK, &mask, NULL)) {
	    error(EXIT_FAILURE, errno, "Unable to install signal handler");
	}
    }

    pipe_loop(timer_fd);

    return EXIT_SUCCESS;
}
