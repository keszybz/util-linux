/*
 * blkzone.c -- the block device zone commands
 *
 * Copyright (C) 2015,2016 Seagate Technology PLC
 * Written by Shaun Tancheff <shaun.tancheff@seagate.com>
 *
 * Copyright (C) 2017 Karel Zak <kzak@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <limits.h>
#include <getopt.h>
#include <time.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/fs.h>
#include <linux/blkzoned.h>

#include "nls.h"
#include "strutils.h"
#include "xalloc.h"
#include "c.h"
#include "closestream.h"
#include "blkdev.h"
#include "sysfs.h"

struct blkzone_control;

static int blkzone_report(struct blkzone_control *ctl);
static int blkzone_reset(struct blkzone_control *ctl);

struct blkzone_command {
	const char *name;
	int (*handler)(struct blkzone_control *);
	const char *help;
};

struct blkzone_control {
	const char *devname;
	const struct blkzone_command *command;

	uint64_t total_sectors;
	int secsize;

	uint64_t offset;
	uint64_t length;

	unsigned int verbose : 1;
};

static const struct blkzone_command commands[] = {
	{ "report",	blkzone_report, N_("Report zone information about the given device") },
	{ "reset",	blkzone_reset,  N_("Reset a range of zones.") }
};

static const struct blkzone_command *name_to_command(const char *name)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		if (strcmp(commands[i].name, name) == 0)
			return &commands[i];
	}

	return NULL;
}

static int init_device(struct blkzone_control *ctl, int mode)
{
	struct stat sb;
	int fd;

	fd = open(ctl->devname, mode);
	if (fd < 0)
		err(EXIT_FAILURE, _("cannot open %s"), ctl->devname);

	if (fstat(fd, &sb) == -1)
		err(EXIT_FAILURE, _("stat of %s failed"), ctl->devname);
	if (!S_ISBLK(sb.st_mode))
		errx(EXIT_FAILURE, _("%s: not a block device"), ctl->devname);

	if (blkdev_get_sectors(fd, (unsigned long long *) &ctl->total_sectors))
		err(EXIT_FAILURE, _("%s: blkdev_get_sectors ioctl failed"), ctl->devname);

	if (blkdev_get_sector_size(fd, &ctl->secsize))
		err(EXIT_FAILURE, _("%s: BLKSSZGET ioctl failed"), ctl->devname);

	return fd;
}

/*
 * blkzone report
 */
#define DEF_REPORT_LEN		(1 << 12) /* 4k zones (256k kzalloc) */
#define MAX_REPORT_LEN		(1 << 16) /* 64k zones */

static const char *type_text[] = {
	"RESERVED",
	"CONVENTIONAL",
	"SEQ_WRITE_REQUIRED",
	"SEQ_WRITE_PREFERRED",
};

static const char *condition_str[] = {
	"cv", /* conventional zone */
	"e0", /* empty */
	"Oi", /* open implicit */
	"Oe", /* open explicit */
	"Cl", /* closed */
	"x5", "x6", "x7", "x8", "x9", "xA", "xB", /* xN: reserved */
	"ro", /* read only */
	"fu", /* full */
	"OL"  /* offline */
};

static int blkzone_report(struct blkzone_control *ctl)
{
	struct blk_zone_report *zi;
	uint32_t i;
	int fd;

	fd = init_device(ctl, O_RDONLY);

	if (ctl->offset > ctl->total_sectors)
		errx(EXIT_FAILURE, _("%s: offset is greater than device size"), ctl->devname);
	if (ctl->length < 1)
		ctl->length = 1;
	if (ctl->length > MAX_REPORT_LEN) {
		ctl->length = MAX_REPORT_LEN;
		warnx(_("limiting report to %" PRIu64 " entries"), ctl->length);
	}

	zi = xmalloc(sizeof(struct blk_zone_report)
		     + (ctl->length * sizeof(struct blk_zone)));
	zi->nr_zones = ctl->length;
	zi->sector = ctl->offset;		/* maybe shift 4Kn -> 512e */

	if (ioctl(fd, BLKREPORTZONE, zi) == -1)
		err(EXIT_FAILURE, _("%s: BLKREPORTZONE ioctl failed"), ctl->devname);

	if (ctl->verbose)
		printf(_("Found %d zones\n"), zi->nr_zones);

	printf(_("Zones returned: %u\n"), zi->nr_zones);

	for (i = 0; i < zi->nr_zones; i++) {
		const struct blk_zone *entry = &zi->zones[i];
		unsigned int type = entry->type;
		uint64_t start = entry->start;
		uint64_t wp = entry->wp;
		uint8_t cond = entry->cond;
		uint64_t len = entry->len;

		if (!len)
			break;

		printf(_("  start: %9lx, len %6lx, wptr %6lx"
			 " reset:%u non-seq:%u, zcond:%2u(%s) [type: %u(%s)]\n"),
			start, len, wp - start,
			entry->reset, entry->non_seq,
			cond, condition_str[cond & ARRAY_SIZE(condition_str)],
			type, type_text[type]);
	}

	free(zi);
	close(fd);

	return 0;
}

/*
 * blkzone reset
 */
static unsigned long blkdev_chunk_sectors(const char *dname)
{
	struct sysfs_cxt cxt = UL_SYSFSCXT_EMPTY;
	dev_t devno = sysfs_devname_to_devno(dname, NULL);
	int major_no = major(devno);
	int block_no = minor(devno) & ~0x0f;
	uint64_t sz;
	int rc;

	/*
	 * Mapping /dev/sdXn -> /sys/block/sdX to read the chunk_size entry.
	 * This method masks off the partition specified by the minor device
	 * component.
	 */
	devno = makedev(major_no, block_no);
	if (sysfs_init(&cxt, devno, NULL))
		return 0;

	rc = sysfs_read_u64(&cxt, "queue/chunk_sectors", &sz);

	sysfs_deinit(&cxt);
	return rc == 0 ? sz : 0;
}

static int blkzone_reset(struct blkzone_control *ctl)
{
	struct blk_zone_range za = { .sector = 0 };
	unsigned long zonesize;
	uint64_t zlen;
	int fd;

	zonesize = blkdev_chunk_sectors(ctl->devname);
	if (!zonesize)
		errx(EXIT_FAILURE, _("%s: unable to determine zone size"), ctl->devname);

	fd = init_device(ctl, O_WRONLY);

	if (ctl->offset & (zonesize - 1))
		errx(EXIT_FAILURE, _("%s: zone %" PRIu64 " is not aligned "
			"to zone size %" PRIu64),
			ctl->devname, ctl->offset, zonesize);

	if (ctl->offset > ctl->total_sectors)
		errx(EXIT_FAILURE, _("%s: offset is greater than device size"), ctl->devname);

	zlen = ctl->length * zonesize;

	if (ctl->offset + zlen > ctl->total_sectors)
		zlen = ctl->total_sectors - ctl->length;

	za.sector = ctl->offset;
	za.nr_sectors = zlen;

	if (ioctl(fd, BLKRESETZONE, &za) == -1)
		err(EXIT_FAILURE, _("%s: BLKRESETZONE ioctl failed"), ctl->devname);

	else if (ctl->verbose)
		printf(_("%s: successfully reset in range from %" PRIu64 ", to %" PRIu64),
			ctl->devname,
			ctl->offset,
			zlen);
	close(fd);
	return 0;
}

static void __attribute__((__noreturn__)) usage(FILE *out)
{
	size_t i;

	fputs(USAGE_HEADER, out);
	fprintf(out, _(" %s <command> [options] <device>\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Run zone command on the given block device.\n"), out);

	fputs(_("\nCommands:\n"), out);
	for (i = 0; i < ARRAY_SIZE(commands); i++)
		fprintf(out, " %-11s  %s\n", commands[i].name, _(commands[i].help));

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -o, --offset <sector>  start sector of zone to act (in 512-byte sectors)\n"), out);
	fputs(_(" -l, --length <number>  maximum number of zones\n"), out);
	fputs(_(" -v, --verbose          display more details\n"), out);
	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);

	fprintf(out, USAGE_MAN_TAIL("blkzone(8)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	int c;
	struct blkzone_control ctl = { .devname = NULL };

	static const struct option longopts[] = {
	    { "help",    no_argument,       NULL, 'h' },
	    { "length",  required_argument, NULL, 'l' }, /* max #of zones (entries) for result */
	    { "offset",  required_argument, NULL, 'o' }, /* starting LBA */
	    { "verbose", no_argument,       NULL, 'v' },
	    { "version", no_argument,       NULL, 'V' },
	    { NULL, 0, NULL, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	if (argc >= 2 && *argv[1] != '-') {
		ctl.command = name_to_command(argv[1]);
		if (!ctl.command)
			errx(EXIT_FAILURE, _("%s is not valid command name"), argv[1]);
		argv++;
		argc--;
	}

	while ((c = getopt_long(argc, argv, "hl:o:vV", longopts, NULL)) != -1) {
		switch (c) {
		case 'h':
			usage(stdout);
			break;
		case 'l':
			ctl.length = strtosize_or_err(optarg,
					_("failed to parse number of zones"));
			break;
		case 'o':
			ctl.offset = strtosize_or_err(optarg,
					_("failed to parse zone offset"));
			break;
		case 'v':
			ctl.verbose = 1;
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (!ctl.command)
		errx(EXIT_FAILURE, _("no command specified"));

	if (optind == argc)
		errx(EXIT_FAILURE, _("no device specified"));
	ctl.devname = argv[optind++];

	if (optind != argc)
		errx(EXIT_FAILURE,_("unexpected number of arguments"));

	if (ctl.command->handler(&ctl) < 0)
		return EXIT_FAILURE;

	return EXIT_SUCCESS;

}
