/*
 * scrollback.c
 *
 * simple scrollback buffer for a virtual terminal
 *
 * tbd: option for the scroll control strings: scrollup, scrolldown
 * tbd: option for the scrollup/scrolldown keycodes: keycodeup, keycodedown
 * tbd: colors
 * tbd: option for the number of lines to scroll (lines)
 * tbd: when scrolling, also use up/down for scrolling 1 line
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
 * void parent(int master, pid_t pid);
 *	the main loop; calls exchange() repeatedly to pass data between the
 *	terminal and the shell in both directions with no timeout
 *
 * int exchange(int master, int readshell, struct timeval *timeout);
 *	process a single data block from the terminal or the shell; parameters
 *	readshell and timeout tell whether to read from the shell and whether
 *	to timeout reading; data from the terminal is forwarded to the shell
 *	except the cursor position answers
 *
 * void knowposition(int master, int alreadyasked);
 *	called whenever the cursor position is needed; ask the terminal and
 *	wait for an answer; the latter is done by calling exchange() to only
 *	receive data from the terminal with a timeout
 *
 * int positionstatus
 *	whether the cursor position is known
 *
 * the cursor position is needed in two cases: first, the shell sent some
 * escape sequences and now wants to print a character; second, the shell sent
 * a ESC[6n to determine the cursor position
 *
 * - in the first case, knownposition() is called with alreadyasked=0, which
 *   makes it ask the terminal via an ESC[6n command and wait for an answer
 *   while processing other data from the terminal as usual
 *
 * - in the second case, the ESC[6n command coming from the shell is forwarded
 *   to the terminal as every other data coming from the shell; knowposition()
 *   is called with alreadyasked=1 so that it does not ask the terminal the
 *   position; it still processes data coming from the terminal as usual until
 *   an answer is received; at that point, an answer is built and sent to the
 *   shell
 *
 * the timeout avoids freezing the shell if for some reason the terminal does
 * not answer the cursor position query at all
 *
 * the cursor position status may be known, unknow or uncertain; the latter is
 * when the terminal reported the cursor in the last column; it is ambigous
 * because the next character is to be placed either at the end of the row or
 * at the start of the next
 */

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <locale.h>
#include <pty.h>
#include <linux/kd.h>
#include <linux/keyboard.h>

/*
 * keys for scrolling
 */
int shift = K_SHIFTTAB;
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
int singlechar;

/*
 * log files
 */
#define DEBUGESCAPE 0x01
#define DEBUGBUFFER 0x02
#define DEBUGKEYS   0x04
int debug;

FILE *logescape;
FILE *logbuffer;

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
#define RESETATTRIBUTES       "\033[0m"
#define BLUEBACKGROUND        "\033[44m"
#define NORMALBACKGROUND      "\033[49m"
#define ERASECURSORDISPLAY    "\033[J"
#define ERASEDISPLAY          "\033[2J"
#define HOMEPOSITION          "\033[H"
#define SAVECURSOR            "\033[s"
#define RESTORECURSOR         "\033[u"

/*
 * print an escape sequence in readable form
 */
void printescape(FILE *fd, unsigned char *sequence) {
	int i;
	for (i = 0; sequence[i] != '\0'; i++)
		if (sequence[i] == ESCAPE)
			fprintf(fd, "ESC");
		else if (isprint(sequence[i]))
			fprintf(fd, "%c", sequence[i]);
		else
			fprintf(fd, "[0x%02X]", sequence[i]);
	fprintf(fd, "\n");
}

/*
 * set escape sequence for a key
 */
int setkey(int keycode, int shift, int func, char *keystring) {
	int tty;
	struct kbentry kbe;
	struct kbsentry kbse;
	int res;

	tty = open("/dev/tty1", O_RDWR);
	if (tty == -1) {
		perror("/dev/tty1");
		return tty;
	}

	kbse.kb_func = func;
	strncpy((char *) kbse.kb_string, keystring, 511);
	kbse.kb_string[511] = '\0';
	res = ioctl(tty, KDSKBSENT, &kbse);
	if (res != 0) {
		perror("KDSKBSENT");
		return res;
	}

	kbe.kb_table = shift;
	kbe.kb_index = keycode;
	kbe.kb_value = func;
	res = ioctl(tty, KDSKBENT, &kbe);
	if (res != 0) {
		perror("KDSKBENT");
		return res;
	}

	fprintf(stdout, "%s%d", shift == K_SHIFTTAB ? "shift-" : "", keycode);
	fprintf(stdout, " => ");
	printescape(stdout, (unsigned char*) keystring);

	return 0;
}

/*
 * set scrolling keys
 */
int setscrollkeys() {
	int res;

	res = setkey(keycodeup, shift, K_F99, KEYSHIFTPAGEUP);
	if (res != 0)
		return res;
	res = setkey(keycodedown, shift, K_F100, KEYSHIFTPAGEDOWN);
	if (res != 0)
		return res;

	return 0;
}

/*
 * key to function string
 */
int keytofunction(int keycode, int shift, char **keystring) {
	struct kbentry kbe;
	struct kbsentry kbse;
	int res;

	kbe.kb_table = shift;
	kbe.kb_index = keycode;
	res = ioctl(STDIN_FILENO, KDGKBENT, &kbe);
	if (res != 0)
		return -1;
	switch (kbe.kb_value) {
	case K_HOLE:
		if (debug & DEBUGKEYS)
			printf("K_HOLE\n");
		return 0;
	case K_NOSUCHMAP:
		if (debug & DEBUGKEYS)
			printf("K_NOSUCHMAP\n");
		return 0;
	case K_ALLOCATED:
		if (debug & DEBUGKEYS)
			printf("K_ALLOCATED\n");
		return 0;
	default:
		if (! (debug & DEBUGKEYS))
			break;
		printf("key %d: ", kbe.kb_value);
		printf("%d ",  KTYP(kbe.kb_value));
		printf("%d\n", KVAL(kbe.kb_value));
	}

	if (KTYP(kbe.kb_value) != KT_FN) {
		if (debug & DEBUGKEYS)
			printf("not a function key\n");
		return 0;
	}

	kbse.kb_func = KVAL(kbe.kb_value);
	if (res != 0)
		return -1;
	res = ioctl(STDIN_FILENO, KDGKBSENT, &kbse);
	if (debug & DEBUGKEYS)
		printescape(stdout, kbse.kb_string);

	*keystring = strdup((char *) kbse.kb_string);
	return 1;
}

/*
 * control codes for the scrolling keys
 */
int scrollkeys(int verbose) {
	int res;

	scrollup = KEYF11;
	scrolldown = KEYF12;

	res = keytofunction(keycodeup, shift, &scrollup);
	if (res == -1)
		return res;
	if (verbose)
		printf("scrollup is %s\n",
			res == 0 ? "F11" : "shift-pageup");

	res = keytofunction(keycodedown, shift, &scrolldown);
	if (res == -1)
		return res;
	if (verbose)
		printf("scrolldown is %s\n",
			res == 0 ? "F12" : "shift-pagedown");

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
int buffersize;
u_int32_t *buffer;
int origin;		/* start of region storing a copy of the screen */
int show;		/* start of region that is shown when scrolling */

/*
 * the screen
 */
struct winsize winsize;	/* screen size */
int row, col;		/* position of the cursor */
int positionstatus;	/* is the cursor position known? */
#define POSITION_UNKNOWN   0
#define POSITION_KNOWN     1
#define POSITION_UNCERTAIN 2

/*
 * show a segment of the scrollback buffer on screen
 */
#define BARUP   "            " BLUEBACKGROUND "↑↑↑↑↑↑↑↑↑" NORMALBACKGROUND
#define BARDOWN "            " BLUEBACKGROUND "↓↓↓↓↓↓↓↓↓" NORMALBACKGROUND
void showscrollback() {
	int size, all, rows, i;
	char buf[10];
	u_int32_t c, prev;

	size = (winsize.ws_row - (show == origin ? 0 : 2)) * winsize.ws_col;
	fprintf(stdout, HOMEPOSITION ERASEDISPLAY RESETATTRIBUTES);
	if (show != origin) {
		all = winsize.ws_row * winsize.ws_col;
		rows = (buffersize - all) / winsize.ws_col;
		if (show - winsize.ws_col >= 0 &&
		    rows > (origin - show) / winsize.ws_col)
			fprintf(stdout, BARUP);
		fprintf(stdout, "\r\n");
	}
	prev = 0;
	for (i = 0; i < size; i++) {
		c = buffer[(show + i) % buffersize];
		if (singlechar) {
			if (prev >= 0xC0 && c >= 0x80 && c < 0xC0)
				putc(DEL, stdout);
			putc(c, stdout);
		}
		else {
			ucs4toutf8(c, buf);
			fputs(buf, stdout);
		}
		prev = c;
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
void knowposition(int master, int alreadyasked) {
	struct timeval tv;
	int i;

	if (! alreadyasked && positionstatus == POSITION_KNOWN)
		return;

	if (debug & DEBUGESCAPE)
		fprintf(logescape, "[knowposition(%d)]", alreadyasked);

	if (! alreadyasked) {
		fprintf(stdout, ASKPOSITION);
		fflush(stdout);
	}

	positionstatus = POSITION_UNKNOWN;
	for (i = 0; i < 4 && positionstatus == POSITION_UNKNOWN; i++) {
		tv.tv_sec = 0;
		tv.tv_usec = 100000;
		exchange(master, 0, &tv);
		// fixme: if return is -1, terminate everything
	}
}

/*
 * erase part of the scrollback buffer
 */
void erase(int startrow, int startcol, int endcol) {
	int start, i;
	start = winsize.ws_col * startrow;
	for (i = start + startcol; i < start + endcol; i++)
		buffer[(origin + i) % buffersize] = ' ';
	start += winsize.ws_col;
	for (i = start; i < winsize.ws_row * winsize.ws_col; i++)
		buffer[(origin + i) % buffersize] = ' ';
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
 * process a character from the shell
 */
#define SEQUENCELEN 40
char sequence[SEQUENCELEN];
int escape = -1;
unsigned char utf8[SEQUENCELEN];
int utf8pos = 0, utf8len = 0;
void shelltoterminal(int master, unsigned char c) {
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
		positionstatus = POSITION_UNKNOWN;
		utf8pos = 0;
		return;
	}

	if (escape == -1 && c == ESCAPE)
		escape = 0;

	if (escape >= 0) {
		putc(c, stdout);
		if (escape >= SEQUENCELEN - 1) {
			escape = -1;
			positionstatus = POSITION_UNKNOWN;
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
			knowposition(master, 0);
			erase(row, col, winsize.ws_col);
		}
		else if (! strcmp(sequence, ASKPOSITION)) {
			fflush(stdout);
			knowposition(master, 1);
			sprintf(buf, ANSWERPOSITION, row + 1,
				col < winsize.ws_col ? col + 1 : col);
			write(master, buf, strlen(buf));
			if (debug & DEBUGESCAPE)
				fprintf(logescape, "fakein(%s)", buf);
			escape = -1;
			return;
		}
		escape = -1;
		if (sequence[1] != '[' || c != 'm')
			positionstatus = POSITION_UNKNOWN;
		return;
	}

					/* send character to terminal */

	if (utf8len == 0)
		knowposition(master, 0);
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

	pos = (origin + row * winsize.ws_col + col) % buffersize;
	if ((c == BS || c == DEL) && col > 0) {
		col--;
		buffer[(buffersize + pos - 1) % buffersize] = ' ';
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
		fseek(logbuffer, 0, SEEK_SET);
		fwrite(buffer, sizeof(u_int32_t), buffersize, logbuffer);
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
		positionstatus = x == winsize.ws_col ?
			POSITION_UNCERTAIN : POSITION_KNOWN;
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
void terminaltoshell(int master, unsigned char c, int next) {
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
			if (origin - pos > buffersize - all) {
				rows = (buffersize - all) / winsize.ws_col;
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
			terminaltoshell(master, buf[i], i < len - 1);
		if (debug & DEBUGESCAPE)
			fprintf(logescape, ")\n");
	}

	if (readshell && FD_ISSET(master, &sin)) {
		len = read(master, &buf, 1024);
		if (len == -1)
			return -1;
		for (i = 0; i < len; i++)
			shelltoterminal(master, buf[i]);
		fflush(stdout);
	}

	return 0;
}

/*
 * open a log file
 */
FILE *logopen(char *filename) {
	char logname[4096];
	FILE *ret;

	if (strstr(filename, "%d"))
		snprintf(logname, 4096, filename, getuid());
	else
		strncpy(logname, filename, 4096);
	ret = fopen(logname, "w");
	if (ret == NULL) {
		perror(logname);
		exit(EXIT_FAILURE);
	}

	return ret;
}

/*
 * parent: main loop
 */
void parent(int master, pid_t pid) {
	int i;

	(void) pid;

	if (debug & DEBUGESCAPE)
		logescape = logopen(LOGESCAPE);
	if (debug & DEBUGBUFFER)
		logbuffer = logopen(LOGBUFFER);

	nolinebuffering();
	buffer = malloc(sizeof(u_int32_t) * buffersize);
	for (i = 0; i < buffersize; i++)
		buffer[i] = ' ';
	origin = 0;
	show = 0;
	positionstatus = POSITION_UNKNOWN;

	while (exchange(master, 1, NULL) == 0) {
	}

	free(buffer);

	if (debug & DEBUGESCAPE)
		fclose(logescape);
	if (debug & DEBUGBUFFER)
		fclose(logbuffer);
}

/*
 * main
 */
int main(int argn, char *argv[]) {
	char *shell;
	int checkonly, keysonly, usage;
	int opt;
	char path[1024];
	char no[20];
	int master;
	pid_t p;
	int res;
	int tty;
	char t;
	struct termios st;

					/* arguments */

	buffersize = 8 * 1024;
	singlechar = -1;
	checkonly = 0;
	keysonly = 0;
	debug = 0;
	usage = 0;
	while (-1 != (opt = getopt(argn, argv, "b:usckd:h"))) {
		switch (opt) {
		case 'b':
			buffersize = atoi(optarg);
			break;
		case 'u':
			singlechar = 0;
			break;
		case 's':
			singlechar = 1;
			break;
		case 'c':
			checkonly = 1;
			break;
		case 'k':
			keysonly = 1;
			break;
		case 'd':
			debug = atoi(optarg);
			break;
		case 'h':
			usage = 1;
			break;
		default:
			usage = 2;
		}
	}
	if (! usage && ! keysonly && argn - 1 < optind) {
		printf("shell missing\n");
		usage = 2;
	}
	if (usage) {
		printf("usage:\n\t%s ", argv[0]);
		printf("[-u] [-s] [-c] [-k] [-d level] [-h] /path/to/shell\n");
		printf("\t\t-u\t\tterminal is in unicode mode\n");
		printf("\t\t-s\t\tterminal is not in unicode mode\n");
		printf("\t\t-c\t\tonly check whether it should run\n");
		printf("\t\t-k\t\tset up the keys for the subsequent calls\n");
		printf("\t\t-d level\tdebug level: 1=in/out 2=buffer\n");
		printf("\t\t-h\t\tthis help\n");
		exit(usage == 2 ? EXIT_FAILURE : EXIT_SUCCESS);
	}
	shell = argv[optind];

					/* singlechar mode */

	setlocale(LC_ALL, "");
	if (singlechar == -1)
		singlechar = MB_CUR_MAX == 1;

					/* setup keys */

	if (keysonly) {
		res = setkey(keycodeup, shift, K_F99, KEYSHIFTPAGEUP);
		if (res != 0)
			exit(EXIT_FAILURE);
		res = setkey(keycodedown, shift, K_F100, KEYSHIFTPAGEDOWN);
		if (res != 0)
			exit(EXIT_FAILURE);
		exit(EXIT_SUCCESS);
	}

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

	res = ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsize);
	if (res == -1) {
		printf("not a linux terminal, not running\n");
		exit(EXIT_FAILURE);
	}
	lines = winsize.ws_row / 2;
	if (buffersize < winsize.ws_row * winsize.ws_col) {
		printf("buffer too small: %d, ", buffersize);
		printf("should be greater than %d\n",
			winsize.ws_row * winsize.ws_col);
		exit(EXIT_FAILURE);
	}

					/* scroll keys */

	res = scrollkeys(! checkonly);
	if (res == -1) {
		printf("cannot determine scroll keys\n");
		exit(EXIT_FAILURE);
	}

					/* check only */

	if (checkonly)
		return EXIT_SUCCESS;

					/* pty fork */

	p = forkpty(&master, NULL, NULL, &winsize);
	if (p == -1) {
		perror("forkpty");
		return EXIT_FAILURE;
	}

	if (p == 0) {
		tcgetattr(0, &st);
		st.c_iflag &= ~(IGNBRK);
		st.c_iflag |= (BRKINT);
		tcsetattr(0, TCSADRAIN, &st);
		execvp(shell, argv + optind);
		perror(shell);
		return EXIT_FAILURE;
	}

	parent(master, p);
	wait(NULL);
	system("reset -I");
	return EXIT_SUCCESS;
}

