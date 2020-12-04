/*
 * vtwrapper.c
 *
 * run a program with VT_FILENO as the standard input, output and error
 *
 * example: vtwrapper ls -l /proc/self/fd
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>

#define VTNAME "VT_FILENO"

/*
 * main
 */
int main(int argn, char *argv[]) {
	struct termios st;
	char *vtenv;
	int vt;

	if (argn - 1 < 1) {
		printf("program to run missing\n");
		exit(EXIT_FAILURE);
	}

	vtenv = getenv(VTNAME);
	if (vtenv == NULL) {
		printf("no environment variable %s\n", VTNAME);
		exit(EXIT_FAILURE);
	}

	vt = atoi(vtenv);
	if (vt < 3) {
		printf("vt filenumber is %d, less than 3\n", vt);
		exit(EXIT_FAILURE);
	}

	dup2(vt, 0);
	dup2(vt, 1);
	dup2(vt, 2);
	close(vt);

	tcgetattr(0, &st);
	st.c_iflag |= (ICRNL);
	st.c_oflag |= (OPOST);
	tcsetattr(0, TCSADRAIN, &st);

	execvp(argv[1], argv + 1);
	perror(argv[1]);
	exit(EXIT_FAILURE);
}

