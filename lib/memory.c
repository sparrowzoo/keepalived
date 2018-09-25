/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Memory management framework. This framework is used to
 *              find any memory leak.
 *
 * Authors:     Alexandre Cassen, <acassen@linux-vs.org>
 *              Jan Holmberg, <jan@artech.net>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#ifdef _MEM_CHECK_
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#endif

#include <errno.h>
#include <string.h>

#include "memory.h"
#include "utils.h"
#include "bitops.h"
#include "logger.h"

#ifdef _MEM_CHECK_
#include "timer.h"

/* Global var */
size_t mem_allocated;		/* Total memory used in Bytes */
size_t max_mem_allocated;	/* Maximum memory used in Bytes */

const char *terminate_banner;	/* banner string for report file */

static bool skip_mem_check_final;
#endif

static void *
xalloc(unsigned long size)
{
	void *mem = malloc(size);

	if (mem == NULL) {
		if (__test_bit(DONT_FORK_BIT, &debug))
			perror("Keepalived");
		else
			log_message(LOG_INFO, "Keepalived xalloc() error - %s", strerror(errno));
		exit(EXIT_FAILURE);
	}

#ifdef _MEM_CHECK_
	mem_allocated += size - sizeof(long);
	if (mem_allocated > max_mem_allocated)
		max_mem_allocated = mem_allocated;
#endif

	return mem;
}

void *
zalloc(unsigned long size)
{
	void *mem = xalloc(size);

	if (mem)
		memset(mem, 0, size);

	return mem;
}

/* KeepAlived memory management. in debug mode,
 * help finding eventual memory leak.
 * Allocation memory types manipulated are :
 *
 * +-type------------------+-meaning------------------+
 * ! FREE_SLOT             ! Free slot                !
 * ! OVERRUN               ! Overrun                  !
 * ! FREE_NULL             ! free null                !
 * ! REALLOC_NULL          ! realloc null             !
 * ! FREE_NOT_ALLOC        ! Not previously allocated !
 * ! REALLOC_NOT_ALLOC     ! Not previously allocated !
 * ! LAST_FREE             ! Last free list           !
 * ! ALLOCATED             ! Allocated                !
 * +-----------------------+--------------------------+
 *
 * global variable debug bit MEM_ERR_DETECT_BIT used to
 * flag some memory error.
 *
 */

#ifdef _MEM_CHECK_

enum slot_type {
	FREE_SLOT,
	OVERRUN,
	FREE_NULL,
	REALLOC_NULL,
	FREE_NOT_ALLOC,
	REALLOC_NOT_ALLOC,
	LAST_FREE,
	ALLOCATED,
} ;

#define TIME_STR_LEN	9

#if ULONG_MAX == 0xffffffffffffffffUL
#define CHECK_VAL	0xa5a55a5aa5a55a5aUL
#elif ULONG_MAX == 0xffffffffUL
#define CHECK_VAL	0xa5a55a5aUL
#else
#define CHECK_VAL	0xa5a5
#endif

/* Max used for 1000 VRRP instance each with VMAC interfaces is 33589 */
#define MAX_ALLOC_LIST 2048*4*4 *2

#define FREE_LIST_SIZE	256

typedef struct {
	enum slot_type type;
	int line;
	char *func;
	char *file;
	void *ptr;
	size_t size;
} MEMCHECK;

/* Last free pointers */
static MEMCHECK free_list[FREE_LIST_SIZE];

static MEMCHECK alloc_list[MAX_ALLOC_LIST];
static int number_alloc_list = 0;
static int n = 0;		/* Alloc list pointer */
static int f = 0;		/* Free list pointer */

static FILE *log_op = NULL;

static const char *
format_time(void)
{
	static char time_buf[TIME_STR_LEN+1];

	strftime(time_buf, sizeof time_buf, "%T ", localtime(&time_now.tv_sec));

	return time_buf;
}

void
memcheck_log(const char *called_func, const char *param, const char *file, const char *function, int line)
{
	int len = strlen(called_func) + (param ? strlen(param) : 0);

	if ((len = 36 - len) < 0)
		len = 0;

	fprintf(log_op, "%s%*s%s(%s) at %s, %d, %s\n",
	       format_time(), len, "", called_func, param ? param : "", file, line, function);
}

void *
keepalived_malloc(size_t size, char *file, char *function, int line)
{
	void *buf;
	int i;

	buf = zalloc(size + sizeof (unsigned long));

	*(unsigned long *) ((char *) buf + size) = size + CHECK_VAL;

	for (i = 0; i < number_alloc_list; i++) {
		if (alloc_list[i].type == FREE_SLOT)
			break;
	}

	if (i == number_alloc_list)
		number_alloc_list++;

	if (number_alloc_list >= MAX_ALLOC_LIST) {
		log_message(LOG_INFO, "number_alloc_list = %d exceeds MAX_ALLOC_LIST(%u). Please increase value in lib/memory.c", number_alloc_list, MAX_ALLOC_LIST);
		assert(number_alloc_list < MAX_ALLOC_LIST);
	}

	alloc_list[i].ptr = buf;
	alloc_list[i].size = size;
	alloc_list[i].file = file;
	alloc_list[i].func = function;
	alloc_list[i].line = line;
	alloc_list[i].type = ALLOCATED;

	fprintf(log_op, "%szalloc [%3d:%3d], %p, %4zu at %s, %3d, %s\n",
	       format_time(), i, number_alloc_list, buf, size, file, line, function);
#ifdef _MEM_CHECK_LOG_
	if (__test_bit(MEM_CHECK_LOG_BIT, &debug))
		log_message(LOG_INFO, "zalloc[%3d:%3d], %p, %4zu at %s, %3d, %s",
		       i, number_alloc_list, buf, size, file, line, function);
#endif

	n++;
	return buf;
}

int
keepalived_free(void *buffer, char *file, char *function, int line)
{
	int i = 0;
	void *buf = buffer;
	unsigned long check;

	/* If nullpointer remember */
	if (buffer == NULL) {
		i = number_alloc_list++;

		assert(number_alloc_list < MAX_ALLOC_LIST);

		alloc_list[i].ptr = buffer;
		alloc_list[i].size = 0;
		alloc_list[i].file = file;
		alloc_list[i].func = function;
		alloc_list[i].line = line;
		alloc_list[i].type = FREE_NULL;
		fprintf(log_op, "%sfree NULL in %s, %3d, %s\n", format_time(), file,
		       line, function);

		__set_bit(MEM_ERR_DETECT_BIT, &debug);	/* Memory Error detect */

		return n;
	}

	while (i < number_alloc_list) {
		if (alloc_list[i].type == ALLOCATED && alloc_list[i].ptr == buf) {
			check = alloc_list[i].size + CHECK_VAL;
			if (*((unsigned long *) ((char *) alloc_list[i].ptr + alloc_list[i].size)) == check) {
				alloc_list[i].type = FREE_SLOT;
				mem_allocated -= alloc_list[i].size;
			} else {
				alloc_list[i].type = OVERRUN;
				fprintf(log_op, "%sfree corrupt, buffer overrun [%3d:%3d], %p, %4zu at %s, %3d, %s\n",
				       format_time(), i, number_alloc_list,
				       buf, alloc_list[i].size, file,
				       line, function);
				dump_buffer(alloc_list[i].ptr,
					    alloc_list[i].size + sizeof (check), log_op, TIME_STR_LEN);
				fprintf(log_op, "%*sCheck_sum\n", TIME_STR_LEN, "");
				dump_buffer((char *) &check,
					    sizeof(check), log_op, TIME_STR_LEN);

				__set_bit(MEM_ERR_DETECT_BIT, &debug);
			}
			break;
		}
		i++;
	}

	/*  Not found */
	if (i == number_alloc_list) {
		fprintf(log_op, "%sFree ERROR %p not found\n", format_time(), buffer);
		number_alloc_list++;

		assert(number_alloc_list < MAX_ALLOC_LIST);

		alloc_list[i].ptr = buf;
		alloc_list[i].size = 0;
		alloc_list[i].file = file;
		alloc_list[i].func = function;
		alloc_list[i].line = line;
		alloc_list[i].type = FREE_NOT_ALLOC;
		__set_bit(MEM_ERR_DETECT_BIT, &debug);

		return n;
	}

	fprintf(log_op, "%sfree   [%3d:%3d], %p, %4zu at %s, %3d, %s\n",
	       format_time(), i, number_alloc_list, buf,
	       alloc_list[i].size, file, line, function);
#ifdef _MEM_CHECK_LOG_
	if (__test_bit(MEM_CHECK_LOG_BIT, &debug))
		log_message(LOG_INFO, "free  [%3d:%3d], %p, %4zu at %s, %3d, %s",
		       i, number_alloc_list, buf,
		       alloc_list[i].size, file, line, function);
#endif

	if (buffer != NULL)
		free(buffer);

	free_list[f].file = file;
	free_list[f].line = line;
	free_list[f].func = function;
	free_list[f].ptr = buffer;
	free_list[f].type = LAST_FREE;
	free_list[f].size = i;	/* Using this field for row id */

	f++;
	f %= FREE_LIST_SIZE;
	n--;

	return n;
}

static void
keepalived_alloc_log(bool final)
{
	unsigned int overrun = 0, badptr = 0;
	size_t sum = 0;
	int i, j;
	i = 0;

	if (final) {
		/* If this is a forked child, we don't want the dump */
		if (skip_mem_check_final)
			return;

		fprintf(log_op, "\n---[ Keepalived memory dump for (%s) ]---\n\n", terminate_banner);
	}
	else
		fprintf(log_op, "\n---[ Keepalived memory dump for (%s) at %s ]---\n\n", terminate_banner, format_time());

	while (i < number_alloc_list) {
		switch (alloc_list[i].type) {
		case REALLOC_NULL:
			badptr++;
			fprintf
			    (log_op, "null pointer to realloc(nil,%zu)! at %s, %3d, %s\n",
			     alloc_list[i].size, alloc_list[i].file,
			     alloc_list[i].line, alloc_list[i].func);
			break;
		case FREE_NOT_ALLOC:
		case REALLOC_NOT_ALLOC:
			badptr++;
			if (alloc_list[i].type == FREE_NOT_ALLOC)
				fprintf
				    (log_op, "pointer not found in table to free(%p) [%3d:%3d], at %s, %3d, %s\n",
				     alloc_list[i].ptr, i, number_alloc_list,
				     alloc_list[i].file, alloc_list[i].line,
				     alloc_list[i].func);
			else
				fprintf
				    (log_op, "pointer not found in table to realloc(%p) [%3d:%3d] %4zu, at %s, %3d, %s\n",
				     alloc_list[i].ptr, i, number_alloc_list,
				     alloc_list[i].size, alloc_list[i].file,
				     alloc_list[i].line, alloc_list[i].func);
			for (j = 0; j < FREE_LIST_SIZE; j++)
				if (free_list[j].ptr == alloc_list[i].ptr)
					if (free_list[j].type == LAST_FREE)
						fprintf
						    (log_op, "  -> pointer already released at [%3d:%3d], at %s, %3d, %s\n",
						     (int) free_list[j].size,
						     number_alloc_list,
						     free_list[j].file,
						     free_list[j].line,
						     free_list[j].func);
			break;
		case FREE_NULL:
			badptr++;
			fprintf(log_op, "null pointer to free(nil)! at %s, %3d, %s\n",
			       alloc_list[i].file, alloc_list[i].line,
			       alloc_list[i].func);
			break;
		case OVERRUN:
			overrun++;
			fprintf(log_op, "%p [%3d:%3d], %4zu buffer overrun!:\n",
			       alloc_list[i].ptr, i, number_alloc_list,
			       alloc_list[i].size);
			fprintf(log_op, " --> source of malloc: %s, %3d, %s\n",
			       alloc_list[i].file, alloc_list[i].line,
			       alloc_list[i].func);
			break;
		case ALLOCATED:
			sum += alloc_list[i].size;
			fprintf(log_op, "%p [%3d:%3d], %4zu %s:\n",
			       alloc_list[i].ptr, i, number_alloc_list,
			       alloc_list[i].size,
			       final ? "not released!" : "currently_allocated");
			fprintf(log_op, " --> source of malloc: %s, %3d, %s\n",
			       alloc_list[i].file, alloc_list[i].line,
			       alloc_list[i].func);
			break;
		case FREE_SLOT:	/* not used - avoid compiler warning */
		case LAST_FREE:
			break;
		}
		i++;
	}

	fprintf(log_op, "\n\n---[ Keepalived memory dump summary for (%s) ]---\n", terminate_banner);
	fprintf(log_op, "Total number of bytes %s...: %zu\n", final ? "not freed" : "allocated", sum);
	fprintf(log_op, "Number of entries %s.......: %d\n", final ? "not freed" : "allocated", n);
	fprintf(log_op, "Maximum allocated entries.........: %d\n", number_alloc_list);
	fprintf(log_op, "Maximum memory allocated..........: %zu\n", max_mem_allocated);
	fprintf(log_op, "Number of bad entries.............: %d\n", badptr);
	fprintf(log_op, "Number of buffer overrun..........: %d\n\n", overrun);
	if (sum != mem_allocated)
		fprintf(log_op, "ERROR - sum of allocated %zu != mem_allocated %zu\n", sum, mem_allocated);

	if (final) {
		if (sum || n || badptr || overrun)
			fprintf(log_op, "=> Program seems to have some memory problem !!!\n\n");
		else
			fprintf(log_op, "=> Program seems to be memory allocation safe...\n\n");
	}
}

static void
keepalived_free_final(void)
{
	keepalived_alloc_log(true);
}

void
keepalived_alloc_dump(void)
{
	keepalived_alloc_log(false);
}

void *
keepalived_realloc(void *buffer, size_t size, char *file, char *function,
		   int line)
{
	int i;
	void *buf = buffer;

	if (buffer == NULL) {
		fprintf(log_op, "%srealloc %p %s, %3d %s\n", format_time(), buffer, file, line, function);
		i = number_alloc_list++;

		assert(number_alloc_list < MAX_ALLOC_LIST);

		alloc_list[i].ptr = NULL;
		alloc_list[i].size = 0;
		alloc_list[i].file = file;
		alloc_list[i].func = function;
		alloc_list[i].line = line;
		alloc_list[i].type = REALLOC_NULL;
		return keepalived_malloc(size, file, function, line);
	}

	for (i = 0; i < number_alloc_list; i++) {
		if (alloc_list[i].ptr == buf) {
			buf = alloc_list[i].ptr;
			break;
		}
	}

	/* not found */
	if (i == number_alloc_list) {
		fprintf(log_op, "%srealloc ERROR no matching zalloc %p \n", format_time(), buffer);
		number_alloc_list++;

		assert(number_alloc_list < MAX_ALLOC_LIST);

		alloc_list[i].ptr = buf;
		alloc_list[i].size = size;
		alloc_list[i].file = file;
		alloc_list[i].func = function;
		alloc_list[i].line = line;
		alloc_list[i].type = REALLOC_NOT_ALLOC;
		__set_bit(MEM_ERR_DETECT_BIT, &debug);	/* Memory Error detect */
		return NULL;
	}

	mem_allocated -= alloc_list[i].size;

	if (*(unsigned long *) (((char *) buf) + alloc_list[i].size) != alloc_list[i].size + CHECK_VAL) {
		alloc_list[i].type = OVERRUN;
		__set_bit(MEM_ERR_DETECT_BIT, &debug);	/* Memory Error detect */
	}

	buf = realloc(buffer, size + sizeof (unsigned long));

	mem_allocated += size;
	if (mem_allocated > max_mem_allocated)
		max_mem_allocated = mem_allocated;

	*(unsigned long *) ((char *) buf + size) = size + CHECK_VAL;

	fprintf(log_op, "%srealloc[%3d:%3d], %p, %4zu at %s, %3d, %s -> %p, %4zu at %s, %3d, %s\n",
	       format_time(), i, number_alloc_list, alloc_list[i].ptr,
	       alloc_list[i].size, alloc_list[i].file,
	       alloc_list[i].line, alloc_list[i].func,
	       buf, size, file, line, function);

	alloc_list[i].ptr = buf;
	alloc_list[i].size = size;
	alloc_list[i].file = file;
	alloc_list[i].line = line;
	alloc_list[i].func = function;

	return buf;
}

void
mem_log_init(const char* prog_name, const char *banner)
{
	size_t log_name_len;
	char *log_name;

	if (__test_bit(LOG_CONSOLE_BIT, &debug)) {
		log_op = stderr;
		return;
	}

	if (log_op)
		fclose(log_op);

	log_name_len = 5 + strlen(prog_name) + 5 + 7 + 4 + 1;	/* "/tmp/" + prog_name + "_mem." + PID + ".log" + '\0" */
	log_name = malloc(log_name_len);
	if (!log_name) {
		log_message(LOG_INFO, "Unable to malloc log file name");
		log_op = stderr;
		return;
	}

	snprintf(log_name, log_name_len, "/tmp/%s_mem.%d.log", prog_name, getpid());
	log_op = fopen(log_name, "a");
	if (log_op == NULL) {
		log_message(LOG_INFO, "Unable to open %s for appending", log_name);
		log_op = stderr;
	}
	else {
		int fd = fileno(log_op);

		/* We don't want any children to inherit the log file */
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

		/* Make the log output line buffered. This was to ensure that
		 * children didn't inherit the buffer, but the CLOEXEC above
		 * should resolve that. */
		setlinebuf(log_op);

		fprintf(log_op, "\n");
	}

	free(log_name);

	terminate_banner = banner;
}

void skip_mem_dump(void)
{
	skip_mem_check_final = true;
}

void enable_mem_log_termination(void)
{
	atexit(keepalived_free_final);
}
#endif
