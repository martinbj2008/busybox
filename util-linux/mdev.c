/* vi: set sw=4 ts=4: */
/*
 * mdev - Mini udev for busybox
 *
 * Copyright 2005 Rob Landley <rob@landley.net>
 * Copyright 2005 Frank Sorenson <frank@tuxrocks.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */

//config:config MDEV
//config:	bool "mdev"
//config:	default y
//config:	select PLATFORM_LINUX
//config:	help
//config:	  mdev is a mini-udev implementation for dynamically creating device
//config:	  nodes in the /dev directory.
//config:
//config:	  For more information, please see docs/mdev.txt
//config:
//config:config FEATURE_MDEV_CONF
//config:	bool "Support /etc/mdev.conf"
//config:	default y
//config:	depends on MDEV
//config:	help
//config:	  Add support for the mdev config file to control ownership and
//config:	  permissions of the device nodes.
//config:
//config:	  For more information, please see docs/mdev.txt
//config:
//config:config FEATURE_MDEV_RENAME
//config:	bool "Support subdirs/symlinks"
//config:	default y
//config:	depends on FEATURE_MDEV_CONF
//config:	help
//config:	  Add support for renaming devices and creating symlinks.
//config:
//config:	  For more information, please see docs/mdev.txt
//config:
//config:config FEATURE_MDEV_RENAME_REGEXP
//config:	bool "Support regular expressions substitutions when renaming device"
//config:	default y
//config:	depends on FEATURE_MDEV_RENAME
//config:	help
//config:	  Add support for regular expressions substitutions when renaming
//config:	  device.
//config:
//config:config FEATURE_MDEV_EXEC
//config:	bool "Support command execution at device addition/removal"
//config:	default y
//config:	depends on FEATURE_MDEV_CONF
//config:	help
//config:	  This adds support for an optional field to /etc/mdev.conf for
//config:	  executing commands when devices are created/removed.
//config:
//config:	  For more information, please see docs/mdev.txt
//config:
//config:config FEATURE_MDEV_LOAD_FIRMWARE
//config:	bool "Support loading of firmwares"
//config:	default y
//config:	depends on MDEV
//config:	help
//config:	  Some devices need to load firmware before they can be usable.
//config:
//config:	  These devices will request userspace look up the files in
//config:	  /lib/firmware/ and if it exists, send it to the kernel for
//config:	  loading into the hardware.

//applet:IF_MDEV(APPLET(mdev, BB_DIR_SBIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_MDEV) += mdev.o

//usage:#define mdev_trivial_usage
//usage:       "[-s]"
//usage:#define mdev_full_usage "\n\n"
//usage:       "mdev -s is to be run during boot to scan /sys and populate /dev.\n"
//usage:       "\n"
//usage:       "Bare mdev is a kernel hotplug helper. To activate it:\n"
//usage:       "	echo /sbin/mdev >/proc/sys/kernel/hotplug\n"
//usage:	IF_FEATURE_MDEV_CONF(
//usage:       "\n"
//usage:       "It uses /etc/mdev.conf with lines\n"
//usage:       "	[-]DEVNAME UID:GID PERM"
//usage:			IF_FEATURE_MDEV_RENAME(" [>|=PATH]|[!]")
//usage:			IF_FEATURE_MDEV_EXEC(" [@|$|*PROG]")
//usage:       "\n"
//usage:       "where DEVNAME is device name regex, @major,minor[-minor2], or\n"
//usage:       "environment variable regex. A common use of the latter is\n"
//usage:       "to load modules for hotplugged devices:\n"
//usage:       "	$MODALIAS=.* 0:0 660 @modprobe \"$MODALIAS\"\n"
//usage:	)
//usage:       "\n"
//usage:       "If /dev/mdev.seq file exists, mdev will wait for its value\n"
//usage:       "to match $SEQNUM variable. This prevents plug/unplug races.\n"
//usage:       "To activate this feature, create empty /dev/mdev.seq at boot.\n"
//usage:       "\n"
//usage:       "If /dev/mdev.log file exists, debug log will be appended to it."

#include "libbb.h"
#include "xregex.h"

/* "mdev -s" scans /sys/class/xxx, looking for directories which have dev
 * file (it is of the form "M:m\n"). Example: /sys/class/tty/tty0/dev
 * contains "4:0\n". Directory name is taken as device name, path component
 * directly after /sys/class/ as subsystem. In this example, "tty0" and "tty".
 * Then mdev creates the /dev/device_name node.
 * If /sys/class/.../dev file does not exist, mdev still may act
 * on this device: see "@|$|*command args..." parameter in config file.
 *
 * mdev w/o parameters is called as hotplug helper. It takes device
 * and subsystem names from $DEVPATH and $SUBSYSTEM, extracts
 * maj,min from "/sys/$DEVPATH/dev" and also examines
 * $ACTION ("add"/"delete") and $FIRMWARE.
 *
 * If action is "add", mdev creates /dev/device_name similarly to mdev -s.
 * (todo: explain "delete" and $FIRMWARE)
 *
 * If /etc/mdev.conf exists, it may modify /dev/device_name's properties.
 *
 * Leading minus in 1st field means "don't stop on this line", otherwise
 * search is stopped after the matching line is encountered.
 *
 * $envvar=regex format is useful for loading modules for hot-plugged devices
 * which do not have driver loaded yet. In this case /sys/class/.../dev
 * does not exist, but $MODALIAS is set to needed module's name
 * (actually, an alias to it) by kernel. This rule instructs mdev
 * to load the module and exit:
 *    $MODALIAS=.* 0:0 660 @modprobe "$MODALIAS"
 * The kernel will generate another hotplug event when /sys/class/.../dev
 * file appears.
 *
 * When line matches, the device node is created, chmod'ed and chown'ed,
 * moved to path, and if >path, a symlink to moved node is created,
 * all this if /sys/class/.../dev exists.
 *    Examples:
 *    =loop/      - moves to /dev/loop
 *    >disk/sda%1 - moves to /dev/disk/sdaN, makes /dev/sdaN a symlink
 *
 * Then "command args..." is executed (via sh -c 'command args...').
 * @:execute on creation, $:on deletion, *:on both.
 * This happens regardless of /sys/class/.../dev existence.
 */

/* Kernel's hotplug environment constantly changes.
 * Here are new cases I observed on 3.1.0:
 *
 * Case with $DEVNAME and $DEVICE, not just $DEVPATH:
 * ACTION=add
 * BUSNUM=001
 * DEVICE=/proc/bus/usb/001/003
 * DEVNAME=bus/usb/001/003
 * DEVNUM=003
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5
 * DEVTYPE=usb_device
 * MAJOR=189
 * MINOR=2
 * PRODUCT=18d1/4e12/227
 * SUBSYSTEM=usb
 * TYPE=0/0/0
 *
 * Case with $DEVICE, but no $DEVNAME - apparenty, usb iface notification?
 * "Please load me a module" thing?
 * ACTION=add
 * DEVICE=/proc/bus/usb/001/003
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5/1-5:1.0
 * DEVTYPE=usb_interface
 * INTERFACE=8/6/80
 * MODALIAS=usb:v18D1p4E12d0227dc00dsc00dp00ic08isc06ip50
 * PRODUCT=18d1/4e12/227
 * SUBSYSTEM=usb
 * TYPE=0/0/0
 *
 * ACTION=add
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5/1-5:1.0/host5
 * DEVTYPE=scsi_host
 * SUBSYSTEM=scsi
 *
 * ACTION=add
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5/1-5:1.0/host5/scsi_host/host5
 * SUBSYSTEM=scsi_host
 *
 * ACTION=add
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5/1-5:1.0/host5/target5:0:0
 * DEVTYPE=scsi_target
 * SUBSYSTEM=scsi
 *
 * Case with strange $MODALIAS:
 * ACTION=add
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5/1-5:1.0/host5/target5:0:0/5:0:0:0
 * DEVTYPE=scsi_device
 * MODALIAS=scsi:t-0x00
 * SUBSYSTEM=scsi
 *
 * ACTION=add
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5/1-5:1.0/host5/target5:0:0/5:0:0:0/scsi_disk/5:0:0:0
 * SUBSYSTEM=scsi_disk
 *
 * ACTION=add
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5/1-5:1.0/host5/target5:0:0/5:0:0:0/scsi_device/5:0:0:0
 * SUBSYSTEM=scsi_device
 *
 * Case with explicit $MAJOR/$MINOR (no need to read /sys/$DEVPATH/dev?):
 * ACTION=add
 * DEVNAME=bsg/5:0:0:0
 * DEVPATH=/devices/pci0000:00/0000:00:02.1/usb1/1-5/1-5:1.0/host5/target5:0:0/5:0:0:0/bsg/5:0:0:0
 * MAJOR=253
 * MINOR=1
 * SUBSYSTEM=bsg
 *
 * ACTION=add
 * DEVPATH=/devices/virtual/bdi/8:16
 * SUBSYSTEM=bdi
 *
 * ACTION=add
 * DEVNAME=sdb
 * DEVPATH=/block/sdb
 * DEVTYPE=disk
 * MAJOR=8
 * MINOR=16
 * SUBSYSTEM=block
 *
 * Case with ACTION=change:
 * ACTION=change
 * DEVNAME=sdb
 * DEVPATH=/block/sdb
 * DEVTYPE=disk
 * DISK_MEDIA_CHANGE=1
 * MAJOR=8
 * MINOR=16
 * SUBSYSTEM=block
 */

static const char keywords[] ALIGN1 = "add\0remove\0change\0";
enum { OP_add, OP_remove };

struct rule {
	bool keep_matching;
	bool regex_compiled;
	mode_t mode;
	int maj, min0, min1;
	struct bb_uidgid_t ugid;
	char *envvar;
	char *ren_mov;
	IF_FEATURE_MDEV_EXEC(char *r_cmd;)
	regex_t match;
};

struct globals {
	int root_major, root_minor;
	smallint verbose;
	char *subsystem;
#if ENABLE_FEATURE_MDEV_CONF
	const char *filename;
	parser_t *parser;
	struct rule **rule_vec;
	unsigned rule_idx;
#endif
	struct rule cur_rule;
} FIX_ALIASING;
#define G (*(struct globals*)&bb_common_bufsiz1)
#define INIT_G() do { \
	IF_NOT_FEATURE_MDEV_CONF(G.cur_rule.maj = -1;) \
	IF_NOT_FEATURE_MDEV_CONF(G.cur_rule.mode = 0660;) \
} while (0)


/* Prevent infinite loops in /sys symlinks */
#define MAX_SYSFS_DEPTH 3

/* We use additional 64+ bytes in make_device() */
#define SCRATCH_SIZE 80

#if 0
# define dbg(...) bb_error_msg(__VA_ARGS__)
#else
# define dbg(...) ((void)0)
#endif


#if ENABLE_FEATURE_MDEV_CONF

static void make_default_cur_rule(void)
{
	memset(&G.cur_rule, 0, sizeof(G.cur_rule));
	G.cur_rule.maj = -1; /* "not a @major,minor rule" */
	G.cur_rule.mode = 0660;
}

static void clean_up_cur_rule(void)
{
	free(G.cur_rule.envvar);
	if (G.cur_rule.regex_compiled)
		regfree(&G.cur_rule.match);
	free(G.cur_rule.ren_mov);
	IF_FEATURE_MDEV_EXEC(free(G.cur_rule.r_cmd);)
	make_default_cur_rule();
}

static void parse_next_rule(void)
{
	/* Note: on entry, G.cur_rule is set to default */
	while (1) {
		char *tokens[4];
		char *val;

		/* No PARSE_EOL_COMMENTS, because command may contain '#' chars */
		if (!config_read(G.parser, tokens, 4, 3, "# \t", PARSE_NORMAL & ~PARSE_EOL_COMMENTS))
			break;

		/* Fields: [-]regex uid:gid mode [alias] [cmd] */
		dbg("token1:'%s'", tokens[1]);

		/* 1st field */
		val = tokens[0];
		G.cur_rule.keep_matching = ('-' == val[0]);
		val += G.cur_rule.keep_matching; /* swallow leading dash */
		if (val[0] == '@') {
			/* @major,minor[-minor2] */
			/* (useful when name is ambiguous:
			 * "/sys/class/usb/lp0" and
			 * "/sys/class/printer/lp0")
			 */
			int sc = sscanf(val, "@%u,%u-%u", &G.cur_rule.maj, &G.cur_rule.min0, &G.cur_rule.min1);
			if (sc < 2 || G.cur_rule.maj < 0) {
				bb_error_msg("bad @maj,min on line %d", G.parser->lineno);
				goto next_rule;
			}
			if (sc == 2)
				G.cur_rule.min1 = G.cur_rule.min0;
		} else {
			if (val[0] == '$') {
				char *eq = strchr(++val, '=');
				if (!eq) {
					bb_error_msg("bad $envvar=regex on line %d", G.parser->lineno);
					goto next_rule;
				}
				G.cur_rule.envvar = xstrndup(val, eq - val);
				val = eq + 1;
			}
			xregcomp(&G.cur_rule.match, val, REG_EXTENDED);
			G.cur_rule.regex_compiled = 1;
		}

		/* 2nd field: uid:gid - device ownership */
		if (get_uidgid(&G.cur_rule.ugid, tokens[1], /*allow_numeric:*/ 1) == 0) {
			bb_error_msg("unknown user/group '%s' on line %d", tokens[1], G.parser->lineno);
			goto next_rule;
		}

		/* 3rd field: mode - device permissions */
		bb_parse_mode(tokens[2], &G.cur_rule.mode);

		/* 4th field (opt): ">|=alias" or "!" to not create the node */
		val = tokens[3];
		if (ENABLE_FEATURE_MDEV_RENAME && val && strchr(">=!", val[0])) {
			char *s = skip_non_whitespace(val);
			G.cur_rule.ren_mov = xstrndup(val, s - val);
			val = skip_whitespace(s);
		}

		if (ENABLE_FEATURE_MDEV_EXEC && val && val[0]) {
			const char *s = "$@*";
			const char *s2 = strchr(s, val[0]);
			if (!s2) {
				bb_error_msg("bad line %u", G.parser->lineno);
				goto next_rule;
			}
			IF_FEATURE_MDEV_EXEC(G.cur_rule.r_cmd = xstrdup(val);)
		}

		return;
 next_rule:
		clean_up_cur_rule();
	} /* while (config_read) */

	dbg("config_close(G.parser)");
	config_close(G.parser);
	G.parser = NULL;

	return;
}

/* If mdev -s, we remember rules in G.rule_vec[].
 * Otherwise, there is no point in doing it, and we just
 * save only one parsed rule in G.cur_rule.
 */
static const struct rule *next_rule(void)
{
	struct rule *rule;

	/* Open conf file if we didn't do it yet */
	if (!G.parser && G.filename) {
		dbg("config_open('%s')", G.filename);
		G.parser = config_open2(G.filename, fopen_for_read);
		G.filename = NULL;
	}

	if (G.rule_vec) {
		/* mdev -s */
		/* Do we have rule parsed already? */
		if (G.rule_vec[G.rule_idx]) {
			dbg("< G.rule_vec[G.rule_idx:%d]=%p", G.rule_idx, G.rule_vec[G.rule_idx]);
			return G.rule_vec[G.rule_idx++];
		}
		make_default_cur_rule();
	} else {
		/* not mdev -s */
		clean_up_cur_rule();
	}

	/* Parse one more rule if file isn't fully read */
	rule = &G.cur_rule;
	if (G.parser) {
		parse_next_rule();
		if (G.rule_vec) { /* mdev -s */
			rule = memcpy(xmalloc(sizeof(G.cur_rule)), &G.cur_rule, sizeof(G.cur_rule));
			G.rule_vec = xrealloc_vector(G.rule_vec, 4, G.rule_idx);
			G.rule_vec[G.rule_idx++] = rule;
			dbg("> G.rule_vec[G.rule_idx:%d]=%p", G.rule_idx, G.rule_vec[G.rule_idx]);
		}
	}

	return rule;
}

#else

# define next_rule() (&G.cur_rule)

#endif

/* Builds an alias path.
 * This function potentionally reallocates the alias parameter.
 * Only used for ENABLE_FEATURE_MDEV_RENAME
 */
static char *build_alias(char *alias, const char *device_name)
{
	char *dest;

	/* ">bar/": rename to bar/device_name */
	/* ">bar[/]baz": rename to bar[/]baz */
	dest = strrchr(alias, '/');
	if (dest) { /* ">bar/[baz]" ? */
		*dest = '\0'; /* mkdir bar */
		bb_make_directory(alias, 0755, FILEUTILS_RECUR);
		*dest = '/';
		if (dest[1] == '\0') { /* ">bar/" => ">bar/device_name" */
			dest = alias;
			alias = concat_path_file(alias, device_name);
			free(dest);
		}
	}

	return alias;
}

/* mknod in /dev based on a path like "/sys/block/hda/hda1"
 * NB1: path parameter needs to have SCRATCH_SIZE scratch bytes
 * after NUL, but we promise to not mangle (IOW: to restore if needed)
 * path string.
 * NB2: "mdev -s" may call us many times, do not leak memory/fds!
 *
 * device_name = $DEVNAME (may be NULL)
 * path        = /sys/$DEVPATH
 */
static void make_device(char *device_name, char *path, int operation)
{
	int major, minor, type, len;

	if (G.verbose)
		bb_error_msg("device: %s, %s", device_name, path);

	/* Try to read major/minor string.  Note that the kernel puts \n after
	 * the data, so we don't need to worry about null terminating the string
	 * because sscanf() will stop at the first nondigit, which \n is.
	 * We also depend on path having writeable space after it.
	 */
	major = -1;
	if (operation == OP_add) {
		char *dev_maj_min = path + strlen(path);

		strcpy(dev_maj_min, "/dev");
		len = open_read_close(path, dev_maj_min + 1, 64);
		*dev_maj_min = '\0';
		if (len < 1) {
			if (!ENABLE_FEATURE_MDEV_EXEC)
				return;
			/* no "dev" file, but we can still run scripts
			 * based on device name */
		} else if (sscanf(++dev_maj_min, "%u:%u", &major, &minor) == 2) {
			if (G.verbose)
				bb_error_msg("maj,min: %u,%u", major, minor);
		} else {
			major = -1;
		}
	}
	/* else: for delete, -1 still deletes the node, but < -1 suppresses that */

	/* Determine device name, type, major and minor */
	if (!device_name)
		device_name = (char*) bb_basename(path);
	/* http://kernel.org/doc/pending/hotplug.txt says that only
	 * "/sys/block/..." is for block devices. "/sys/bus" etc is not.
	 * But since 2.6.25 block devices are also in /sys/class/block.
	 * We use strstr("/block/") to forestall future surprises.
	 */
	type = S_IFCHR;
	if (strstr(path, "/block/") || (G.subsystem && strncmp(G.subsystem, "block", 5) == 0))
		type = S_IFBLK;

#if ENABLE_FEATURE_MDEV_CONF
	G.rule_idx = 0; /* restart from the beginning (think mdev -s) */
#endif
	for (;;) {
		const char *str_to_match;
		regmatch_t off[1 + 9 * ENABLE_FEATURE_MDEV_RENAME_REGEXP];
		char *command;
		char *alias;
		char aliaslink = aliaslink; /* for compiler */
		char *node_name;
		const struct rule *rule;

		str_to_match = device_name;

		rule = next_rule();

#if ENABLE_FEATURE_MDEV_CONF
		if (rule->maj >= 0) {  /* @maj,min rule */
			if (major != rule->maj)
				continue;
			if (minor < rule->min0 || minor > rule->min1)
				continue;
			memset(off, 0, sizeof(off));
			goto rule_matches;
		}
		if (rule->envvar) { /* $envvar=regex rule */
			str_to_match = getenv(rule->envvar);
			dbg("getenv('%s'):'%s'", rule->envvar, str_to_match);
			if (!str_to_match)
				continue;
		}
		/* else: str_to_match = device_name */

		if (rule->regex_compiled) {
			int regex_match = regexec(&rule->match, str_to_match, ARRAY_SIZE(off), off, 0);
			dbg("regex_match for '%s':%d", str_to_match, regex_match);
			//bb_error_msg("matches:");
			//for (int i = 0; i < ARRAY_SIZE(off); i++) {
			//	if (off[i].rm_so < 0) continue;
			//	bb_error_msg("match %d: '%.*s'\n", i,
			//		(int)(off[i].rm_eo - off[i].rm_so),
			//		device_name + off[i].rm_so);
			//}

			if (regex_match != 0
			/* regexec returns whole pattern as "range" 0 */
			 || off[0].rm_so != 0
			 || (int)off[0].rm_eo != (int)strlen(str_to_match)
			) {
				continue; /* this rule doesn't match */
			}
		}
		/* else: it's final implicit "match-all" rule */
 rule_matches:
#endif
		dbg("rule matched");

		/* Build alias name */
		alias = NULL;
		if (ENABLE_FEATURE_MDEV_RENAME && rule->ren_mov) {
			aliaslink = rule->ren_mov[0];
			if (aliaslink == '!') {
				/* "!": suppress node creation/deletion */
				major = -2;
			}
			else if (aliaslink == '>' || aliaslink == '=') {
				if (ENABLE_FEATURE_MDEV_RENAME_REGEXP) {
					char *s;
					char *p;
					unsigned n;

					/* substitute %1..9 with off[1..9], if any */
					n = 0;
					s = rule->ren_mov;
					while (*s)
						if (*s++ == '%')
							n++;

					p = alias = xzalloc(strlen(rule->ren_mov) + n * strlen(str_to_match));
					s = rule->ren_mov + 1;
					while (*s) {
						*p = *s;
						if ('%' == *s) {
							unsigned i = (s[1] - '0');
							if (i <= 9 && off[i].rm_so >= 0) {
								n = off[i].rm_eo - off[i].rm_so;
								strncpy(p, str_to_match + off[i].rm_so, n);
								p += n - 1;
								s++;
							}
						}
						p++;
						s++;
					}
				} else {
					alias = xstrdup(rule->ren_mov + 1);
				}
			}
		}
		dbg("alias:'%s'", alias);

		command = NULL;
		IF_FEATURE_MDEV_EXEC(command = rule->r_cmd;)
		if (command) {
			const char *s = "$@*";
			const char *s2 = strchr(s, command[0]);

			/* Are we running this command now?
			 * Run $cmd on delete, @cmd on create, *cmd on both
			 */
			if (s2 - s != (operation == OP_remove) || *s2 == '*') {
				/* We are here if: '*',
				 * or: '@' and delete = 0,
				 * or: '$' and delete = 1
				 */
				command++;
			} else {
				command = NULL;
			}
		}
		dbg("command:'%s'", command);

		/* "Execute" the line we found */
		node_name = device_name;
		if (ENABLE_FEATURE_MDEV_RENAME && alias) {
			node_name = alias = build_alias(alias, device_name);
			dbg("alias2:'%s'", alias);
		}

		if (operation == OP_add && major >= 0) {
			char *slash = strrchr(node_name, '/');
			if (slash) {
				*slash = '\0';
				bb_make_directory(node_name, 0755, FILEUTILS_RECUR);
				*slash = '/';
			}
			if (G.verbose)
				bb_error_msg("mknod: %s (%d,%d) %o", node_name, major, minor, rule->mode | type);
			if (mknod(node_name, rule->mode | type, makedev(major, minor)) && errno != EEXIST)
				bb_perror_msg("can't create '%s'", node_name);
			if (ENABLE_FEATURE_MDEV_CONF) {
				chmod(node_name, rule->mode);
				chown(node_name, rule->ugid.uid, rule->ugid.gid);
			}
			if (major == G.root_major && minor == G.root_minor)
				symlink(node_name, "root");
			if (ENABLE_FEATURE_MDEV_RENAME && alias) {
				if (aliaslink == '>') {
//TODO: on devtmpfs, device_name already exists and symlink() fails.
//End result is that instead of symlink, we have two nodes.
//What should be done?
					if (G.verbose)
						bb_error_msg("symlink: %s", device_name);
					symlink(node_name, device_name);
				}
			}
		}

		if (ENABLE_FEATURE_MDEV_EXEC && command) {
			/* setenv will leak memory, use putenv/unsetenv/free */
			char *s = xasprintf("%s=%s", "MDEV", node_name);
			char *s1 = xasprintf("%s=%s", "SUBSYSTEM", G.subsystem);
			putenv(s);
			putenv(s1);
			if (G.verbose)
				bb_error_msg("running: %s", command);
			if (system(command) == -1)
				bb_perror_msg("can't run '%s'", command);
			bb_unsetenv_and_free(s1);
			bb_unsetenv_and_free(s);
		}

		if (operation == OP_remove && major >= -1) {
			if (ENABLE_FEATURE_MDEV_RENAME && alias) {
				if (aliaslink == '>') {
					if (G.verbose)
						bb_error_msg("unlink: %s", device_name);
					unlink(device_name);
				}
			}
			if (G.verbose)
				bb_error_msg("unlink: %s", node_name);
			unlink(node_name);
		}

		if (ENABLE_FEATURE_MDEV_RENAME)
			free(alias);

		/* We found matching line.
		 * Stop unless it was prefixed with '-'
		 */
		if (!ENABLE_FEATURE_MDEV_CONF || !rule->keep_matching)
			break;
	} /* for (;;) */
}

/* File callback for /sys/ traversal */
static int FAST_FUNC fileAction(const char *fileName,
		struct stat *statbuf UNUSED_PARAM,
		void *userData,
		int depth UNUSED_PARAM)
{
	size_t len = strlen(fileName) - 4; /* can't underflow */
	char *scratch = userData;

	/* len check is for paranoid reasons */
	if (strcmp(fileName + len, "/dev") != 0 || len >= PATH_MAX)
		return FALSE;

	strcpy(scratch, fileName);
	scratch[len] = '\0';
	make_device(/*DEVNAME:*/ NULL, scratch, OP_add);

	return TRUE;
}

/* Directory callback for /sys/ traversal */
static int FAST_FUNC dirAction(const char *fileName UNUSED_PARAM,
		struct stat *statbuf UNUSED_PARAM,
		void *userData UNUSED_PARAM,
		int depth)
{
	/* Extract device subsystem -- the name of the directory
	 * under /sys/class/ */
	if (1 == depth) {
		free(G.subsystem);
		G.subsystem = strrchr(fileName, '/');
		if (G.subsystem)
			G.subsystem = xstrdup(G.subsystem + 1);
	}

	return (depth >= MAX_SYSFS_DEPTH ? SKIP : TRUE);
}

/* For the full gory details, see linux/Documentation/firmware_class/README
 *
 * Firmware loading works like this:
 * - kernel sets FIRMWARE env var
 * - userspace checks /lib/firmware/$FIRMWARE
 * - userspace waits for /sys/$DEVPATH/loading to appear
 * - userspace writes "1" to /sys/$DEVPATH/loading
 * - userspace copies /lib/firmware/$FIRMWARE into /sys/$DEVPATH/data
 * - userspace writes "0" (worked) or "-1" (failed) to /sys/$DEVPATH/loading
 * - kernel loads firmware into device
 */
static void load_firmware(const char *firmware, const char *sysfs_path)
{
	int cnt;
	int firmware_fd, loading_fd;

	/* check for /lib/firmware/$FIRMWARE */
	xchdir("/lib/firmware");
	firmware_fd = open(firmware, O_RDONLY); /* can fail */

	/* check for /sys/$DEVPATH/loading ... give 30 seconds to appear */
	xchdir(sysfs_path);
	for (cnt = 0; cnt < 30; ++cnt) {
		loading_fd = open("loading", O_WRONLY);
		if (loading_fd >= 0)
			goto loading;
		sleep(1);
	}
	goto out;

 loading:
	cnt = 0;
	if (firmware_fd >= 0) {
		int data_fd;

		/* tell kernel we're loading by "echo 1 > /sys/$DEVPATH/loading" */
		if (full_write(loading_fd, "1", 1) != 1)
			goto out;

		/* load firmware into /sys/$DEVPATH/data */
		data_fd = open("data", O_WRONLY);
		if (data_fd < 0)
			goto out;
		cnt = bb_copyfd_eof(firmware_fd, data_fd);
		if (ENABLE_FEATURE_CLEAN_UP)
			close(data_fd);
	}

	/* Tell kernel result by "echo [0|-1] > /sys/$DEVPATH/loading"
	 * Note: we emit -1 also if firmware file wasn't found.
	 * There are cases when otherwise kernel would wait for minutes
	 * before timing out.
	 */
	if (cnt > 0)
		full_write(loading_fd, "0", 1);
	else
		full_write(loading_fd, "-1", 2);

 out:
	if (ENABLE_FEATURE_CLEAN_UP) {
		close(firmware_fd);
		close(loading_fd);
	}
}

int mdev_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int mdev_main(int argc UNUSED_PARAM, char **argv)
{
	RESERVE_CONFIG_BUFFER(temp, PATH_MAX + SCRATCH_SIZE);

	INIT_G();

#if ENABLE_FEATURE_MDEV_CONF
	G.filename = "/etc/mdev.conf";
#endif

	/* We can be called as hotplug helper */
	/* Kernel cannot provide suitable stdio fds for us, do it ourself */
	bb_sanitize_stdio();

	/* Force the configuration file settings exactly */
	umask(0);

	xchdir("/dev");

	if (argv[1] && strcmp(argv[1], "-s") == 0) {
		/* Scan:
		 * mdev -s
		 */
		struct stat st;

#if ENABLE_FEATURE_MDEV_CONF
		/* Same as xrealloc_vector(NULL, 4, 0): */
		G.rule_vec = xzalloc((1 << 4) * sizeof(*G.rule_vec));
#endif
		xstat("/", &st);
		G.root_major = major(st.st_dev);
		G.root_minor = minor(st.st_dev);

		/* ACTION_FOLLOWLINKS is needed since in newer kernels
		 * /sys/block/loop* (for example) are symlinks to dirs,
		 * not real directories.
		 * (kernel's CONFIG_SYSFS_DEPRECATED makes them real dirs,
		 * but we can't enforce that on users)
		 */
		if (access("/sys/class/block", F_OK) != 0) {
			/* Scan obsolete /sys/block only if /sys/class/block
			 * doesn't exist. Otherwise we'll have dupes.
			 * Also, do not complain if it doesn't exist.
			 * Some people configure kernel to have no blockdevs.
			 */
			recursive_action("/sys/block",
				ACTION_RECURSE | ACTION_FOLLOWLINKS | ACTION_QUIET,
				fileAction, dirAction, temp, 0);
		}
		recursive_action("/sys/class",
			ACTION_RECURSE | ACTION_FOLLOWLINKS,
			fileAction, dirAction, temp, 0);
	} else {
		char *fw;
		char *seq;
		char *action;
		char *env_devname;
		char *env_devpath;
		smalluint op;

		/* Hotplug:
		 * env ACTION=... DEVPATH=... SUBSYSTEM=... [SEQNUM=...] mdev
		 * ACTION can be "add" or "remove"
		 * DEVPATH is like "/block/sda" or "/class/input/mice"
		 */
		action = getenv("ACTION");
		op = index_in_strings(keywords, action);
		env_devname = getenv("DEVNAME"); /* can be NULL */
		env_devpath = getenv("DEVPATH");
		G.subsystem = getenv("SUBSYSTEM");
		if (!action || !env_devpath /*|| !G.subsystem*/)
			bb_show_usage();
		fw = getenv("FIRMWARE");
		/* If it exists, does /dev/mdev.seq match $SEQNUM?
		 * If it does not match, earlier mdev is running
		 * in parallel, and we need to wait */
		seq = getenv("SEQNUM");
		if (seq) {
			int timeout = 2000 / 32; /* 2000 msec */
			do {
				int seqlen;
				char seqbuf[sizeof(int)*3 + 2];

				seqlen = open_read_close("mdev.seq", seqbuf, sizeof(seqbuf) - 1);
				if (seqlen < 0) {
					seq = NULL;
					break;
				}
				seqbuf[seqlen] = '\0';
				if (seqbuf[0] == '\n' /* seed file? */
				 || strcmp(seq, seqbuf) == 0 /* correct idx? */
				) {
					break;
				}
				usleep(32*1000);
			} while (--timeout);
		}

		{
			int logfd = open("/dev/mdev.log", O_WRONLY | O_APPEND);
			if (logfd >= 0) {
				xmove_fd(logfd, STDERR_FILENO);
				G.verbose = 1;
				bb_error_msg("seq: %s action: %s", seq, action);
			}
		}

		snprintf(temp, PATH_MAX, "/sys%s", env_devpath);
		if (op == OP_remove) {
			/* Ignoring "remove firmware". It was reported
			 * to happen and to cause erroneous deletion
			 * of device nodes. */
			if (!fw)
				make_device(env_devname, temp, op);
		}
		else if (op == OP_add) {
			make_device(env_devname, temp, op);
			if (ENABLE_FEATURE_MDEV_LOAD_FIRMWARE) {
				if (fw)
					load_firmware(fw, temp);
			}
		}

		if (seq) {
			xopen_xwrite_close("mdev.seq", utoa(xatou(seq) + 1));
		}
	}

	if (ENABLE_FEATURE_CLEAN_UP)
		RELEASE_CONFIG_BUFFER(temp);

	return EXIT_SUCCESS;
}
