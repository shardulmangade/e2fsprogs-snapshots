/*
 * uuidd.c --- UUID-generation daemon
 *
 * Copyright (C) 2007  Theodore Ts'o
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <signal.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern int getopt(int argc, char * const argv[], const char *optstring);
extern char *optarg;
extern int optind;
#endif
#include "uuid/uuid.h"
#include "uuid/uuidd.h"
#include "nls-enable.h"

#ifdef __GNUC__
#define CODE_ATTR(x) __attribute__(x)
#else
#define CODE_ATTR(x)
#endif

static void usage(const char *progname)
{
	fprintf(stderr, _("Usage: %s [-d] [-p pidfile] [-s socketpath] "
			  "[-T timeout]\n"), progname);
	fprintf(stderr, _("       %s [-r|t] [-n num] [-s socketpath]\n"),
		progname);
	fprintf(stderr, _("       %s -k\n"), progname);
	exit(1);
}

static void create_daemon(const char *pidfile_path)
{
	pid_t pid;
	uid_t euid;
	FILE *f;

	pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(1);
	} else if (pid != 0) {
	    exit(0);
	}

	close(0);
	close(1);
	close(2);
	open("/dev/null", O_RDWR);
	open("/dev/null", O_RDWR);
	open("/dev/null", O_RDWR);

	chdir("/");
	(void) setsid();
	euid = geteuid();
	(void) setreuid(euid, euid);

	f = fopen(pidfile_path, "w");
	if (f) {
		fprintf(f, "%d\n", getpid());
		fclose(f);
	}
}

static int read_all(int fd, char *buf, size_t count)
{
	ssize_t ret;
	int c = 0;

	memset(buf, 0, count);
	while (count > 0) {
		ret = read(fd, buf, count);
		if (ret < 0) {
			if ((errno == EAGAIN) || (errno == EINTR))
				continue;
			return -1;
		}
		count -= ret;
		buf += ret;
		c += ret;
	}
	return c;
}

static const char *cleanup_pidfile, *cleanup_socket;

static void terminate_intr(int signo CODE_ATTR((unused)))
{
	(void) unlink(cleanup_pidfile);
	(void) unlink(cleanup_socket);
	exit(0);
}

static void server_loop(const char *socket_path, int debug,
			const char *pidfile_path,
			int timeout, int quiet)
{
	struct sockaddr_un	my_addr, from_addr;
	unsigned char		reply_buf[1024], *cp;
	socklen_t		fromlen;
	int32_t			reply_len = 0;
	uuid_t			uu;
	mode_t			save_umask;
	char			op, str[37];
	int			i, s, ns, len, num;

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		if (!quiet)
			fprintf(stderr, _("Couldn't create unix stream "
					  "socket: %s"), strerror(errno));
		exit(1);
	}

	/*
	 * Create the address we will be binding to.
	 */
	my_addr.sun_family = AF_UNIX;
	strcpy(my_addr.sun_path, socket_path);
	(void) unlink(socket_path);
	save_umask = umask(0);
	if (bind(s, (const struct sockaddr *) &my_addr,
		 sizeof(struct sockaddr_un)) < 0) {
		if (!quiet)
			fprintf(stderr,
				_("Couldn't bind unix socket %s: %s\n"),
				socket_path, strerror(errno));
		exit(1);
	}
	(void) umask(save_umask);

	if (listen(s, 5) < 0) {
		if (!quiet)
			fprintf(stderr, _("Couldn't listen on unix "
					  "socket %s: %s\n"), socket_path,
				strerror(errno));
		exit(1);
	}

	if (!debug) {
		create_daemon(pidfile_path);
		cleanup_pidfile = pidfile_path;
		cleanup_socket = socket_path;
		signal(SIGHUP, terminate_intr);
		signal(SIGINT, terminate_intr);
		signal(SIGPIPE, terminate_intr);
		signal(SIGTERM, terminate_intr);
		signal(SIGALRM, terminate_intr);
	}
	signal(SIGPIPE, SIG_IGN);

	while (1) {
		fromlen = sizeof(from_addr);
		if (timeout > 0)
			alarm(timeout);
		ns = accept(s, (struct sockaddr *) &from_addr, &fromlen);
		alarm(0);
		if (ns < 0) {
			if ((errno == EAGAIN) || (errno == EINTR))
				continue;
			perror("accept");
			exit(1);
		}
		len = read(ns, &op, 1);
		if (len != 1) {
			if (len < 0)
				perror("read");
			else
				printf(_("Error reading from client, "
					 "len = %d\n"), len);
			goto shutdown_socket;
		}
		if ((op == 4) || (op == 5)) {
			if (read_all(ns, (char *) &num, sizeof(num)) != 4)
				goto shutdown_socket;
			if (debug)
				printf(_("operation %d, incoming num = %d\n"),
				       op, num);
		} else if (debug)
			printf("operation %d\n", op);

		switch(op) {
		case UUIDD_OP_GETPID:
			sprintf((char *) reply_buf, "%d", getpid());
			reply_len = strlen((char *) reply_buf)+1;
			break;
		case UUIDD_OP_GET_MAXOP:
			sprintf((char *) reply_buf, "%d", UUIDD_MAX_OP);
			reply_len = strlen((char *) reply_buf)+1;
			break;
		case UUIDD_OP_TIME_UUID:
			num = 1;
			uuid__generate_time(uu, &num);
			if (debug) {
				uuid_unparse(uu, str);
				printf(_("Generated time UUID: %s\n"), str);
			}
			memcpy(reply_buf, uu, sizeof(uu));
			reply_len = sizeof(uu);
			break;
		case UUIDD_OP_RANDOM_UUID:
			num = 1;
			uuid__generate_random(uu, &num);
			if (debug) {
				uuid_unparse(uu, str);
				printf(_("Generated random UUID: %s\n"), str);
			}
			memcpy(reply_buf, uu, sizeof(uu));
			reply_len = sizeof(uu);
			break;
		case UUIDD_OP_BULK_TIME_UUID:
			uuid__generate_time(uu, &num);
			if (debug) {
				uuid_unparse(uu, str);
				printf(_("Generated time UUID %s and %d "
					 "following\n"), str, num);
			}
			memcpy(reply_buf, uu, sizeof(uu));
			reply_len = sizeof(uu);
			memcpy(reply_buf+reply_len, &num, sizeof(num));
			reply_len += sizeof(num);
			break;
		case UUIDD_OP_BULK_RANDOM_UUID:
			if (num < 0)
				num = 1;
			if (num > 1000)
				num = 1000;
			if (num*16 > (int) (sizeof(reply_buf)-sizeof(num)))
				num = (sizeof(reply_buf)-sizeof(num)) / 16;
			uuid__generate_random(reply_buf+sizeof(num), &num);
			if (debug) {
				printf(_("Generated %d UUID's:\n"), num);
				for (i=0, cp=reply_buf+sizeof(num);
				     i < num; i++, cp+=16) {
					uuid_unparse(cp, str);
					printf("\t%s\n", str);
				}
			}
			reply_len = (num*16) + sizeof(num);
			memcpy(reply_buf, &num, sizeof(num));
			break;
		default:
			if (debug)
				printf(_("Invalid operation %d\n"), op);
			goto shutdown_socket;
		}
		write(ns, &reply_len, sizeof(reply_len));
		write(ns, reply_buf, reply_len);
	shutdown_socket:
		close(ns);
	}
}

static int call_daemon(const char *socket_path, int op, unsigned char *buf,
		       int buflen, int *num, const char **err_context)
{
	char op_buf[8];
	int op_len;
	int s;
	ssize_t ret;
	int32_t reply_len = 0;
	struct sockaddr_un srv_addr;

	if (((op == 4) || (op == 5)) && !num) {
		if (err_context)
			*err_context = _("bad arguments");
		errno = EINVAL;
		return -1;
	}

	if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		if (err_context)
			*err_context = _("socket");
		return -1;
	}

	srv_addr.sun_family = AF_UNIX;
	strcpy(srv_addr.sun_path, socket_path);

	if (connect(s, (const struct sockaddr *) &srv_addr,
		    sizeof(struct sockaddr_un)) < 0) {
		if (err_context)
			*err_context = _("connect");
		close(s);
		return -1;
	}

	if (op == 5) {
		if ((*num)*16 > buflen-4)
			*num = (buflen-4) / 16;
	}
	op_buf[0] = op;
	op_len = 1;
	if ((op == 4) || (op == 5)) {
		memcpy(op_buf+1, num, sizeof(int));
		op_len += sizeof(int);
	}

	ret = write(s, op_buf, op_len);
	if (ret < op_len) {
		if (err_context)
			*err_context = _("write");
		close(s);
		return -1;
	}

	ret = read_all(s, (char *) &reply_len, sizeof(reply_len));
	if (ret < 0) {
		if (err_context)
			*err_context = _("read count");
		close(s);
		return -1;
	}
	if (reply_len < 0 || reply_len > buflen) {
		if (err_context)
			*err_context = _("bad response length");
		close(s);
		return -1;
	}
	ret = read_all(s, (char *) buf, reply_len);

	if ((ret > 0) && (op == 4)) {
		if (reply_len >= (int) (16+sizeof(int)))
			memcpy(buf+16, num, sizeof(int));
		else
			*num = -1;
	}
	if ((ret > 0) && (op == 5)) {
		if (*num >= (int) sizeof(int))
			memcpy(buf, num, sizeof(int));
		else
			*num = -1;
	}

	close(s);

	return ret;
}


int main(int argc, char **argv)
{
	const char	*socket_path = UUIDD_SOCKET_PATH;
	const char	*pidfile_path = UUIDD_PIDFILE_PATH;
	const char	*err_context;
	unsigned char	buf[1024], *cp;
	char   		str[37], *tmp;
	uuid_t		uu;
	uid_t		uid;
	gid_t 		gid;
	int		i, c, ret;
	int		debug = 0, do_type = 0, do_kill = 0, num = 0;
	int		timeout = 0, quiet = 0, drop_privs = 0;

#ifdef ENABLE_NLS
	setlocale(LC_MESSAGES, "");
	setlocale(LC_CTYPE, "");
	bindtextdomain(NLS_CAT_NAME, LOCALEDIR);
	textdomain(NLS_CAT_NAME);
#endif

	while ((c = getopt (argc, argv, "dkn:qp:s:tT:r")) != EOF) {
		switch (c) {
		case 'd':
			debug++;
			drop_privs++;
			break;
		case 'k':
			do_kill++;
			drop_privs++;
			break;
		case 'n':
			num = strtol(optarg, &tmp, 0);
			if ((num < 0) || *tmp) {
				fprintf(stderr, _("Bad number: %s\n"), optarg);
				exit(1);
			}
		case 'p':
			pidfile_path = optarg;
			drop_privs++;
			break;
		case 'q':
			quiet++;
			break;
		case 's':
			socket_path = optarg;
			drop_privs++;
			break;
		case 't':
			do_type = UUIDD_OP_TIME_UUID;
			drop_privs++;
			break;
		case 'T':
			timeout = strtol(optarg, &tmp, 0);
			if ((timeout < 0) || *tmp) {
				fprintf(stderr, _("Bad number: %s\n"), optarg);
				exit(1);
			}
			break;
		case 'r':
			do_type = UUIDD_OP_RANDOM_UUID;
			drop_privs++;
			break;
		default:
			usage(argv[0]);
		}
	}
	uid = getuid();
	if (uid && drop_privs) {
		gid = getgid();
#ifdef HAVE_SETRESUID
		setresuid(uid, uid, uid);
#else
		setreuid(uid, uid);
#endif
#ifdef HAVE_SETRESGID
		setresgid(gid, gid, gid);
#else
		setregid(gid, gid);
#endif
	}
	if (num && do_type) {
		ret = call_daemon(socket_path, do_type+2, buf,
				  sizeof(buf), &num, &err_context);
		if (ret < 0) {
			printf(_("Error calling uuidd daemon (%s): %s\n"),
			       err_context, strerror(errno));
			exit(1);
		}
		if (do_type == UUIDD_OP_TIME_UUID) {
			if (ret != sizeof(uu) + sizeof(num))
				goto unexpected_size;

			uuid_unparse(buf, str);

			printf(_("%s and subsequent %d UUID's\n"), str, num);
		} else {
			printf(_("List of UUID's:\n"));
			cp = buf + 4;
			if (ret != sizeof(num) + num*sizeof(uu))
				goto unexpected_size;
			for (i=0; i < num; i++, cp+=16) {
				uuid_unparse(cp, str);
				printf("\t%s\n", str);
			}
		}
		exit(0);
	}
	if (do_type) {
		ret = call_daemon(socket_path, do_type, (unsigned char *) &uu,
				  sizeof(uu), 0, &err_context);
		if (ret < 0) {
			printf(_("Error calling uuidd daemon (%s): %s\n"),
			       err_context, strerror(errno));
			exit(1);
		}
		if (ret != sizeof(uu)) {
		unexpected_size:
			printf(_("Unexpected reply length from server %d\n"),
			       ret);
			exit(1);
		}
		uuid_unparse(uu, str);

		printf("%s\n", str);
		exit(0);
	}

	/*
	 * Check to make sure there isn't another daemon running already
	 */
	ret = call_daemon(socket_path, 0, buf, sizeof(buf), 0, 0);
	if (ret > 0) {
		if (do_kill && ((do_kill = atoi((char *) buf)) > 0)) {
			ret = kill(do_kill, SIGTERM);
			if (ret < 0) {
				if (!quiet)
					fprintf(stderr,
						_("Couldn't kill uuidd running "
						  "at pid %d: %s\n"), do_kill,
						strerror(errno));
				exit(1);
			}
			if (!quiet)
				printf(_("Killed uuidd running at pid %d\n"),
				       do_kill);
			exit(0);
		}
		if (!quiet)
			printf(_("uuidd daemon already running at pid %s\n"),
			       buf);
		exit(1);
	}
	if (do_kill)
		exit(0);	/* Nothing to kill */

	server_loop(socket_path, debug, pidfile_path, timeout, quiet);
	return 0;
}