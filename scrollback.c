/*
 * scrollback.c
 *
 * simple scrollback buffer for a virtual terminal
 *
 * tbd: print the keys to scroll up and down on startup
 * tbd: document how to use shift-pageup and shift-pagedown instead of f11/f12
 * tbd: option for the scroll control strings: scrollup, scrolldown
 * tbd: option for the scrollup/scrolldown keycodes: keycodeup, keycodedown
 * tbd: colors
 * tbd: option for the scrollback buffer size
 * tbd: option for the number of lines to scroll (lines)
 * tbd: when scrolling, also use up/down for scrolling 1 line
 * tbd: option -k for doing the same as "loadkeys keys.txt"
 * tbd: option for single-char encodings (singlechar)
 * tbd: implement cursor movements instead of asking the position
 */

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
#define FFEED                 0x0B

#define KEYF11                "\033[23~"
#define KEYF12                "\033[24~"
#define KEYSHIFTPAGEUP        "\033[11~"
#define KEYSHIFTPAGEDOWN      "\033[12~"

#define ASKPOSITION           "\033[6n"
#define GETPOSITION           "%c[%d;%d%c"
#define GETPOSITIONSTARTER    ESCAPE
#define GETPOSITIONTERMINATOR            'R'
#define BLUEBACKGROUND        "\033[44m"
#define NORMALBACKGROUND      "\033[49m"
#define ERASEDISPLAY          "\033[J"
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
		return 1;
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
		printf("%d: ", kbe.kb_value);
		printf("%d ",  KTYP(kbe.kb_value));
		printf("%d\n", KVAL(kbe.kb_value));
	}

	if (KTYP(kbe.kb_value) != KT_FN) {
		printf("not a function key\n");
		return 0;
	}

	kbse.kb_func = KVAL(kbe.kb_value);
	if (res != 0)
		return 1;
	res = ioctl(0, KDGKBSENT, &kbse);
	for (i = 0; kbse.kb_string[i] != '\0'; i++)
		if (kbse.kb_string[i] == ESCAPE)
			printf("ESC");
		else
			printf("%c", kbse.kb_string[i]);
	printf("\n");

	*keystring = strdup((char *) kbse.kb_string);
	return 0;
}

/*
 * control codes for the scrolling keys
 */
int scrollkeys() {
	scrollup = KEYF11;
	scrolldown = KEYF12;

	if (keytofunction(keycodeup, K_SHIFTTAB, &scrollup))
		return 1;
	if (keytofunction(keycodedown, K_SHIFTTAB, &scrolldown))
		return 1;

	return 0;
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
int origin, show;
int row, col;

/*
 * show a segment of the scrollback buffer on screen
 */
#define BARUP   "            " BLUEBACKGROUND "↑↑↑↑↑↑↑↑↑" NORMALBACKGROUND
#define BARDOWN "            " BLUEBACKGROUND "↓↓↓↓↓↓↓↓↓" NORMALBACKGROUND
void showscrollback(const struct winsize *winsize) {
	int size, all, rows, i;
	char buf[10];

	size = (winsize->ws_row - (show == origin ? 0 : 2)) * winsize->ws_col;
	fprintf(stdout, HOMEPOSITION ERASEDISPLAY);
	if (show != origin) {
		all = winsize->ws_row * winsize->ws_col;
		rows = (BUFFERSIZE - all) / winsize->ws_col;
		if (show - winsize->ws_col >= 0 &&
		    rows > (origin - show) / winsize->ws_col)
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
			(origin - show) / winsize->ws_col);
	}
	else
		fprintf(stdout, RESTORECURSOR);
	fflush(stdout);
}

/*
 * ask the current position
 */
int askposition() {
	int y, x;
	char s, t;

	fprintf(stdout, ASKPOSITION);
	fflush(stdout);
	while (4 != fscanf(stdin, GETPOSITION, &s, &y, &x, &t) ||
	       s != GETPOSITIONSTARTER ||
	       t != GETPOSITIONTERMINATOR) {
	}
	row = y - 1;
	col = x - 1;
	return 1;
}

/*
 * new row
 */
void newrow(const struct winsize *winsize) {
	int i, pos;
	if (row < winsize->ws_row - 1)
		row++;
	else {
		origin += winsize->ws_col;
		show = origin;
		for (i = 0; i < winsize->ws_col; i++) {
			pos = origin +
			      (winsize->ws_row - 1) * winsize->ws_col + i;
			buffer[pos % BUFFERSIZE] = ' ';
		}
	}
}

/*
 * process a character from the program
 */
#define SEQUENCELEN 20
char sequence[SEQUENCELEN];
int escape = -1;
unsigned char utf8[SEQUENCELEN];
int utf8pos = 0, utf8len = 0;
int new = 1;
void programtoterminal(unsigned char c, const struct winsize *winsize) {
	int pos, i;
	u_int32_t w;

				/* input character: end scrolling mode */

	if (show != origin) {
		show = origin;
		showscrollback(winsize);
	}

				/* escape and special characters */

	if (c <= 0x1F && c != ESCAPE && c != '\b' && c != '\n' && c != FFEED) {
		putc(c, stdout);
		if (debug & DEBUGESCAPE)
			putc(c, logescape);
		escape = -1;
		new = 1;
		utf8pos = 0;
		return;
	}

	if (escape == -1 && c == ESCAPE) {
		escape = 0;
		new = 1;
	}

	if (escape >= 0) {
		putc(c, stdout);
		if (escape >= SEQUENCELEN - 1) {
			escape = -1;
			return;
		}
		sequence[escape++] = c;
		if (c == '[' && escape - 1 == 1)
			return;
		if (c < 0x40 || c > 0x7F)
			return;
		sequence[escape] = '\0';
		if (debug & DEBUGESCAPE)
			fprintf(logescape, "<%s>", sequence);
		if (! strcmp(sequence, ERASEDISPLAY))
			for (i = 0; i < winsize->ws_row * winsize->ws_col; i++)
				buffer[(origin + i) % BUFFERSIZE] = ' ';
		escape = -1;
		return;
	}

					/* put character on vt */

	if (new) {
		askposition();
		new = 0;
	}
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
				fprintf(logescape, "[UTF8:%s]", sequence);
			w = utf8toucs4(utf8);
		}
	}

					/* update scrollback buffer */

	pos = (origin + row * winsize->ws_col + col) % BUFFERSIZE;
	if (c == '\b') {
		if (col > 0)
			col--;
		buffer[pos] = ' ';
	}
	else if (c == '\n')
		newrow(winsize);
	else if (c == FFEED)
		col = 0;
	else {
		buffer[pos] = w;
		if (col < winsize->ws_col - 1)
			col++;
		else {
			col = 0;
			newrow(winsize);
		}
	}
	if (debug & DEBUGESCAPE)
		fprintf(logescape, "[nextpos:%d,%d]", row, col);

	if (debug & DEBUGBUFFER) {
		lseek(logbuffer, 0, SEEK_SET);
		// fixme: do not read buffer over its end
		write(logbuffer, buffer, sizeof(u_int32_t) * BUFFERSIZE);
	}
}

/*
 * process a character from the terminal
 */
int special = -1;
char specialsequence[SEQUENCELEN];
void terminaltoprogram(int master, unsigned char c, int next,
		const struct winsize *winsize) {
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

		size = lines * winsize->ws_col;
		if (! strcmp(specialsequence, scrollup)) {
			pos = show - size;
			if (pos < 0)
				pos = 0;
			all = winsize->ws_row * winsize->ws_col;
			if (origin - pos > BUFFERSIZE - all) {
				rows = (BUFFERSIZE - all) / winsize->ws_col;
				pos = origin - rows * winsize->ws_col;
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
 * parent
 */
void parent(int master, pid_t pid, const struct winsize *winsize) {
	fd_set sin;
	char buf[1024];
	int len, i;
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
	askposition();
	origin = 0;
	show = 0;

	FD_ZERO(&sin);
	FD_SET(STDIN_FILENO, &sin);
	FD_SET(master, &sin);

	while (0 < select(master + 1, &sin, NULL, NULL, NULL)) {
		if (FD_ISSET(STDIN_FILENO, &sin)) {
			len = read(STDOUT_FILENO, &buf, 1024);
			if (len == -1)
				break;
			if (debug & DEBUGESCAPE)
				fprintf(logescape, "\nin[%d](", len);
			for (i = 0; i < len; i++)
				terminaltoprogram(master, buf[i], i < len - 1,
					winsize);
			if (debug & DEBUGESCAPE)
				fprintf(logescape, ")\n");
		}
		if (FD_ISSET(master, &sin)) {
			len = read(master, &buf, 1024);
			if (len == -1)
				break;
			for (i = 0; i < len; i++)
				programtoterminal(buf[i], winsize);
			fflush(stdout);
		}
		FD_ZERO(&sin);
		FD_SET(STDIN_FILENO, &sin);
		FD_SET(master, &sin);
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
	struct winsize full;
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

	res = ioctl(1, TIOCGWINSZ, &full);
	if (res == -1) {
		printf("not a linux terminal, not running\n");
		exit(EXIT_FAILURE);
	}
	lines = full.ws_row / 2;

					/* scroll keys */

	res = scrollkeys();
	if (res) {
		printf("cannot determine scoll keys\n");
		exit(EXIT_FAILURE);
	}

					/* program name */

	if (argn - 1 < 1) {
		printf("usage: %s /path/to/shell\n", argv[0]);
		exit(EXIT_FAILURE);
	}

					/* pty fork */

	p = forkpty(&master, NULL, NULL, &full);
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

	parent(master, p, &full);
	wait(NULL);
	system("reset -I");
	return EXIT_SUCCESS;
}

