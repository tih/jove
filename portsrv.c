/**************************************************************************
 * This program is Copyright (C) 1986-2002 by Jonathan Payne.  JOVE is    *
 * provided by Jonathan and Jovehacks without charge and without          *
 * warranty.  You may copy, modify, and/or distribute JOVE, provided that *
 * this notice is included in all the source files and documentation.     *
 **************************************************************************/

/* This program is invoked by JOVE for two purposes related to PIPEPROCS:
 * - "kbd": gather tty input into lumps and send them to JOVE proper.
 * - "portsrv": gather process output into lumps and send them to JOVE
 * These functions are gathered into one program to reduce the number
 * of misc. programs to install.  The resulting program is small anyway.
 */

#include "jove.h"

#ifdef PIPEPROCS	/* almost the whole file! */

#include <signal.h>
#include <sys/ioctl.h>
#include "sysprocs.h"
#include "iproc.h"
extern int pause proto((void));

private struct lump	lump;

/* JOVE sends KBDSIG whenever it wants the kbd process (this program)
 * to stop competing for input from the keyboard.  JOVE does this when
 * JOVE realizes that there are no more interactive processes running.
 * The reason we go through all this trouble is that JOVE slows down
 * a lot when it's getting its keyboard input via a pipe.
 */

private SIGRESTYPE strt_read proto((int));

private SIGRESTYPE
hold_read(junk)
int	junk;	/* passed in when invoked by a signal; of no interest */
{
	signal(KBDSIG, strt_read);
	pause();
	return SIGRESVALUE;
}

private SIGRESTYPE
strt_read(junk)
int	junk;
{
	signal(KBDSIG, hold_read);
	return SIGRESVALUE;
}

private void
detach()
{
	setsid();
#ifdef TIOCNOTTY
	{
		int fd = open("/dev/tty", O_WRONLY | O_BINARY | O_CLOEXEC);
		/*
		 * if one tries to use portsrv on modern *n*x,
		 * shells (bash, dash) seem to hang, because
		 * they seem to try to open /dev/tty, even if
		 * given the -s option.  portsrv should not be
		 * needed on such machines, since they should
		 * have and use ptys, but for testing on such
		 * machines, need to detach from the
		 * controlling terminal.
		 */
		(void) ioctl(fd, TIOCNOTTY, (UnivPtr)0);
		(void) close(fd);
	}
#endif
	NEWPG();
}

private void
kbd_process()
{
	int	pid,
		n;

	detach();
	signal(SIGINT, SIG_IGN);
	pid = getpid();
	lump.header.pid = pid;

	strt_read(0);
	for (;;) {
		n = read(0, (UnivPtr) lump.data, sizeof(lump.data));
		if (n == -1) {
			if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
				break;

			continue;
		}
		lump.header.nbytes = n;
		/* It is not clear what we can do if this write fails */
		do {} while (write(1, (UnivPtr) &lump, sizeof(struct header) + n) < 0
			&& errno == EINTR);
	}
}

/* This is a server for jove sub processes.  By the time we get here, our
 * standard output goes to jove's process input.
 */

private int	tty_fd;

private void
proc_write(ptr, n)
UnivConstPtr	ptr;
size_t	n;
{
	/* It is not clear what we can do if this write fails */
	do {} while (write(1, ptr, n) < 0 && errno == EINTR);
}

private void
read_pipe(fd)
int	fd;
{
	register size_t	n;

	while ((lump.header.nbytes = read(fd, (UnivPtr) lump.data, sizeof lump.data)) > 0) {
		n = sizeof(struct header) + lump.header.nbytes;
		proc_write((UnivConstPtr) &lump, n);
	}
}

private void
proc_error(str)
char	*str;
{
	lump.header.pid = getpid();
	lump.header.nbytes = strlen(str);
	strcpy(lump.data, str);
	proc_write((UnivConstPtr) &lump, sizeof(struct header) + lump.header.nbytes);
	/* It is not clear what we can do if this write fails */
	do {} while (write(tty_fd, (UnivConstPtr)str, strlen(str)) < 0 && errno == EINTR);
	exit(-2);
}

private void
portsrv_process(argc, argv)
int	argc;
char	**argv;
{
	int	p[2];
	pid_t	pid;

	if (pipe(p) == -1)
		proc_error("Cannot pipe jove portsrv.\n");

	switch (pid = fork()) {
	case -1:
		proc_error("portsrv: cannot fork.\n");
		/*NOTREACHED*/

	case 0:
		/* We'll intercept child's output in p[0] */
		(void) dup2(p[1], 1);
		(void) dup2(p[1], 2);
		(void) close(p[0]);
		(void) close(p[1]);
		detach();
		execv(argv[1], &argv[2]);
		_exit(-4);
		/*NOTREACHED*/

	default:
		(void) close(0);
#ifndef TIOCNOTTY
		tty_fd = open("/dev/tty", O_WRONLY | O_BINARY);
#endif
		(void) signal(SIGINT, SIG_IGN);
		(void) signal(SIGQUIT, SIG_IGN);
		(void) close(p[1]);

		/* tell jove the pid of the real child as opposed to us */
		lump.header.pid = getpid();
		lump.header.nbytes = sizeof (pid_t);
		byte_copy((UnivConstPtr) &pid, (UnivPtr) lump.data, sizeof(pid_t));
		/* It is not clear what we can do if this write fails */
		do {} while (write(1, (UnivConstPtr) &lump, sizeof(struct header) + sizeof(pid_t)) < 0
			&& errno == EINTR);

		/* read proc's output and send it to jove */
		read_pipe(p[0]);

		/* received EOF - wait for child to die and then write the
		 * child's status to JOVE.
		 *
		 * Notice that we use a byte count of -1 (an otherwise
		 * impossible value) as a marker.  JOVE "knows" the real
		 * length is sizeof(wait_status_t).
		 */

		(void) close(p[0]);
		lump.header.pid = getpid();
		lump.header.nbytes = -1;	/* tell jove we are finished */
		/* try to exit like our child did ... */
		{
			wait_status_t	status;

			do {} while (wait(&status) != pid);
			byte_copy((UnivPtr)&status, (UnivPtr)lump.data,
				sizeof(status));
		}
		/* It is not clear what we can do if this write fails */
		do {} while (write(1, (UnivConstPtr) &lump,
			sizeof(struct header) + sizeof(wait_status_t)) < 0
				&& errno == EINTR);
	}
}

int
main(argc, argv)
int	argc;
char	**argv;
{
	if (strcmp(argv[0], "kbd") == 0)
		kbd_process();
	else
		portsrv_process(argc, argv);
	return 0;
}

#else /* !PIPEPROCS */
int
main()
{
	return 0;
}
#endif /* !PIPEPROCS */
