/*
 * memtester version 4
 *
 * Very simple but very effective user-space memory tester.
 * Originally by Simon Kirby <sim@stormix.com> <sim@neato.org>
 * Version 2 by Charles Cazabon <charlesc-memtester@pyropus.ca>
 * Version 3 not publicly released.
 * Version 4 rewrite:
 * Copyright (C) 2004-2012 Charles Cazabon <charlesc-memtester@pyropus.ca>
 * Licensed under the terms of the GNU General Public License version 2 (only).
 * See the file COPYING for details.
 *
 */

#define __version__ "4.3.0"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include "types.h"
#include "sizes.h"
#include "tests.h"

#define EXIT_FAIL_NONSTARTER    0x01
#define EXIT_FAIL_ADDRESSLINES  0x02
#define EXIT_FAIL_OTHERTEST     0x04

struct test tests[] = {

	{ "Random Value", test_random_value },
	{ "Compare XOR", test_xor_comparison },
	{ "Compare SUB", test_sub_comparison },
	{ "Compare MUL", test_mul_comparison },
	{ "Compare DIV",test_div_comparison },
	{ "Compare OR", test_or_comparison },
	{ "Compare AND", test_and_comparison },
	{ "Sequential Increment", test_seqinc_comparison },
	{ "Solid Bits", test_solidbits_comparison },
	{ "Block Sequential", test_blockseq_comparison },
	{ "Checkerboard", test_checkerboard_comparison },
	{ "Bit Spread", test_bitspread_comparison },
	{ "Bit Flip", test_bitflip_comparison },
	{ "Walking Ones", test_walkbits1_comparison },
	{ "Walking Zeroes", test_walkbits0_comparison },

#ifdef TEST_NARROW_WRITES    
	{ "8-bit Writes", test_8bit_wide_random },
	{ "16-bit Writes", test_16bit_wide_random },
#endif
	{ NULL, NULL }
};

#ifdef _SC_PAGE_SIZE
int memtester_pagesize(void) {
	int pagesize = sysconf(_SC_PAGE_SIZE);

	if (pagesize == -1) {
		perror("get page size failed");
		exit(EXIT_FAIL_NONSTARTER);
	}
	printf("pagesize is %ld\n", (long) pagesize);
	return pagesize;
}
#else
int memtester_pagesize(void) {
	printf("sysconf(_SC_PAGE_SIZE) not supported; using pagesize of 8192\n");
	return 8192;
}
#endif

/* Some systems don't define MAP_LOCKED.  Define it to 0 here
   so it's just a no-op when ORed with other constants. */
#ifndef MAP_LOCKED
  #define MAP_LOCKED 0
#endif

/* Global vars - so tests have access to this information */
int use_phys = 0;
off_t physaddrbase = 0;

int main(int argc, char **argv) {

	ul loops, loop, i;
	size_t pagesize, wantraw, wantmb, wantbytes, wantbytes_orig, bufsize,halflen, count;
	char *memsuffix, *addrsuffix, *loopsuffix;
	ptrdiff_t pagesizemask;
	void volatile *buf, *aligned;
	ulv *bufa, *bufb;
	int do_mlock = 1, done_mem = 0;
	int exit_code = 0;
	int memfd, opt, memshift;
	size_t maxbytes = -1; /* addressable memory, in bytes */
	size_t maxmb = (maxbytes >> 20) + 1; /* addressable memory, in MB */
	/* Device to mmap memory from with -p, default is normal core */
	char *device_name = "/dev/mem";
	struct stat statbuf;
	int device_specified = 0;
	char *env_testmask = 0;
	ul testmask = 0;
	char res[10];
	int mem_size;

	FILE *fp = popen("cat /proc/meminfo | grep MemFree | sed -r 's/[^0-9]+//g'", "r");
	fgets(res, 10, fp);
	fputs(res,stdout);
	mem_size=atoi(res)-20480;
	printf("\nsize=%d\n",mem_size);
	pclose(fp);

	pagesize = memtester_pagesize();
	pagesizemask = (ptrdiff_t) ~(pagesize - 1);

	/* If MEMTESTER_TEST_MASK is set, we use its value as a mask of which
	* tests we run.
	*/
	if (env_testmask = getenv("MEMTESTER_TEST_MASK")) {
		errno = 0;
		testmask = strtoul(env_testmask, 0, 0);
		if (errno) {
			fprintf(stderr, "error parsing MEMTESTER_TEST_MASK %s: %s\n",
			env_testmask, strerror(errno));
		}
		printf("using testmask 0x%lx\n", testmask);
	}

	wantbytes_orig = wantbytes =mem_size << 10;
	wantmb = mem_size >> 10;

	printf("want %lluMB (%llu bytes)\n", (ull) wantmb, (ull) wantbytes);
	buf = NULL;

	while (!done_mem) {

		while (!buf && wantbytes) {
			buf = (void volatile *) malloc(wantbytes);
			if (!buf) wantbytes -= pagesize;
		}

		bufsize = wantbytes;
		printf("got  %lluMB (%llu bytes)", (ull) wantbytes >> 20,(ull) wantbytes);
		fflush(stdout);
		if (do_mlock) {
			printf(", trying mlock ...");
			fflush(stdout);
			if ((size_t) buf % pagesize) {
				aligned = (void volatile *) ((size_t) buf & pagesizemask) + pagesize;
				bufsize -= ((size_t) aligned - (size_t) buf);
			} else {
				aligned = buf;
			}
			/* Try mlock */
			if (mlock((void *) aligned, bufsize) < 0) {
				switch(errno) {

				    case EAGAIN: /* BSDs */
				        printf("over system/pre-process limit, reducing...\n");
				        free((void *) buf);
				        buf = NULL;
				        wantbytes -= pagesize;
				        break;
				    case ENOMEM:
				        printf("too many pages, reducing...\n");
				        free((void *) buf);
				        buf = NULL;
				        wantbytes -= pagesize;
				        break;
				    case EPERM:
				        printf("insufficient permission.\n");
				        printf("Trying again, unlocked:\n");
				        do_mlock = 0;
				        free((void *) buf);
				        buf = NULL;
				        wantbytes = wantbytes_orig;
				        break;
				    default:
				        printf("failed for unknown reason.\n");
				        do_mlock = 0;
				        done_mem = 1;
				}

			} else {
				printf("locked.\n");
				done_mem = 1;
			}
		} else {
			done_mem = 1;
			printf("\n");
		}
	}

	if (!do_mlock) fprintf(stderr, "Continuing with unlocked memory\n");

	halflen = bufsize / 2;
	count = halflen / sizeof(ul);
	bufa = (ulv *) aligned;
	bufb = (ulv *) ((size_t) aligned + halflen);

        printf("  %-20s: ", "Stuck Address");
        fflush(stdout);
	if (!test_stuck_address(aligned, bufsize / sizeof(ul))) {
		printf("ok\n");
	} else {
		exit_code |= EXIT_FAIL_ADDRESSLINES;
		printf("err\n");
		return -1;
        }
        for (i=0;i<0;i++) {
		if (!tests[i].name) break;

		if (testmask && (!((1 << i) & testmask))) {
			continue;
		}
		printf("  %-20s: ", tests[i].name);
		if (!tests[i].fp(bufa, bufb, count)) {
			printf("ok\n");
		} else {
			exit_code |= EXIT_FAIL_OTHERTEST;
		}
		fflush(stdout);
        }
        printf("\n");
        fflush(stdout);

	if (do_mlock) munlock((void *) aligned, bufsize);
	printf("Done.\n");
	fflush(stdout);
	exit(exit_code);
}
