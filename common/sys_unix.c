/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <signal.h>
#include <sys/ipc.h>
#include <sys/mman.h>

#include "buildinfo.h"
#include "common.h"
#include "sys.h"
#include "zone.h"

#include "quakedef.h"

#include "client.h"
#include "host.h"

qboolean isDedicated;

static qboolean noconinput = false;
static qboolean nostdout = false;

/*
 * ===========================================================================
 * General Routines
 * ===========================================================================
 */

void
Sys_Printf(const char *fmt, ...)
{
    va_list argptr;
    char text[MAX_PRINTMSG];
    unsigned char *p;

    va_start(argptr, fmt);
    qvsnprintf(text, sizeof(text), fmt, argptr);
    va_end(argptr);

    if (nostdout)
	return;

    for (p = (unsigned char *)text; *p; p++) {
	if ((*p > 128 || *p < 32) && *p != 10 && *p != 13 && *p != 9)
	    printf("[%02x]", *p);
	else
	    putc(*p, stdout);
    }
}

void
Sys_Quit(void)
{
    Host_Shutdown();
    fcntl(STDIN_FILENO, F_SETFL,
	  fcntl(STDIN_FILENO, F_GETFL, 0) & ~O_NONBLOCK);
    fflush(stdout);
    exit(0);
}

void
Sys_RegisterVariables()
{
}

void
Sys_Init(void)
{
    Sys_SetFPCW();
}

void
Sys_Error(const char *error, ...)
{
    va_list argptr;
    char string[MAX_PRINTMSG];

    /* remove non-blocking flag from standard input */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);

    va_start(argptr, error);
    qvsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);
    fprintf(stderr, "Error: %s\n", string);

    Host_Shutdown();
    exit(1);
}

/*
============
Sys_FileTime

returns -1 if not present
============
*/
int64_t
Sys_FileTime(const char *path)
{
    struct stat buf;

    if (stat(path, &buf) == -1)
	return -1;

    return buf.st_mtime;
}

void
Sys_mkdir(const char *path)
{
    mkdir(path, 0777);
}

double
Sys_DoubleTime(void)
{
    struct timeval tp;
    struct timezone tzp;
    static int secbase;

    gettimeofday(&tp, &tzp);

    if (!secbase) {
	secbase = tp.tv_sec;
	return tp.tv_usec / 1000000.0;
    }

    return (tp.tv_sec - secbase) + tp.tv_usec / 1000000.0;
}

/*
================
Sys_ConsoleInput

Checks for a complete line of text typed in at the console, then forwards
it to the host command processor
================
*/
char *
Sys_ConsoleInput(void)
{
    static char text[256];
    int len;
    fd_set fdset;
    struct timeval timeout;

    if (cls.state != ca_dedicated)
	return NULL;

    FD_ZERO(&fdset);
    FD_SET(STDIN_FILENO, &fdset);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    if (select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout) == -1
	|| !FD_ISSET(STDIN_FILENO, &fdset))
	return NULL;
    len = read(STDIN_FILENO, text, sizeof(text));
    if (len < 1)
	return NULL;
    text[len - 1] = 0; /* remove the /n and terminate */

    return text;
}

void
Sys_Sleep(void)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;

    while (nanosleep(&ts, &ts) == -1)
	if (errno != EINTR)
	    break;
}

void
Sys_DebugLog(const char *file, const char *fmt, ...)
{
    va_list argptr;
    static char data[MAX_PRINTMSG];
    int fd;

    va_start(argptr, fmt);
    qvsnprintf(data, sizeof(data), fmt, argptr);
    va_end(argptr);
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0666);
    write(fd, data, strlen(data));
    close(fd);
}

void Sys_HighFPPrecision(void) {}
void Sys_LowFPPrecision(void) {}
void Sys_SetFPCW(void) {}

/*
================
Sys_MakeCodeWriteable
================
*/
void
Sys_MakeCodeWriteable(void *start_addr, void *end_addr)
{
}

/*
================
Sys_MakeCodeUnwriteable
================
*/
void
Sys_MakeCodeUnwriteable(void *start_addr, void *end_addr)
{
}


/*
 * ===========================================================================
 * Main
 * ===========================================================================
 */

int
main(int argc, char **argv)
{
    double time, oldtime, newtime;
    quakeparms_t parms;

    signal(SIGFPE, SIG_IGN);

    memset(&parms, 0, sizeof(parms));

    COM_InitArgv(argc, (const char **)argv);
    parms.argc = com_argc;
    parms.argv = com_argv;
    parms.basedir = stringify(QBASEDIR);
    parms.memsize = Memory_GetSize();
    parms.membase = malloc(parms.memsize);
    if (!parms.membase)
	Sys_Error("Allocation of %d byte heap failed", parms.memsize);

    if (COM_CheckParm("-noconinput"))
	noconinput = true;
    if (COM_CheckParm("-nostdout"))
	nostdout = true;

    // Make stdin non-blocking
    // FIXME - check both return values
    if (!noconinput)
	fcntl(STDIN_FILENO, F_SETFL,
	      fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
    if (!nostdout)
	printf("Quake -- TyrQuake Version %s\n", build_version);

    Sys_Init();
    Host_Init(&parms, NULL);

    /*
     * Main Loop
     */
    oldtime = Sys_DoubleTime() - 0.1;
    while (1) {

	/* find time passed since last cycle */
	newtime = Sys_DoubleTime();
	time = newtime - oldtime;

	if (cls.state == ca_dedicated) {
	    if (time < sys_ticrate.value) {
		usleep(1);
		continue;	// not time to run a server only tic yet
	    }
	    time = sys_ticrate.value;
	}
	if (time > sys_ticrate.value * 2)
	    oldtime = newtime;
	else
	    oldtime += time;

	Host_Frame(time);
    }

    return 0;
}
