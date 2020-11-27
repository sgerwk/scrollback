/*
 * scrollback.c
 *
 * simple scrollback buffer for a virtual terminal
 *
 * tbd: document how to use shift-pageup and shift-pagedown instead of f11/f12
 * tbd: option for the scroll control strings: scrollup, scrolldown
 * tbd: option for the scrollup/scrolldown keycodes: keycodeup, keycodedown
 * tbd: colors
 * tbd: option for the scrollback buffer size
 * tbd: option for the number of lines to scroll (lines)
 * tbd: when scrolling, also use up/down for scrolling 1 line
 * tbd: option -k for doing the same as "loadkeys keys.txt"
 * tbd: option for single-char encodings (singlechar)
 * tbd: implement common cursor movements instead of asking the position
 */

/*
 * cursor position
 * ---------------
 *
 * most data is passed unchanged from the terminal to the shell and back; the
 * characters that are printed are also stored in the scrollback buffer; this
 * requires the current cursor position
 *
 * rather than trying to follow the cursor across the many cursor movement
 * commands (move, save/restore, move to tab stop), whenever its position is
 * needed it is found by asking the terminal via the ESC[6n command; the answer
 * to this command is intercepted and not sent to the shell
 *
 * the answer may be preceded by other characters coming from the terminal;
 * they have to be processed as usual; at the same time, data is not to be read
 * from the shell while waiting for the answer; this is achieved by:
 *
 * int unknownposition
 *	whether the cursor position is known
 *
 * void knowposition(int master, int ask);
 *	ask the terminal for the current position and wait for an answer; the
 *	latter is done by calling exchange() to only receive data from the
 *	terminal with a timeout
 *
 * int exchange(int master, int readshell, struct timeval *timeout);
 *	process a single data block from the terminal or the shell; parameters
 *	readshell and timeout tell whether to read from the shell and whether
 *	to timeout reading; data from the terminal is forwarded to the shell
 *	except the cursor position answers
 *
 * void parent(int master, pid_t pid);
 *	the main loop; calls exchange() to pass data between the terminal and
 *	the shell in both directions with no timeout
 *
 * a ESC[6n command coming from the shell (not generated internally by this
 * program) is forwarded to the terminal as the others; knowposition() is then
 * called with ask=0 to skip asking the terminal for the cursor position but to
 * still process data coming from the terminal as usual until an answer is
 * received; at that point, an answer is built and sent to the shell
 *
 * the timeout is avoids the terminal locking if for some reason the terminal
 * does not answer the cursor position query at all
 */

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <pty.h>
#include <linux/kd.h>
#include <linux/keyboard.h>

/*
 * keys for scrolling
 */
int keycodeup = 104;
int keycodedown = 109;
char *scrollup;
char *scrolldown;

/*
 * how many lines to scroll
 */
int lines;

/*
 * single-char encodings
 */
int singlechar = 0;

/*
 * log files
 */
#define DEBUGESCAPE 0x01
#define DEBUGBUFFER 0x02
int debug = DEBUGESCAPE;

FILE *logescape;
int logbuffer;

#define LOGDIR    "/run/user/%d"
#define LOGESCAPE (LOGDIR "/" "logescape")
#define LOGBUFFER (LOGDIR "/" "logbuffer")

/*
 * keys and escape sequences
 */
#define ESCAPE                0x1B
#define BS                    0x08
#define NL                    0x0A
#define CR                    0x0D
#define DEL                   0x7F

#define KEYF11                "\033[23~"
#define KEYF12                "\033[24~"
#define KEYSHIFTPAGEUP        "\033[11~"
#define KEYSHIFTPAGEDOWN      "\033[12~"

#define ASKPOSITION           "\033[6n"
#define ANSWERPOSITION        "\033[%d;%dR"
#define GETPOSITION           "%c[%d;%d%c"
#define GETPOSITIONSTARTER     ESCAPE
#define GETPOSITIONTERMINATOR          'R'
#define BLUEBACKGROUND        "\033[44m"
#define NORMALBACKGROUND      "\033[49m"
#define ERASECURSORDISPLAY    "\033[J"
#define ERASEDISPLAY          "\033[2J"
#define HOMEPOSITION          "\033[H"
#define SAVECURSOR            "\033[s"
#define RESTORECURSOR         "\033[u"

/*
 * key to function string
 */
int keytofunction(int keycode, int shift, char **keystring) {
	struct kbentry kbe;
	struct kbsentry kbse;
	int res;
	int i;

	kbe.kb_table = shift;
	kbe.kb_index = keycode;
	res = ioctl(0, KDGKBENT, &kbe);
	if (res != 0)
		return -1;
	switch (kbe.kb_value) {
	case K_HOLE:
		printf("K_HOLE\n");
		return 0;
	case K_NOSUCHMAP:
		printf("K_NOSUCHMAP\n");
		return 0;
	case K_ALLOCATED:
		printf("K_ALLOCATED\n");
		return 0;
	default:
		;
		// printf("%d: ", kbe.kb_value);
		// printf("%d ",  KTYP(kbe.kb_value));
		// printf("%d\n", KVAL(kbe.kb_value));
	}

	if (KTYP(kbe.kb_value) != KT_FN) {
		// printf("not a function key\n");
		return 0;
	}

	kbse.kb_func = KVAL(kbe.kb_value);
	if (res != 0)
		return -1;
	res = ioctl(0, KDGKBSENT, &kbse);
	for (i = 0; 0 && kbse.kb_string[i] != '\0'; i++)
		if (kbse.kb_string[i] == ESCAPE)
			printf("ESC");
		else
			printf("%c", kbse.kb_string[i]);
	// printf("\n");

	*keystring = strdup((char *) kbse.kb_string);
	return 1;
}

/*
 * control codes for the scrolling keys
 */
int scrollkeys() {
	int res;

	scrollup = KEYF11;
	scrolldown = KEYF12;

	res = keytofunction(keycodeup, K_SHIFTTAB, &scrollup);
	if (res == 0)
		printf("scrollup is F11\n");
	else if (res == -1)
		return res;
	else
		printf("scrollup is shift-pageup\n");

	res = keytofunction(keycodedown, K_SHIFTTAB, &scrolldown);
	if (res == 0)
		printf("scrolldown is F12\n");
	else if (res == -1)
		return res;
	else
		printf("scrolldown is shift-pagedown\n");

	return res;
}

/*
 * disable line buffering
 */
void nolinebuffering() {
	struct termios st;
	tcgetattr(0, &st);
	cfmakeraw(&st);
	tcsetattr(0, TCSADRAIN, &st);
}

/*
 * utf8 conversion
 */
u_int32_t utf8toucs4(unsigned char *utf8) {
	if ((utf8[0] & 0x80) == 0)
		return utf8[0];
	else if ((utf8[0] & 0xE0) == 0xC0 &&
	         (utf8[1] & 0xC0) == 0x80)
		return ((utf8[0] & 0x1F) <<  6) |
		       ((utf8[1] & 0x3F) <<  0);
	else if ((utf8[0] & 0xF0) == 0xE0 &&
	         (utf8[1] & 0xC0) == 0x80 &&
	         (utf8[2] & 0xC0) == 0x80)
		return ((utf8[0] & 0x0F) << 12) |
		       ((utf8[1] & 0x3F) <<  6) |
		       ((utf8[2] & 0x3F) <<  0);
	else if ((utf8[0] & 0xF8) == 0xF0 &&
	         (utf8[1] & 0xC0) == 0x80 &&
	         (utf8[2] & 0xC0) == 0x80 &&
	         (utf8[3] & 0xC0) == 0x80)
		return ((utf8[0] & 0x07) << 18) |
		       ((utf8[1] & 0x3F) << 12) |
		       ((utf8[2] & 0x3F) <<  6) |
		       ((utf8[3] & 0x3F) <<  0);
	else
		return -1;
}
void ucs4toutf8(u_int32_t ucs4, char buf[10]) {
	if (ucs4 < 0x80) {
		buf[0] = ucs4;
		buf[1] = '\0';
	}
	else if (ucs4 < 0x0800) {
		buf[0] = 0xC0 | ((ucs4 >>  6) & 0x1F);
		buf[1] = 0x80 | ((ucs4 >>  0) & 0x3F);
		buf[2] = '\0';
	}
	else if (ucs4 < 0x10000) {
		buf[0] = 0xE0 | ((ucs4 >> 12) & 0x0F);
		buf[1] = 0x80 | ((ucs4 >>  6) & 0x3F);
		buf[2] = 0x80 | ((ucs4 >>  0) & 0x3F);
		buf[3] = '\0';
	}
	else {
		buf[0] = 0xF0 | ((ucs4 >> 18) & 0x07);
		buf[1] = 0x80 | ((ucs4 >> 12) & 0x3F);
		buf[2] = 0x80 | ((ucs4 >>  6) & 0x3F);
		buf[3] = 0x80 | ((ucs4 >>  0) & 0x3F);
		buf[4] = '\0';
	}
}

/*
 * the scrollback buffer
 */
#define BUFFERSIZE (8 * 1024)
u_int32_t buffer[BUFFERSIZE];
int origin;		/* start of region storing a copy of the screen */
int show;		/* start of region that is shown when scrolling */

/*
 * the screen
 */
struct winsize winsize;	/* screen size */
int row, col;		/* position of the cursor */
int unknownposition;	/* is the cursor position known? */

/*
 * show a segment of the scrollback buffer on screen
 */
#define BARUP   "            " BLUEBACKGROUND "↑↑↑↑↑↑↑↑↑" NORMALBACKGROUND
#define BARDOWN "            " BLUEBACKGROUND "↓↓↓↓↓↓↓↓↓" NORMALBACKGROUND
void showscrollback() {
	int size, all, rows, i;
	char buf[10];

	size = (winsize.ws_row - (show == origin ? 0 : 2)) * winsize.ws_col;
	fprintf(stdout, HOMEPOSITION ERASEDISPLAY);
	if (show != origin) {
		all = winsize.ws_row * winsize.ws_col;
		rows = (BUFFERSIZE - all) / winsize.ws_col;
		if (show - winsize.ws_col >= 0 &&
		    rows > (origin - show) / winsize.ws_col)
			fprintf(stdout, BARUP);
		fprintf(stdout, "\r\n");
	}
	for (i = 0; i < size; i++) {
		if (singlechar)
			putc(buffer[(show + i) % BUFFERSIZE] & 0xFF, stdout);
		else {
			ucs4toutf8(buffer[(show + i) % BUFFERSIZE], buf);
			fputs(buf, stdout);
		}
	}
	if (show != origin) {
		fprintf(stdout, BARDOWN "       %d lines below",
			(origin - show) / winsize.ws_col);
	}
	else
		fprintf(stdout, RESTORECURSOR);
	fflush(stdout);
}

/*
 * exchange one block of data between terminal and shell (forward declaration)
 */
int exchange(int master, int readshell, struct timeval *timeout);

/*
 * make the current position known
 */
void knowposition(int master, int ask) {
	struct timeval tv;
	int i;

	if (ask && ! unknownposition)
		return;

	if (debug & DEBUGESCAPE)
		fprintf(logescape, "[knowposition(%d)]", ask);

	if (ask) {
		fprintf(stdout, ASKPOSITION);
		fflush(stdout);
	}

	for (i = 0; i < 4 && unknownposition; i++) {
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		exchange(master, 0, &tv);
		// fixme: if return is -1, terminate everything
	}

	if (debug & DEBUGESCAPE)
		fprintf(logescape, "[answer:%d,%d]", row, col);
}

/*
 * erase part of the scrollback buffer
 */
void erase(int startrow, int startcol, int endcol) {
	int start, i;
	start = winsize.ws_col * startrow;
	for (i = start + startcol; i < start + endcol; i++)
		buffer[(origin + i) % BUFFERSIZE] = ' ';
	start += winsize.ws_col;
	for (i = start; i < winsize.ws_row * winsize.ws_col; i++)
		buffer[(origin + i) % BUFFERSIZE] = ' ';
}

/*
 * new row
 */
void newrow() {
	if (row < winsize.ws_row - 1)
		row++;
	else {
		origin += winsize.ws_col;
		show = origin;
		erase(winsize.ws_row - 1, 0, winsize.ws_col);
	}
}

/*
 * process a character from the program
 */
#define SEQUENCELEN 40
char sequence[SEQUENCELEN];
int escape = -1;
unsigned char utf8[SEQUENCELEN];
int utf8pos = 0, utf8len = 0;
void programtoterminal(int master, unsigned char c) {
	int pos;
	u_int32_t w;
	char buf[20];

				/* input character: end scrolling mode */

	if (show != origin) {
		show = origin;
		showscrollback(winsize);
	}

				/* escape and special characters */

	if (c <= 0x1F && c != ESCAPE && c != '\b' && c != NL && c != CR) {
		putc(c, stdout);
		if (debug & DEBUGESCAPE)
			putc(c, logescape);
		escape = -1;
		unknownposition = 1;
		utf8pos = 0;
		return;
	}

	if (escape == -1 && c == ESCAPE)
		escape = 0;

	if (escape >= 0) {
		putc(c, stdout);
		if (escape >= SEQUENCELEN - 1) {
			escape = -1;
			unknownposition = 1;
			return;
		}
		sequence[escape++] = c;
		if (escape - 1 == 1) {
			if (c == '[')
				return;
			else if (c == '8')
				c = 'A';
		}
		if (c < 0x40 || c > 0x7F)
			return;

		sequence[escape] = '\0';
		if (debug & DEBUGESCAPE)
			fprintf(logescape, "<%s>", sequence);

		if (! strcmp(sequence, ERASEDISPLAY))
			erase(0, 0, winsize.ws_col);
		if (! strcmp(sequence, ERASECURSORDISPLAY)) {
			knowposition(master, 1);
			erase(row, col, winsize.ws_col);
		}
		else if (! strcmp(sequence, ASKPOSITION)) {
			fflush(stdout);
			knowposition(master, 0);
			sprintf(buf, ANSWERPOSITION, row + 1, col + 1);
			write(master, buf, strlen(buf));
			if (debug & DEBUGESCAPE)
				fprintf(logescape, "fin(%s)", buf);
			escape = -1;
			return;
		}
		escape = -1;
		if (sequence[1] != '[' || c != 'm')
			unknownposition = 1;
		return;
	}

					/* send character to terminal */

	knowposition(master, 1);
	putc(c, stdout);
	if (debug & DEBUGESCAPE)
		fprintf(logescape, "[pos:%d,%d]%c", row, col, c);

					/* utf8 */

	if ((c & 0x80) == 0 || singlechar)
		utf8pos = 0;
	else if (utf8pos >= SEQUENCELEN - 1) {
		utf8pos = 0;
		utf8len = 0;
		c = '_';
	}
	else if ((c & 0xC0) == 0xC0) {
		utf8len = (c & 0xE0) == 0xC0 ? 1 :
		          (c & 0xF0) == 0xE0 ? 2 :
		          (c & 0xF8) == 0xF0 ? 3 : 0;
		utf8pos = 0;
		utf8[utf8pos++] = c;
	}
	else if ((c & 0xC0) == 0x80) {
		if (utf8pos == 0 || utf8len == 0)
			return;
		utf8[utf8pos++] = c;
		utf8len--;
	}
	else
		return;
	if (utf8pos == 0)
		w = c;
	else {
		if (utf8len != 0)
			return;
		else {
			utf8[utf8pos] = '\0';
			if (debug & DEBUGESCAPE)
				fprintf(logescape, "[UTF8:%s]", utf8);
			w = utf8toucs4(utf8);
		}
	}

					/* update scrollback buffer */

	pos = (origin + row * winsize.ws_col + col) % BUFFERSIZE;
	if ((c == BS || c == DEL) && col > 0) {
		col--;
		buffer[(BUFFERSIZE + pos - 1) % BUFFERSIZE] = ' ';
	}
	else if (c == NL)
		newrow(winsize);
	else if (c == CR)
		col = 0;
	else {
		if (col >= winsize.ws_col) {
			col = 0;
			newrow(winsize);
		}
		buffer[pos] = w;
		col++;
	}
	if (debug & DEBUGESCAPE)
		fprintf(logescape, "[nextpos:%d,%d]", row, col);

	if (debug & DEBUGBUFFER) {
		lseek(logbuffer, 0, SEEK_SET);
		write(logbuffer, buffer, sizeof(u_int32_t) * BUFFERSIZE);
	}
}

/*
 * read the current position
 */
int readposition(char *sequence) {
	char s, t;
	int x, y;

	if (4 == sscanf(sequence, GETPOSITION, &s, &y, &x, &t) &&
	    s == GETPOSITIONSTARTER && t == GETPOSITIONTERMINATOR) {
		row = y - 1;
		col = x - 1;
		unknownposition = 0;
		if (debug & DEBUGESCAPE)
			fprintf(logescape, "[gotposition:%d,%d]", row, col);
		return 1;
        }

	return 0;
}

/*
 * process a character from the terminal
 */
int special = -1;
char specialsequence[SEQUENCELEN];
void terminaltoprogram(int master, unsigned char c, int next) {
	int len;
	int pos, size, all;
	int rows;

	if (debug & DEBUGESCAPE)
		putc(c, logescape);

	if (c == ESCAPE && next)
		special = 0;

	if (special >= 0) {
		specialsequence[special++] = c;
		if (c == '[' && special - 1 == 1)
			return;
		if (c < 0x40 || c > 0x7F)
			return;

		specialsequence[special] = '\0';
		len = special;
		special = -1;

		size = lines * winsize.ws_col;
		if (! strcmp(specialsequence, scrollup)) {
			pos = show - size;
			if (pos < 0)
				pos = 0;
			all = winsize.ws_row * winsize.ws_col;
			if (origin - pos > BUFFERSIZE - all) {
				rows = (BUFFERSIZE - all) / winsize.ws_col;
				pos = origin - rows * winsize.ws_col;
			}
			if (show == origin && pos != show)
				fprintf(stdout, SAVECURSOR);
			if (debug & DEBUGESCAPE)
				fprintf(logescape, "[UP]");
		}
		else if (! strcmp(specialsequence, scrolldown)) {
			pos = show + size;
			if (pos - origin >= 0) {
				if (show == origin)
					return;
				pos = origin;
			}
		}
		else if (readposition(specialsequence))
			return;
		else {
			write(master, specialsequence, len);
			return;
		}

		if (pos != show) {
			show = pos;
			showscrollback(winsize);
		}
		return;
	}

	write(master, &c, 1);
	return;
}

/*
 * exchange one block of data between terminal and shell
 */
int exchange(int master, int readshell, struct timeval *timeout) {
	fd_set sin;
	char buf[1024];
	int len, i;
	int res;

	if (debug & DEBUGESCAPE)
		fprintf(logescape, "[exchange(%d)]", readshell);

	FD_ZERO(&sin);
	FD_SET(STDIN_FILENO, &sin);
	if (readshell)
		FD_SET(master, &sin);

	res = select(master + 1, &sin, NULL, NULL, timeout);
	if (res == -1) {
		if (debug & DEBUGESCAPE)
			fprintf(logescape, "[errno=%d]", errno);
		return res;
	}

	if (res == 0)
		if (debug & DEBUGESCAPE)
			fprintf(logescape, "[timeout]");

	if (FD_ISSET(STDIN_FILENO, &sin)) {
		len = read(STDOUT_FILENO, &buf, 1024);
		if (len == -1)
			return -1;
		if (debug & DEBUGESCAPE)
			fprintf(logescape, "\nin[%d](", len);
		for (i = 0; i < len; i++)
			terminaltoprogram(master, buf[i], i < len - 1);
		if (debug & DEBUGESCAPE)
			fprintf(logescape, ")\n");
	}

	if (readshell && FD_ISSET(master, &sin)) {
		len = read(master, &buf, 1024);
		if (len == -1)
			return -1;
		for (i = 0; i < len; i++)
			programtoterminal(master, buf[i]);
		fflush(stdout);
	}

	return 0;
}

/*
 * parent: main loop
 */
void parent(int master, pid_t pid) {
	int i;
	char logname[4096];

	(void) pid;

	if (debug & DEBUGESCAPE) {
		if (strstr(LOGESCAPE, "%d"))
			snprintf(logname, 4096, LOGESCAPE, getuid());
		else
			strncpy(logname, LOGESCAPE, 4096);
		logescape = fopen(logname, "w");
		if (logescape == NULL) {
			perror(logname);
			exit(EXIT_FAILURE);
		}
	}
	if (debug & DEBUGBUFFER) {
		if (strstr(LOGBUFFER, "%d"))
			snprintf(logname, 4096, LOGBUFFER, getuid());
		else
			strncpy(logname, LOGBUFFER, 4096);
		logbuffer =
			creat(logname, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		if (logbuffer == -1) {
			perror(logname);
			exit(EXIT_FAILURE);
		}
	}

	nolinebuffering();
	for (i = 0; i < BUFFERSIZE; i++)
		buffer[i] = ' ';
	origin = 0;
	show = 0;
	unknownposition = 1;

	while (exchange(master, 1, NULL) == 0) {
	}

	if (debug & DEBUGESCAPE)
		fclose(logescape);
	if (debug & DEBUGBUFFER)
		close(logbuffer);
}

/*
 * main
 */
int main(int argn, char *argv[]) {
	char path[1024];
	char no[20];
	int master;
	pid_t p;
	int res;
	int tty;
	char t;
	struct termios st;

					/* do not run in self */

	if (getenv("SCROLLBACK")) {
		printf("scrollback already running\n");
		exit(EXIT_FAILURE);
	}
	setenv("SCROLLBACK", "true", 1);

					/* find and check tty */

	res = readlink("/proc/self/fd/0", path, 1024 - 1);
	if (res == -1) {
		printf("cannot determine current tty\n");
		exit(EXIT_FAILURE);
	}
	path[res] = '\0';
	if (sscanf(path, "/dev/tty%d%c", &tty, &t) != 1) {
		printf("not running on /dev/ttyX\n");
		exit(EXIT_FAILURE);
	}
	if (tty == 6) {
		printf("not running on tty6\n");
		exit(EXIT_FAILURE);
	}

	sprintf(no, "%d", tty);
	setenv("SCROLLBACKTTY", path, 1);
	setenv("SCROLLBACKNO", no, 1);

					/* window size + check if console */

	res = ioctl(1, TIOCGWINSZ, &winsize);
	if (res == -1) {
		printf("not a linux terminal, not running\n");
		exit(EXIT_FAILURE);
	}
	lines = winsize.ws_row / 2;

					/* scroll keys */

	res = scrollkeys();
	if (res == -1) {
		printf("cannot determine scroll keys\n");
		exit(EXIT_FAILURE);
	}

					/* program name */

	if (argn - 1 < 1) {
		printf("usage: %s /path/to/shell\n", argv[0]);
		exit(EXIT_FAILURE);
	}

					/* pty fork */

	p = forkpty(&master, NULL, NULL, &winsize);
	if (p == 0) {
		tcgetattr(0, &st);
		st.c_iflag &= ~(IGNBRK);
		st.c_iflag |= (BRKINT);
		tcsetattr(0, TCSADRAIN, &st);
		execvp(argv[1], argv + 1);
		perror(argv[1]);
		return EXIT_FAILURE;
	}
	else if (p == -1) {
		perror("forkpty");
		return EXIT_FAILURE;
	}

	parent(master, p);
	wait(NULL);
	system("reset -I");
	return EXIT_SUCCESS;
}

