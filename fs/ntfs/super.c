/*
 * super.c - NTFS kernel super block handling. Part of the Linux-NTFS project.
 *
 * Copyright (c) 2001,2002 Anton Altaparmakov.
 * Copyright (C) 2001,2002 Richard Russon.
 *
 * This program/include file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program/include file is distributed in the hope that it will be 
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty 
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (in the main directory of the Linux-NTFS 
 * distribution in the file COPYING); if not, write to the Free Software
 * Foundation,Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/stddef.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/spinlock.h>
#include <linux/genhd.h>	/* For gendisk stuff. */
#include <linux/blkdev.h>	/* Fox get_hardsect_size. */

#include "ntfs.h"
#include "sysctl.h"

/* Number of mounted file systems which have compression enabled. */
static unsigned long ntfs_nr_compression_users = 0;

/* Error constants/strings used in inode.c::ntfs_show_options(). */
typedef enum {
	ON_ERRORS_PANIC			= 0x01,
	ON_ERRORS_REMOUNT_RO		= 0x02,
	ON_ERRORS_CONTINUE		= 0x04,
	ON_ERRORS_RECOVER		= 0x10,
} ON_ERRORS_ACTIONS;

const option_t on_errors_arr[] = {
	{ ON_ERRORS_PANIC,			    "panic" },
	{ ON_ERRORS_REMOUNT_RO,			    "remount-ro", },
	{ ON_ERRORS_CONTINUE,			    "continue", },
	{ ON_ERRORS_RECOVER,			    "recover" },
	{ ON_ERRORS_RECOVER | ON_ERRORS_PANIC,	    "recover_or_panic" },
	{ ON_ERRORS_RECOVER | ON_ERRORS_REMOUNT_RO, "recover_or_remount-ro" },
	{ ON_ERRORS_RECOVER | ON_ERRORS_CONTINUE,   "recover_or_continue" },
	{ 0,					    NULL }
};

static const option_t readdir_opts_arr[] = {
	{ SHOW_SYSTEM,	"system" },
	{ SHOW_WIN32,	"win32" },
	{ SHOW_WIN32,	"long" },
	{ SHOW_DOS,	"dos" },
	{ SHOW_DOS,	"short" },
	{ SHOW_POSIX,	"posix" },
	{ SHOW_ALL,	"all" },
	{ 0,		NULL }
};

/**
 * simple_getbool -
 *
 * Copied from old ntfs driver (which copied from vfat driver).
 */
static int simple_getbool(char *s, BOOL *setval)
{
	if (s) {
		if (!strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "true"))
			*setval = TRUE;
		else if (!strcmp(s, "0") || !strcmp(s, "no") ||
							!strcmp(s, "false"))
			*setval = FALSE;
		else
			return 0;
	} else
		*setval = TRUE;
	return 1;
}

/**
 * parse_options - parse the (re)mount options
 * @vol:	ntfs volume
 * @opt:	string containing the (re)mount options
 *
 * Parse the recognized options in @opt for the ntfs volume described by @vol.
 */
static BOOL parse_options(ntfs_volume *vol, char *opt)
{
	char *p, *v, *ov;
	static char *utf8 = "utf8";
	int errors = 0, sloppy = 0;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	mode_t fmask = (mode_t)-1, dmask = (mode_t)-1;
	int mft_zone_multiplier = -1, on_errors = -1, readdir_opts = -1;
	struct nls_table *nls_map = NULL, *old_nls;

	/* I am lazy... (-8 */
#define NTFS_GETOPT_WITH_DEFAULT(option, variable, default_value)	\
	if (!strcmp(p, option)) {					\
		if (!v || !*v)						\
			variable = default_value;			\
		else {							\
			variable = simple_strtoul(ov = v, &v, 0);	\
			if (*v)						\
				goto needs_val;				\
		}							\
	} 
#define NTFS_GETOPT(option, variable)					\
	if (!strcmp(p, option)) {					\
		if (!v || !*v)						\
			goto needs_arg;					\
		variable = simple_strtoul(ov = v, &v, 0);		\
		if (*v)							\
			goto needs_val;					\
	} 
#define NTFS_GETOPT_OPTIONS_ARRAY(option, variable, opt_array)		\
	if (!strcmp(p, option)) {					\
		int _i;							\
		if (!v || !*v)						\
			goto needs_arg;					\
		ov = v;							\
		if (variable == -1)					\
			variable = 0;					\
		for (_i = 0; opt_array[_i].str && *opt_array[_i].str; _i++) \
			if (!strcmp(opt_array[_i].str, v)) {		\
				variable |= opt_array[_i].val;		\
				break;					\
			}						\
		if (!opt_array[_i].str || !*opt_array[_i].str)		\
			goto needs_val;					\
	}
	if (!opt || !*opt)
		goto no_mount_options;
	while ((p = strsep(&opt, ","))) {
		if ((v = strchr(p, '=')))
			*v++ = '\0';
		NTFS_GETOPT("uid", uid)
		else NTFS_GETOPT("gid", gid)
		else NTFS_GETOPT("umask", fmask = dmask)
		else NTFS_GETOPT("fmask", fmask)
		else NTFS_GETOPT("dmask", dmask)
		else NTFS_GETOPT_WITH_DEFAULT("sloppy", sloppy, TRUE)
		else NTFS_GETOPT("mft_zone_multiplier", mft_zone_multiplier)
		else NTFS_GETOPT_OPTIONS_ARRAY("errors", on_errors,
				on_errors_arr)
		else NTFS_GETOPT_OPTIONS_ARRAY("show_inodes", readdir_opts,
				readdir_opts_arr)
		else if (!strcmp(p, "show_system_files")) {
			BOOL val = FALSE;
			ntfs_warning(vol->sb, "Option show_system_files is "
				   "deprecated. Please use option "
				   "show_inodes=system in the future.");
			if (!v || !*v)
				val = TRUE;
			else if (!simple_getbool(v, &val))
				goto needs_bool;
			if (val) {
				if (readdir_opts == -1)
					readdir_opts = 0;
				readdir_opts |= SHOW_SYSTEM;
			}
		} else if (!strcmp(p, "posix")) {
			BOOL val = FALSE;
			ntfs_warning(vol->sb, "Option posix is deprecated. "
				   "Please use option show_inodes=posix "
				   "instead. Be aware that some userspace "
				   "applications may be confused by this, "
				   "since the short and long names of "
				   "directory inodes will have the same inode "
				   "numbers, yet each will only have a link "
				   "count of 1 due to Linux not supporting "
				   "directory hard links.");
			if (!v || !*v)
				goto needs_arg;
			else if (!simple_getbool(v, &val))
				goto needs_bool;
			if (val) {
				if (readdir_opts == -1)
					readdir_opts = 0;
				readdir_opts |= SHOW_POSIX;
			}
		} else if (!strcmp(p, "nls") || !strcmp(p, "iocharset")) {
			if (!strcmp(p, "iocharset"))
				ntfs_warning(vol->sb, "Option iocharset is "
						"deprecated. Please use "
						"option nls=<charsetname> in "
						"the future.");
			if (!v || !*v)
				goto needs_arg;
use_utf8:
			old_nls = nls_map;
			nls_map = load_nls(v);
			if (!nls_map) {
				if (!old_nls) {
					ntfs_error(vol->sb, "NLS character set "
							"%s not found.", v);
					return FALSE;
				}
				ntfs_error(vol->sb, "NLS character set %s not "
						"found. Using previous one %s.",
						v, old_nls->charset);
				nls_map = old_nls;
			} else /* nls_map */ {
				if (old_nls)
					unload_nls(old_nls);
			}
		} else if (!strcmp(p, "utf8")) {
			BOOL val = FALSE;
			ntfs_warning(vol->sb, "Option utf8 is no longer "
				   "supported, using option nls=utf8. Please "
				   "use option nls=utf8 in the future and "
				   "make sure utf8 is compiled either as a "
				   "module or into the kernel.");
			if (!v || !*v)
				val = TRUE;
			else if (!simple_getbool(v, &val))
				goto needs_bool;
			if (val) {
				v = utf8;
				goto use_utf8;
			}
		} else {
			ntfs_error(vol->sb, "Unrecognized mount option %s.", p);
			if (errors < INT_MAX)
				errors++;
		}
#undef NTFS_GETOPT_OPTIONS_ARRAY
#undef NTFS_GETOPT
#undef NTFS_GETOPT_WITH_DEFAULT
	}
no_mount_options:
	if (errors && !sloppy)
		return FALSE;
	if (sloppy)
		ntfs_warning(vol->sb, "Sloppy option given. Ignoring "
				"unrecognized mount option(s) and continuing.");
	/* Keep this first! */
	if (on_errors != -1) {
		if (!on_errors) {
			ntfs_error(vol->sb, "Invalid errors option argument "
					"or bug in options parser.");
			return FALSE;
		}
	}
	if (nls_map) {
		if (vol->nls_map) {
			ntfs_error(vol->sb, "Cannot change NLS character set "
					"on remount.");
			return FALSE;
		} /* else (!vol->nls_map) */
		ntfs_debug("Using NLS character set %s.", nls_map->charset);
		vol->nls_map = nls_map;
	} else /* (!nls_map) */ {
		if (!vol->nls_map) {
			vol->nls_map = load_nls_default();
			if (!vol->nls_map) {
				ntfs_error(vol->sb, "Failed to load default "
						"NLS character set.");
				return FALSE;
			}
			ntfs_debug("Using default NLS character set (%s).",
					vol->nls_map->charset);
		}
	}
	if (mft_zone_multiplier != -1) {
		if (vol->mft_zone_multiplier && vol->mft_zone_multiplier !=
				mft_zone_multiplier) {
			ntfs_error(vol->sb, "Cannot change mft_zone_multiplier "
					"on remount.");
			return FALSE;
		}
		if (mft_zone_multiplier < 1 || mft_zone_multiplier > 4) {
			ntfs_error(vol->sb, "Invalid mft_zone_multiplier. "
					"Using default value, i.e. 1.");
			mft_zone_multiplier = 1;
		}
		vol->mft_zone_multiplier = mft_zone_multiplier;
	} if (!vol->mft_zone_multiplier)
		/* Not specified and it is the first mount, so set default. */
		vol->mft_zone_multiplier = 1;
	if (on_errors != -1)
		vol->on_errors = on_errors;
	if (!vol->on_errors)
		vol->on_errors = ON_ERRORS_CONTINUE;
	if (uid != (uid_t)-1)
		vol->uid = uid;
	if (gid != (gid_t)-1)
		vol->gid = gid;
	if (fmask != (mode_t)-1)
		vol->fmask = fmask;
	if (dmask != (mode_t)-1)
		vol->dmask = dmask;
	if (readdir_opts != -1)
		vol->readdir_opts = readdir_opts;
	return TRUE;
needs_arg:
	ntfs_error(vol->sb, "The %s option requires an argument.", p);
	return FALSE;
needs_bool:
	ntfs_error(vol->sb, "The %s option requires a boolean argument.", p);
	return FALSE;
needs_val:
	ntfs_error(vol->sb, "Invalid %s option argument: %s", p, ov);
	return FALSE;
}

/**
 * ntfs_remount - change the mount options of a mounted ntfs filesystem
 * @sb:		superblock of mounted ntfs filesystem
 * @flags:	remount flags
 * @opt:	remount options string
 *
 * Change the mount options of an already mounted ntfs filesystem.
 *
 * NOTE: The VFS set the @sb->s_flags remount flags to @flags after
 * ntfs_remount() returns successfully (i.e. returns 0). Otherwise,
 * @sb->s_flags are not changed.
 */
static int ntfs_remount(struct super_block *sb, int *flags, char *opt)
{
	ntfs_volume *vol = NTFS_SB(sb);

	ntfs_debug("Entering.");

	// FIXME/TODO: If left like this we will have problems with rw->ro and
	// ro->rw, as well as with sync->async and vice versa remounts.
	// Note: The VFS already checks that there are no pending deletes and
	// no open files for writing. So we only need to worry about dirty
	// inode pages and dirty system files (which include dirty inodes).
	// Either handle by flushing the whole volume NOW or by having the
	// write routines work on MS_RDONLY fs and guarantee we don't mark
	// anything as dirty if MS_RDONLY is set. That way the dirty data
	// would get flushed but no new dirty data would appear. This is
	// probably best but we need to be careful not to mark anything dirty
	// or the MS_RDONLY will be leaking writes.

	// TODO: Deal with *flags.

	if (!parse_options(vol, opt))
		return -EINVAL;
	return 0;
}

/**
 * is_boot_sector_ntfs - check whether a boot sector is a valid NTFS boot sector
 * @sb:		Super block of the device to which @b belongs.
 * @b:		Boot sector of device @sb to check.
 * @silent:	If TRUE, all output will be silenced.
 *
 * is_boot_sector_ntfs() checks whether the boot sector @b is a valid NTFS boot
 * sector. Returns TRUE if it is valid and FALSE if not.
 *
 * @sb is only needed for warning/error output, i.e. it can be NULL when silent
 * is TRUE.
 */
static BOOL is_boot_sector_ntfs(const struct super_block *sb,
		const NTFS_BOOT_SECTOR *b, const BOOL silent)
{
	/*
	 * Check that checksum == sum of u32 values from b to the checksum
	 * field. If checksum is zero, no checking is done.
	 */
	if ((void*)b < (void*)&b->checksum && b->checksum) {
		u32 i, *u;
		for (i = 0, u = (u32*)b; u < (u32*)(&b->checksum); ++u)
			i += le32_to_cpup(u);
		if (le32_to_cpu(b->checksum) != i)
			goto not_ntfs;
	}
	/* Check OEMidentifier is "NTFS    " */
	if (b->oem_id != magicNTFS)
		goto not_ntfs;
	/* Check bytes per sector value is between 256 and 4096. */
	if (le16_to_cpu(b->bpb.bytes_per_sector) <  0x100 ||
			le16_to_cpu(b->bpb.bytes_per_sector) > 0x1000)
		goto not_ntfs;
	/* Check sectors per cluster value is valid. */
	switch (b->bpb.sectors_per_cluster) {
	case 1: case 2: case 4: case 8: case 16: case 32: case 64: case 128:
		break;
	default:
		goto not_ntfs;
	}
	/* Check the cluster size is not above 65536 bytes. */
	if ((u32)le16_to_cpu(b->bpb.bytes_per_sector) *
			b->bpb.sectors_per_cluster > 0x10000)
		goto not_ntfs;
	/* Check reserved/unused fields are really zero. */
	if (le16_to_cpu(b->bpb.reserved_sectors) ||
			le16_to_cpu(b->bpb.root_entries) ||
			le16_to_cpu(b->bpb.sectors) ||
			le16_to_cpu(b->bpb.sectors_per_fat) ||
			le32_to_cpu(b->bpb.large_sectors) || b->bpb.fats)
		goto not_ntfs;
	/* Check clusters per file mft record value is valid. */
	if ((u8)b->clusters_per_mft_record < 0xe1 || 
			(u8)b->clusters_per_mft_record > 0xf7)
		switch (b->clusters_per_mft_record) {
		case 1: case 2: case 4: case 8: case 16: case 32: case 64:
			break;
		default:
			goto not_ntfs;
		}
	/* Check clusters per index block value is valid. */
	if ((u8)b->clusters_per_index_record < 0xe1 || 
			(u8)b->clusters_per_index_record > 0xf7)
		switch (b->clusters_per_index_record) {
		case 1: case 2: case 4: case 8: case 16: case 32: case 64:
			break;
		default:
			goto not_ntfs;
		}
	/*
	 * Check for valid end of sector marker. We will work without it, but
	 * many BIOSes will refuse to boot from a bootsector if the magic is
	 * incorrect, so we emit a warning.
	 */
	if (!silent && b->end_of_sector_marker != cpu_to_le16(0xaa55))
		ntfs_warning(sb, "Invalid end of sector marker.");
	return TRUE;
not_ntfs:
	return FALSE;
}

/**
 * read_boot_sector - read the NTFS boot sector of a device
 * @sb:		super block of device to read the boot sector from
 * @silent:	if true, suppress all output
 *
 * Reads the boot sector from the device and validates it. If that fails, tries
 * to read the backup boot sector, first from the end of the device a-la NT4 and
 * later and then from the middle of the device a-la NT3.51 and before.
 *
 * If a valid boot sector is found but it is not the primary boot sector, we
 * repair the primary boot sector silently (unless the device is read-only or
 * the primary boot sector is not accessible).
 *
 * NOTE: To call this function, @sb must have the fields s_dev, the ntfs super
 * block (u.ntfs_sb), nr_blocks and the device flags (s_flags) initialized
 * to their respective values.
 *
 * Return the unlocked buffer head containing the boot sector or NULL on error.
 */
static struct buffer_head *read_ntfs_boot_sector(struct super_block *sb,
		const int silent)
{
	const char *read_err_str = "Unable to read %s boot sector.";
	struct buffer_head *bh_primary, *bh_backup;
	long nr_blocks = NTFS_SB(sb)->nr_blocks;

	/* Try to read primary boot sector. */
	if ((bh_primary = sb_bread(sb, 0))) {
		if (is_boot_sector_ntfs(sb, (NTFS_BOOT_SECTOR*)
				bh_primary->b_data, silent))
			return bh_primary;
		if (!silent)
			ntfs_error(sb, "Primary boot sector is invalid.");
	} else if (!silent)
		ntfs_error(sb, read_err_str, "primary");
	if (NTFS_SB(sb)->on_errors & ~ON_ERRORS_RECOVER) {
		if (bh_primary)
			brelse(bh_primary);
		if (!silent)
			ntfs_error(sb, "Mount option errors=recover not used. "
					"Aborting without trying to recover.");
		return NULL;
	}
	/* Try to read NT4+ backup boot sector. */
	if ((bh_backup = sb_bread(sb, nr_blocks - 1))) {
		if (is_boot_sector_ntfs(sb, (NTFS_BOOT_SECTOR*)
				bh_backup->b_data, silent))
			goto hotfix_primary_boot_sector;
		brelse(bh_backup);
	} else if (!silent)
		ntfs_error(sb, read_err_str, "backup");
	/* Try to read NT3.51- backup boot sector. */
	if ((bh_backup = sb_bread(sb, nr_blocks >> 1))) {
		if (is_boot_sector_ntfs(sb, (NTFS_BOOT_SECTOR*)
				bh_backup->b_data, silent))
			goto hotfix_primary_boot_sector;
		if (!silent)
			ntfs_error(sb, "Could not find a valid backup boot "
					"sector.");
		brelse(bh_backup);
	} else if (!silent)
		ntfs_error(sb, read_err_str, "backup");
	/* We failed. Cleanup and return. */
	if (bh_primary)
		brelse(bh_primary);
	return NULL;
hotfix_primary_boot_sector:
	if (bh_primary) {
		/*
		 * If we managed to read sector zero and the volume is not
		 * read-only, copy the found, valid backup boot sector to the
		 * primary boot sector.
		 */
		if (!(sb->s_flags & MS_RDONLY)) {
			ntfs_warning(sb, "Hot-fix: Recovering invalid primary "
					"boot sector from backup copy.");
			memcpy(bh_primary->b_data, bh_backup->b_data,
					sb->s_blocksize);
			mark_buffer_dirty(bh_primary);
			ll_rw_block(WRITE, 1, &bh_primary);
			wait_on_buffer(bh_primary);
			if (buffer_uptodate(bh_primary)) {
				brelse(bh_backup);
				return bh_primary;
			}
			ntfs_error(sb, "Hot-fix: Device write error while "
					"recovering primary boot sector.");
		} else {
			ntfs_warning(sb, "Hot-fix: Recovery of primary boot "
					"sector failed: Read-only mount.");
		}
		brelse(bh_primary);
	}
	ntfs_warning(sb, "Using backup boot sector.");
	return bh_backup;
}

/**
 * parse_ntfs_boot_sector - parse the boot sector and store the data in @vol
 * @vol:	volume structure to initialise with data from boot sector
 * @b:		boot sector to parse
 * 
 * Parse the ntfs boot sector @b and store all imporant information therein in
 * the ntfs super block @vol. Return TRUE on success and FALSE on error.
 */
static BOOL parse_ntfs_boot_sector(ntfs_volume *vol, const NTFS_BOOT_SECTOR *b)
{
	unsigned int sectors_per_cluster_bits, nr_hidden_sects;
	int clusters_per_mft_record, clusters_per_index_record;
	s64 ll;

	vol->sector_size = le16_to_cpu(b->bpb.bytes_per_sector);
	vol->sector_size_bits = ffs(vol->sector_size) - 1;
	ntfs_debug("vol->sector_size = %i (0x%x)", vol->sector_size,
			vol->sector_size);
	ntfs_debug("vol->sector_size_bits = %i (0x%x)", vol->sector_size_bits,
			vol->sector_size_bits);
	if (vol->sector_size != vol->sb->s_blocksize)
		ntfs_warning(vol->sb, "The boot sector indicates a sector size "
				"different from the device sector size.");
	ntfs_debug("sectors_per_cluster = 0x%x", b->bpb.sectors_per_cluster);
	sectors_per_cluster_bits = ffs(b->bpb.sectors_per_cluster) - 1;
	ntfs_debug("sectors_per_cluster_bits = 0x%x",
			sectors_per_cluster_bits);
	nr_hidden_sects = le32_to_cpu(b->bpb.hidden_sectors);
	ntfs_debug("number of hidden sectors = 0x%x", nr_hidden_sects);
	vol->cluster_size = vol->sector_size << sectors_per_cluster_bits;
	vol->cluster_size_mask = vol->cluster_size - 1;
	vol->cluster_size_bits = ffs(vol->cluster_size) - 1;
	ntfs_debug("vol->cluster_size = %i (0x%x)", vol->cluster_size,
			vol->cluster_size);
	ntfs_debug("vol->cluster_size_mask = 0x%x", vol->cluster_size_mask);
	ntfs_debug("vol->cluster_size_bits = %i (0x%x)",
			vol->cluster_size_bits, vol->cluster_size_bits);
	if (vol->sector_size > vol->cluster_size) {
		ntfs_error(vol->sb, "Sector sizes above the cluster size are "
				"not supported. Sorry.");
		return FALSE;
	}
	if (vol->sb->s_blocksize > vol->cluster_size) {
		ntfs_error(vol->sb, "Cluster sizes smaller than the device "
				"sector size are not supported. Sorry.");
		return FALSE;
	}
	clusters_per_mft_record = b->clusters_per_mft_record;
	ntfs_debug("clusters_per_mft_record = %i (0x%x)",
			clusters_per_mft_record, clusters_per_mft_record);
	if (clusters_per_mft_record > 0)
		vol->mft_record_size = vol->cluster_size <<
				(ffs(clusters_per_mft_record) - 1);
	else
		/*
		 * When mft_record_size < cluster_size, clusters_per_mft_record
		 * = -log2(mft_record_size) bytes. mft_record_size normaly is
		 * 1024 bytes, which is encoded as 0xF6 (-10 in decimal).
		 */
		vol->mft_record_size = 1 << -clusters_per_mft_record;
	vol->mft_record_size_mask = vol->mft_record_size - 1;
	vol->mft_record_size_bits = ffs(vol->mft_record_size) - 1;
	ntfs_debug("vol->mft_record_size = %i (0x%x)", vol->mft_record_size,
			vol->mft_record_size);
	ntfs_debug("vol->mft_record_size_mask = 0x%x",
			vol->mft_record_size_mask);
	ntfs_debug("vol->mft_record_size_bits = %i (0x%x)",
			vol->mft_record_size_bits, vol->mft_record_size_bits); 
	clusters_per_index_record = b->clusters_per_index_record;
	ntfs_debug("clusters_per_index_record = %i (0x%x)",
			clusters_per_index_record, clusters_per_index_record); 
	if (clusters_per_index_record > 0)
		vol->index_record_size = vol->cluster_size <<
				(ffs(clusters_per_index_record) - 1);
	else
		/*
		 * When index_record_size < cluster_size,
		 * clusters_per_index_record = -log2(index_record_size) bytes.
		 * index_record_size normaly equals 4096 bytes, which is
		 * encoded as 0xF4 (-12 in decimal).
		 */
		vol->index_record_size = 1 << -clusters_per_index_record;
	vol->index_record_size_mask = vol->index_record_size - 1;
	vol->index_record_size_bits = ffs(vol->index_record_size) - 1;
	ntfs_debug("vol->index_record_size = %i (0x%x)",
			vol->index_record_size, vol->index_record_size); 
	ntfs_debug("vol->index_record_size_mask = 0x%x",
			vol->index_record_size_mask);
	ntfs_debug("vol->index_record_size_bits = %i (0x%x)",
			vol->index_record_size_bits,
			vol->index_record_size_bits);
	/*
	 * Get the size of the volume in clusters and check for 64-bit-ness.
	 * Windows currently only uses 32 bits to save the clusters so we do
	 * the same as it is much faster on 32-bit CPUs.
	 */
	ll = sle64_to_cpu(b->number_of_sectors) >> sectors_per_cluster_bits;
	if ((u64)ll >= 1ULL << (sizeof(unsigned long) * 8)) {
		ntfs_error(vol->sb, "Cannot handle %i-bit clusters. Sorry.",
				sizeof(unsigned long) * 4);
		return FALSE;
	}
	vol->_VCL(nr_clusters) = ll;
	ntfs_debug("vol->nr_clusters = 0x%Lx", (long long)vol->_VCL(nr_clusters));
	ll = sle64_to_cpu(b->mft_lcn);
	if (ll >= vol->_VCL(nr_clusters)) {
		ntfs_error(vol->sb, "MFT LCN is beyond end of volume. Weird.");
		return FALSE;
	}
	vol->mft_lcn = ll;
	ntfs_debug("vol->mft_lcn = 0x%Lx", (long long)vol->mft_lcn);
	ll = sle64_to_cpu(b->mftmirr_lcn);
	if (ll >= vol->_VCL(nr_clusters)) {
		ntfs_error(vol->sb, "MFTMirr LCN is beyond end of volume. "
				"Weird.");
		return FALSE;
	}
	vol->mftmirr_lcn = ll;
	ntfs_debug("vol->mftmirr_lcn = 0x%Lx", (long long)vol->mftmirr_lcn);
	vol->serial_no = le64_to_cpu(b->volume_serial_number);
	ntfs_debug("vol->serial_no = 0x%Lx",
			(unsigned long long)vol->serial_no);
	/*
	 * Determine MFT zone size. This is not strictly the right place to do
	 * this, but I am too lazy to create a function especially for it...
	 */
	vol->mft_zone_end = vol->_VCL(nr_clusters);
	switch (vol->mft_zone_multiplier) {  /* % of volume size in clusters */
	case 4:
		vol->mft_zone_end = vol->mft_zone_end >> 1;	/* 50%   */
		break;
	case 3:
		vol->mft_zone_end = (vol->mft_zone_end +
				(vol->mft_zone_end >> 1)) >> 2;	/* 37.5% */
		break;
	case 2:
		vol->mft_zone_end = vol->mft_zone_end >> 2;	/* 25%   */
		break;
	default:
		vol->mft_zone_multiplier = 1;
		/* Fall through into case 1. */
	case 1:
		vol->mft_zone_end = vol->mft_zone_end >> 3;	/* 12.5% */
		break;
	}
	ntfs_debug("vol->mft_zone_multiplier = 0x%x",
			vol->mft_zone_multiplier);
	vol->mft_zone_start = vol->mft_lcn;
	vol->mft_zone_end += vol->mft_lcn;
	ntfs_debug("vol->mft_zone_start = 0x%Lx",
			(long long)vol->mft_zone_start);
	ntfs_debug("vol->mft_zone_end = 0x%Lx", (long long)vol->mft_zone_end);
	/* And another misplaced defaults setting. */
	if (!vol->on_errors)
		vol->on_errors = ON_ERRORS_PANIC;
	return TRUE;
}

/**
 * load_and_init_upcase - load the upcase table for an ntfs volume
 * @vol:	ntfs super block describing device whose upcase to load
 *
 * Return TRUE on success or FALSE on error.
 */
static BOOL load_and_init_upcase(ntfs_volume *vol)
{
	struct super_block *sb = vol->sb;
	struct inode *ino;
	struct page *page;
	unsigned long index, max_index;
	unsigned int size;
	int i, max;

	ntfs_debug("Entering.");
	/* Read upcase table and setup vol->upcase and vol->upcase_len. */
	ino = iget(sb, FILE_UpCase);
	if (!ino || is_bad_inode(ino)) {
		if (ino)
			iput(ino);
		goto upcase_failed;
	}
	/*
	 * The upcase size must not be above 64k Unicode characters, must not
	 * be zero and must be a multiple of sizeof(uchar_t).
	 */
	if (!ino->i_size || ino->i_size & (sizeof(uchar_t) - 1) ||
			ino->i_size > 64ULL * 1024 * sizeof(uchar_t))
		goto iput_upcase_failed;
	vol->upcase = (uchar_t*)ntfs_malloc_nofs(ino->i_size);
	if (!vol->upcase)
		goto iput_upcase_failed;
	index = 0;
	max_index = ino->i_size >> PAGE_CACHE_SHIFT;
	size = PAGE_CACHE_SIZE;
	while (index < max_index) {
		/* Read the upcase table and copy it into the linear buffer. */
read_partial_upcase_page:
		page = ntfs_map_page(ino->i_mapping, index);
		if (IS_ERR(page))
			goto iput_upcase_failed;
		memcpy((char*)vol->upcase + (index++ << PAGE_CACHE_SHIFT),
				page_address(page), size);
		ntfs_unmap_page(page);
	};
	if (size == PAGE_CACHE_SIZE) {
		size = ino->i_size & ~PAGE_CACHE_MASK;
		if (size)
			goto read_partial_upcase_page;
	}
	vol->upcase_len = ino->i_size >> UCHAR_T_SIZE_BITS;
	ntfs_debug("Read %Lu bytes from $UpCase (expected %u bytes).",
			ino->i_size, 64 * 1024 * sizeof(uchar_t));
	iput(ino);
	down(&ntfs_lock);
	if (!default_upcase) {
		ntfs_debug("Using volume specified $UpCase since default is "
				"not present.");
		up(&ntfs_lock);
		return TRUE;
	}
	max = default_upcase_len;
	if (max > vol->upcase_len)
		max = vol->upcase_len;
	for (i = 0; i < max; i++)
		if (vol->upcase[i] != default_upcase[i])
			break;
	if (i == max) {
		ntfs_free(vol->upcase);
		vol->upcase = default_upcase;
		vol->upcase_len = max;
		ntfs_nr_upcase_users++;
		up(&ntfs_lock);
		ntfs_debug("Volume specified $UpCase matches default. Using "
				"default.");
		return TRUE;
	}
	up(&ntfs_lock);
	ntfs_debug("Using volume specified $UpCase since it does not match "
			"the default.");
	return TRUE;
iput_upcase_failed:
	iput(ino);
	ntfs_free(vol->upcase);
	vol->upcase = NULL;
upcase_failed:
	down(&ntfs_lock);
	if (default_upcase) {
		vol->upcase = default_upcase;
		vol->upcase_len = default_upcase_len;
		ntfs_nr_upcase_users++;
		up(&ntfs_lock);
		ntfs_error(sb, "Failed to load $UpCase from the volume. Using "
				"default.");
		return TRUE;
	}
	up(&ntfs_lock);
	ntfs_error(sb, "Failed to initialized upcase table.");
	return FALSE;
}

/**
 * load_system_files - open the system files using normal functions
 * @vol:	ntfs super block describing device whose system files to load
 *
 * Open the system files with normal access functions and complete setting up
 * the ntfs super block @vol.
 *
 * Return TRUE on success or FALSE on error.
 */
static BOOL load_system_files(ntfs_volume *vol)
{
	VCN next_vcn, last_vcn, highest_vcn;
	struct super_block *sb = vol->sb;
	struct inode *tmp_ino;
	MFT_RECORD *m;
	ATTR_RECORD *attr;
	VOLUME_INFORMATION *vi;
	attr_search_context *ctx;
	run_list_element *rl;

	ntfs_debug("Entering.");
	/*
	 * We have $MFT already (vol->mft_ino) but we need to setup access to
	 * the $MFT/$BITMAP attribute.
	 */
	m = map_mft_record(READ, NTFS_I(vol->mft_ino));
	if (IS_ERR(m)) {
		ntfs_error(sb, "Failed to map $MFT.");
		return FALSE;
	}
	if (get_attr_search_ctx(&ctx, NTFS_I(vol->mft_ino), m)) {
		ntfs_error(sb, "Failed to get attribute search context.");
		goto unmap_err_out;
	}
	/* Load all attribute extents. */
	attr = NULL;
	rl = NULL;
	next_vcn = last_vcn = highest_vcn = 0;
	while (lookup_attr(AT_BITMAP, NULL, 0, 0, next_vcn, NULL, 0, ctx)) {
		run_list_element *nrl;

		/* Cache the current attribute extent. */
		attr = ctx->attr;
		/* $MFT/$BITMAP must be non-resident. */
		if (!attr->non_resident) {
			ntfs_error(sb, "$MFT/$BITMAP must be non-resident but "
					"a resident extent was found. $MFT is "
					"corrupt. Run chkdsk.");
			goto put_err_out;
		}
		/* $MFT/$BITMAP must be uncompressed and unencrypted. */
		if (attr->flags & ATTR_COMPRESSION_MASK ||
				attr->flags & ATTR_IS_ENCRYPTED) {
			ntfs_error(sb, "$MFT/$BITMAP must be uncompressed and "
					"unencrypted but a compressed/"
					"encrypted extent was found. $MFT is "
					"corrupt. Run chkdsk.");
			goto put_err_out;
		}
		/*
		 * Decompress the mapping pairs array of this extent
		 * and merge the result into the existing run list. Note we
		 * don't need any locking at this stage as we are already
		 * running exclusively as we are mount in progress task.
		 */
		nrl = decompress_mapping_pairs(vol, attr, rl);
		if (IS_ERR(nrl)) {
			ntfs_error(sb, "decompress_mapping_pairs() failed with "
					"error code %ld. $MFT is corrupt.",
					PTR_ERR(nrl));
			goto put_err_out;
		}
		rl = nrl;

		/* Are we in the first extent? */
		if (!next_vcn) {
			/* Get the last vcn in the $BITMAP attribute. */
			last_vcn = sle64_to_cpu(attr->_ANR(allocated_size)) >>
					vol->cluster_size_bits;
			vol->mftbmp_size = sle64_to_cpu(attr->_ANR(data_size));
			vol->mftbmp_initialized_size =
					sle64_to_cpu(attr->_ANR(initialized_size));
			vol->mftbmp_allocated_size =
					sle64_to_cpu(attr->_ANR(allocated_size));
			/* Consistency check. */
			if (vol->mftbmp_size < (vol->_VMM(nr_mft_records) + 7) >> 3) {
				ntfs_error(sb, "$MFT/$BITMAP is too short to "
						"contain a complete mft "
						"bitmap: impossible. $MFT is "
						"corrupt. Run chkdsk.");
				goto put_err_out;
			}
		}

		/* Get the lowest vcn for the next extent. */
		highest_vcn = sle64_to_cpu(attr->_ANR(highest_vcn));
		next_vcn = highest_vcn + 1;

		/* Only one extent or error, which we catch below. */
		if (next_vcn <= 0)
			break;

		/* Avoid endless loops due to corruption. */
		if (next_vcn < sle64_to_cpu(attr->_ANR(lowest_vcn))) {
			ntfs_error(sb, "$MFT/$BITMAP has corrupt attribute "
					"list attribute. Run chkdsk.");
			goto put_err_out;
		}

	}
	if (!attr) {
		ntfs_error(sb, "Missing or invalid $BITMAP attribute in file "
				"$MFT. $MFT is corrupt. Run chkdsk.");
put_err_out:
		put_attr_search_ctx(ctx);
unmap_err_out:
		unmap_mft_record(READ, NTFS_I(vol->mft_ino));
		return FALSE;
	}

	/* We are finished with $MFT/$BITMAP. */
	put_attr_search_ctx(ctx);
	unmap_mft_record(READ, NTFS_I(vol->mft_ino));

	/* Catch errors. */
	if (highest_vcn && highest_vcn != last_vcn - 1) {
		ntfs_error(sb, "Failed to load the complete run list for "
				"$MFT/$BITMAP. Driver bug or corrupt $MFT. "
				"Run chkdsk.");
		ntfs_debug("highest_vcn = 0x%Lx, last_vcn - 1 = 0x%Lx",
				(long long)highest_vcn,
				(long long)last_vcn - 1);
		return FALSE;;
	}

	/* Setup the run list and the address space in the volume structure. */
	vol->mftbmp_rl.rl = rl;
	vol->mftbmp_mapping.a_ops = &ntfs_mftbmp_aops;
	
	/* Not inode data, set to NULL. Our mft bitmap access kludge... */
	vol->mftbmp_mapping.host = NULL;

	// FIXME: If mounting read-only, it would be ok to ignore errors when
	// loading the mftbmp but we then need to make sure nobody remounts the
	// volume read-write...

	/* Get mft mirror inode. */
	vol->mftmirr_ino = iget(sb, FILE_MFTMirr);
	if (!vol->mftmirr_ino || is_bad_inode(vol->mftmirr_ino)) {
		if (is_bad_inode(vol->mftmirr_ino))
			iput(vol->mftmirr_ino);
		ntfs_error(sb, "Failed to load $MFTMirr.");
		return FALSE;
	}
	// FIXME: Compare mftmirr with mft and repair if appropriate and not
	// a read-only mount.

	/* Read upcase table and setup vol->upcase and vol->upcase_len. */
	if (!load_and_init_upcase(vol))
		goto iput_mirr_err_out;
	/*
	 * Get the cluster allocation bitmap inode and verify the size, no
	 * need for any locking at this stage as we are already running
	 * exclusively as we are mount in progress task.
	 */
	vol->lcnbmp_ino = iget(sb, FILE_Bitmap);
	if (!vol->lcnbmp_ino || is_bad_inode(vol->lcnbmp_ino)) {
		if (is_bad_inode(vol->lcnbmp_ino))
			iput(vol->lcnbmp_ino);
		goto bitmap_failed;
	}
	if ((vol->_VCL(nr_lcn_bits) + 7) >> 3 > vol->lcnbmp_ino->i_size) {
		iput(vol->lcnbmp_ino);
bitmap_failed:
		ntfs_error(sb, "Failed to load $Bitmap.");
		goto iput_mirr_err_out;
	}
	/*
	 * Get the volume inode and setup our cache of the volume flags and
	 * version.
	 */
	vol->vol_ino = iget(sb, FILE_Volume);
	if (!vol->vol_ino || is_bad_inode(vol->vol_ino)) {
		if (is_bad_inode(vol->vol_ino))
			iput(vol->vol_ino);
volume_failed:
		ntfs_error(sb, "Failed to load $Volume.");
		goto iput_bmp_mirr_err_out;
	}
	m = map_mft_record(READ, NTFS_I(vol->vol_ino));
	if (IS_ERR(m)) {
iput_volume_failed:
		iput(vol->vol_ino);
		goto volume_failed;
	}
	if (get_attr_search_ctx(&ctx, NTFS_I(vol->vol_ino), m)) {
		ntfs_error(sb, "Failed to get attribute search context.");
		goto get_ctx_vol_failed;
	}
	if (!lookup_attr(AT_VOLUME_INFORMATION, NULL, 0, 0, 0, NULL, 0, ctx) ||
			ctx->attr->non_resident || ctx->attr->flags) {
err_put_vol:
		put_attr_search_ctx(ctx);
get_ctx_vol_failed:
		unmap_mft_record(READ, NTFS_I(vol->vol_ino));
		goto iput_volume_failed;
	}
	vi = (VOLUME_INFORMATION*)((char*)ctx->attr +
			le16_to_cpu(ctx->attr->_ARA(value_offset)));
	/* Some bounds checks. */
	if ((u8*)vi < (u8*)ctx->attr || (u8*)vi +
			le32_to_cpu(ctx->attr->_ARA(value_length)) > (u8*)ctx->attr +
			le32_to_cpu(ctx->attr->length))
		goto err_put_vol;
	/* Setup volume flags and version. */
	vol->vol_flags = vi->flags;
	vol->major_ver = vi->major_ver;
	vol->minor_ver = vi->minor_ver;
	put_attr_search_ctx(ctx);
	unmap_mft_record(READ, NTFS_I(vol->vol_ino));
	printk(KERN_INFO "NTFS volume version %i.%i.\n", vol->major_ver,
			vol->minor_ver);
	/*
	 * Get the inode for the logfile and empty it if this is a read-write
	 * mount.
	 */
	tmp_ino = iget(sb, FILE_LogFile);
	if (!tmp_ino || is_bad_inode(tmp_ino)) {
		if (is_bad_inode(tmp_ino))
			iput(tmp_ino);
		ntfs_error(sb, "Failed to load $LogFile.");
		// FIMXE: We only want to empty the thing so pointless bailing
		// out. Can recover/ignore.
		goto iput_vol_bmp_mirr_err_out;
	}
	// FIXME: Empty the logfile, but only if not read-only.
	// FIXME: What happens if someone remounts rw? We need to empty the file
	// then. We need a flag to tell us whether we have done it already.
	iput(tmp_ino);
	/*
	 * Get the inode for the attribute definitions file and parse the
	 * attribute definitions.
	 */ 
	tmp_ino = iget(sb, FILE_AttrDef);
	if (!tmp_ino || is_bad_inode(tmp_ino)) {
		if (is_bad_inode(tmp_ino))
			iput(tmp_ino);
		ntfs_error(sb, "Failed to load $AttrDef.");
		goto iput_vol_bmp_mirr_err_out;
	}
	// FIXME: Parse the attribute definitions.
	iput(tmp_ino);
	/* Get the root directory inode. */
	vol->root_ino = iget(sb, FILE_root);
	if (!vol->root_ino || is_bad_inode(vol->root_ino)) {
		if (is_bad_inode(vol->root_ino))
			iput(vol->root_ino);
		ntfs_error(sb, "Failed to load root directory.");
		goto iput_vol_bmp_mirr_err_out;
	}
	/* If on NTFS versions before 3.0, we are done. */
	if (vol->major_ver < 3)
		return TRUE;
	/* NTFS 3.0+ specific initialization. */
	/* Get the security descriptors inode. */
	vol->secure_ino = iget(sb, FILE_Secure);
	if (!vol->secure_ino || is_bad_inode(vol->secure_ino)) {
		if (is_bad_inode(vol->secure_ino))
			iput(vol->secure_ino);
		ntfs_error(sb, "Failed to load $Secure.");
		goto iput_root_vol_bmp_mirr_err_out;
	}
	// FIXME: Initialize security.
	/* Get the extended system files' directory inode. */
	tmp_ino = iget(sb, FILE_Extend);
	if (!tmp_ino || is_bad_inode(tmp_ino)) {
		if (is_bad_inode(tmp_ino))
			iput(tmp_ino);
		ntfs_error(sb, "Failed to load $Extend.");
		goto iput_sec_root_vol_bmp_mirr_err_out;
	}
	// FIXME: Do something. E.g. want to delete the $UsnJrnl if exists.
	// Note we might be doing this at the wrong level; we might want to
	// d_alloc_root() and then do a "normal" open(2) of $Extend\$UsnJrnl
	// rather than using iget here, as we don't know the inode number for
	// the files in $Extend directory.
	iput(tmp_ino);
	return TRUE;
iput_sec_root_vol_bmp_mirr_err_out:
	iput(vol->secure_ino);
iput_root_vol_bmp_mirr_err_out:
	iput(vol->root_ino);
iput_vol_bmp_mirr_err_out:
	iput(vol->vol_ino);
iput_bmp_mirr_err_out:
	iput(vol->lcnbmp_ino);
iput_mirr_err_out:
	iput(vol->mftmirr_ino);
	return FALSE;
}

/**
 * ntfs_put_super - called by the vfs to unmount a volume
 * @vfs_sb:	vfs superblock of volume to unmount
 *
 * ntfs_put_super() is called by the VFS (from fs/super.c::do_umount()) when
 * the volume is being unmounted (umount system call has been invoked) and it
 * releases all inodes and memory belonging to the NTFS specific part of the
 * super block.
 */
void ntfs_put_super(struct super_block *vfs_sb)
{
	ntfs_volume *vol = NTFS_SB(vfs_sb);

	ntfs_debug("Entering.");
	iput(vol->vol_ino);
	vol->vol_ino = NULL;
	/* NTFS 3.0+ specific clean up. */
	if (vol->major_ver >= 3) {
		if (vol->secure_ino) {
			iput(vol->secure_ino);
			vol->secure_ino = NULL;
		}
	}
	iput(vol->root_ino);
	vol->root_ino = NULL;
	down_write(&vol->lcnbmp_lock);
	iput(vol->lcnbmp_ino);
	vol->lcnbmp_ino = NULL;
	up_write(&vol->lcnbmp_lock);
	iput(vol->mftmirr_ino);
	vol->mftmirr_ino = NULL;
	iput(vol->mft_ino);
	vol->mft_ino = NULL;
	down_write(&vol->mftbmp_lock);
	/*
	 * Clean up mft bitmap address space. Ignore the _inode_ bit in the
	 * name of the function... FIXME: What does this do with dirty pages?
	 * (ask Al Viro)
	 */
	truncate_inode_pages(&vol->mftbmp_mapping, 0);
	vol->mftbmp_mapping.a_ops = NULL;
	vol->mftbmp_mapping.host = NULL;
	up_write(&vol->mftbmp_lock);
	write_lock(&vol->mftbmp_rl.lock);
	ntfs_free(vol->mftbmp_rl.rl);
	vol->mftbmp_rl.rl = NULL;
	write_unlock(&vol->mftbmp_rl.lock);
	vol->upcase_len = 0;
	/*
	 * Decrease the number of mounts and destroy the global default upcase
	 * table if necessary. Also decrease the number of upcase users if we
	 * are a user.
	 */
	down(&ntfs_lock);
	ntfs_nr_mounts--;
	if (vol->upcase == default_upcase) {
		ntfs_nr_upcase_users--;
		vol->upcase = NULL;
	}
	if (!ntfs_nr_upcase_users && default_upcase) {
		ntfs_free(default_upcase);
		default_upcase = NULL;
	}
	if (vol->cluster_size <= 4096 && !--ntfs_nr_compression_users)
		free_compression_buffers();
	up(&ntfs_lock);
	if (vol->upcase) {
		ntfs_free(vol->upcase);
		vol->upcase = NULL;
	}
	if (vol->nls_map) {
		unload_nls(vol->nls_map);
		vol->nls_map = NULL;
	}
	vfs_sb->u.generic_sbp = NULL;
	kfree(vol);
	return;
}

/**
 * get_nr_free_clusters - return the number of free clusters on a volume
 * @vol:	ntfs volume for which to obtain free cluster count
 *
 * Calculate the number of free clusters on the mounted NTFS volume @vol.
 *
 * Errors are ignored and we just return the number of free clusters we have
 * found. This means we return an underestimate on error.
 */
s64 get_nr_free_clusters(ntfs_volume *vol)
{
	struct address_space *mapping = vol->lcnbmp_ino->i_mapping;
	filler_t *readpage = (filler_t*)mapping->a_ops->readpage;
	struct page *page;
	unsigned long index, max_index;
	unsigned int max_size, i;
	s64 nr_free = 0LL;
	u32 *b;

	ntfs_debug("Entering.");
	/* Serialize accesses to the cluster bitmap. */
	down_read(&vol->lcnbmp_lock);
	/*
	 * Convert the number of bits into bytes rounded up, then convert into
	 * multiples of PAGE_CACHE_SIZE.
	 */
	max_index = (vol->_VCL(nr_clusters) + 7) >> (3 + PAGE_CACHE_SHIFT);
	/* Use multiples of 4 bytes. */
	max_size = PAGE_CACHE_SIZE >> 2;
	ntfs_debug("Reading $BITMAP, max_index = 0x%lx, max_size = 0x%x.",
			max_index, max_size);
	for (index = 0UL; index < max_index;) {
handle_partial_page:
		/*
		 * Read the page from page cache, getting it from backing store
		 * if necessary, and increment the use count.
		 */
		page = read_cache_page(mapping, index++, (filler_t*)readpage,
				NULL);
		/* Ignore pages which errored synchronously. */
		if (IS_ERR(page)) {
			ntfs_debug("Sync read_cache_page() error. Skipping "
					"page (index 0x%lx).", index - 1);
			continue;
		}
		wait_on_page(page);
		if (!Page_Uptodate(page)) {
			ntfs_debug("Async read_cache_page() error. Skipping "
					"page (index 0x%lx).", index - 1);
			/* Ignore pages which errored asynchronously. */
			page_cache_release(page);
			continue;
		}
		b = (u32*)kmap(page);
		/* For each 4 bytes, add up the number zero bits. */
	  	for (i = 0; i < max_size; i++)
			nr_free += (s64)(32 - hweight32(b[i]));
		kunmap(page);
		page_cache_release(page);
	}
	if (max_size == PAGE_CACHE_SIZE >> 2) {
		/*
		 * Get the multiples of 4 bytes in use in the final partial
		 * page.
		 */
		max_size = ((((vol->_VCL(nr_clusters) + 7) >> 3) & ~PAGE_CACHE_MASK)
				+ 3) >> 2;
		/* If there is a partial page go back and do it. */
		if (max_size) {
			ntfs_debug("Handling partial page, max_size = 0x%x.",
					max_size);
			goto handle_partial_page;
		}
	}
	ntfs_debug("Finished reading $BITMAP, last index = 0x%lx", index - 1);
	up_read(&vol->lcnbmp_lock);
	ntfs_debug("Exiting.");
	return nr_free;
}

/**
 * get_nr_free_mft_records - return the number of free inodes on a volume
 * @vol:	ntfs volume for which to obtain free inode count
 *
 * Calculate the number of free mft records (inodes) on the mounted NTFS
 * volume @vol.
 *
 * Errors are ignored and we just return the number of free inodes we have
 * found. This means we return an underestimate on error.
 */
s64 get_nr_free_mft_records(ntfs_volume *vol)
{
	struct address_space *mapping;
	filler_t *readpage;
	struct page *page;
	unsigned long index, max_index;
	unsigned int max_size, i;
	s64 nr_free = 0LL;
	u32 *b;

	ntfs_debug("Entering.");
	/* Serialize accesses to the inode bitmap. */
	down_read(&vol->mftbmp_lock);
	mapping = &vol->mftbmp_mapping;
	readpage = (filler_t*)mapping->a_ops->readpage;
	/*
	 * Convert the number of bits into bytes rounded up, then convert into
	 * multiples of PAGE_CACHE_SIZE.
	 */
	max_index = (vol->_VMM(nr_mft_records) + 7) >> (3 + PAGE_CACHE_SHIFT);
	/* Use multiples of 4 bytes. */
	max_size = PAGE_CACHE_SIZE >> 2;
	ntfs_debug("Reading $MFT/$BITMAP, max_index = 0x%lx, max_size = "
			"0x%x.", max_index, max_size);
	for (index = 0UL; index < max_index;) {
handle_partial_page:
		/*
		 * Read the page from page cache, getting it from backing store
		 * if necessary, and increment the use count.
		 */
		page = read_cache_page(mapping, index++, (filler_t*)readpage,
				vol);
		/* Ignore pages which errored synchronously. */
		if (IS_ERR(page)) {
			ntfs_debug("Sync read_cache_page() error. Skipping "
					"page (index 0x%lx).", index - 1);
			continue;
		}
		wait_on_page(page);
		if (!Page_Uptodate(page)) {
			ntfs_debug("Async read_cache_page() error. Skipping "
					"page (index 0x%lx).", index - 1);
			/* Ignore pages which errored asynchronously. */
			page_cache_release(page);
			continue;
		}
		b = (u32*)kmap(page);
		/* For each 4 bytes, add up the number of zero bits. */
	  	for (i = 0; i < max_size; i++)
			nr_free += (s64)(32 - hweight32(b[i]));
		kunmap(page);
		page_cache_release(page);
	}
	if (index == max_index) {
		/*
		 * Get the multiples of 4 bytes in use in the final partial
		 * page.
		 */
		max_size = ((((vol->_VMM(nr_mft_records) + 7) >> 3) &
				~PAGE_CACHE_MASK) + 3) >> 2;
		/* If there is a partial page go back and do it. */
		if (max_size) {
			/* Compensate for out of bounds zero bits. */
			if ((i = vol->_VMM(nr_mft_records) & 31))
				nr_free -= (s64)(32 - i);
			ntfs_debug("Handling partial page, max_size = 0x%x",
					max_size);
			goto handle_partial_page;
		}
	}
	ntfs_debug("Finished reading $MFT/$BITMAP, last index = 0x%lx",
			index - 1);
	up_read(&vol->mftbmp_lock);
	ntfs_debug("Exiting.");
	return nr_free;
}

/**
 * ntfs_statfs - return information about mounted NTFS volume
 * @sb:		super block of mounted volume
 * @sfs:	statfs structure in which to return the information
 *
 * Return information about the mounted NTFS volume @sb in the statfs structure
 * pointed to by @sfs (this is initialized with zeros before ntfs_statfs is
 * called). We interpret the values to be correct of the moment in time at
 * which we are called. Most values are variable otherwise and this isn't just
 * the free values but the totals as well. For example we can increase the
 * total number of file nodes if we run out and we can keep doing this until
 * there is no more space on the volume left at all.
 *
 * Called from vfs_statfs which is used to handle the statfs, fstatfs, and
 * ustat system calls.
 *
 * Return 0 on success or -errno on error.
 */
int ntfs_statfs(struct super_block *sb, struct statfs *sfs)
{
	ntfs_volume *vol = NTFS_SB(sb);
	s64 size;

	ntfs_debug("Entering.");
	/* Type of filesystem. */
	sfs->f_type   = NTFS_SB_MAGIC;
	/* Optimal transfer block size. */
	sfs->f_bsize  = PAGE_CACHE_SIZE;
	/*
	 * Total data blocks in file system in units of f_bsize and since
	 * inodes are also stored in data blocs ($MFT is a file) this is just
	 * the total clusters.
	 */
	sfs->f_blocks = vol->_VCL(nr_clusters) << vol->cluster_size_bits >>
				PAGE_CACHE_SHIFT;
	/* Free data blocks in file system in units of f_bsize. */
	size	      = get_nr_free_clusters(vol) << vol->cluster_size_bits >>
				PAGE_CACHE_SHIFT;
	if (size < 0LL)
		size = 0LL;
	/* Free blocks avail to non-superuser, same as above on NTFS. */
	sfs->f_bavail = sfs->f_bfree = size;
	/* Total file nodes in file system (at this moment in time). */
	sfs->f_files  = vol->mft_ino->i_size >> vol->mft_record_size_bits;
	/* Free file nodes in fs (based on current total count). */
	size	      = get_nr_free_mft_records(vol);
	if (size < 0LL)
		size = 0LL;
	sfs->f_ffree = size;
	/*
	 * File system id. This is extremely *nix flavour dependent and even
	 * within Linux itself all fs do their own thing. I interpret this to
	 * mean a unique id associated with the mounted fs and not the id
	 * associated with the file system driver, the latter is already given
	 * by the file system type in sfs->f_type. Thus we use the 64-bit
	 * volume serial number splitting it into two 32-bit parts. We enter
	 * the least significant 32-bits in f_fsid[0] and the most significant
	 * 32-bits in f_fsid[1].
	 */
	sfs->f_fsid.val[0] = vol->serial_no & 0xffffffff;
	sfs->f_fsid.val[1] = (vol->serial_no >> 32) & 0xffffffff;
	/* Maximum length of filenames. */
	sfs->f_namelen	   = NTFS_MAX_NAME_LEN;
	return 0;
}

/**
 * Super operations for mount time when we don't have enough setup to use the
 * proper functions.
 */
struct super_operations ntfs_mount_sops = {
	alloc_inode:	ntfs_alloc_big_inode,	/* VFS: Allocate a new inode. */
	destroy_inode:	ntfs_destroy_big_inode,	/* VFS: Deallocate an inode. */
	read_inode:	ntfs_read_inode_mount,	/* VFS: Load inode from disk,
						   called from iget(). */
	clear_inode:	ntfs_clear_big_inode,	/* VFS: Called when an inode is
						   removed from memory. */
};

/**
 * The complete super operations.
 */
struct super_operations ntfs_sops = {
	alloc_inode:	ntfs_alloc_big_inode,	/* VFS: Allocate a new inode. */
	destroy_inode:	ntfs_destroy_big_inode,	/* VFS: Deallocate an inode. */
	read_inode:	ntfs_read_inode,	/* VFS: Load inode from disk,
						   called from iget(). */
	dirty_inode:	ntfs_dirty_inode,	/* VFS: Called from
						   __mark_inode_dirty(). */
	write_inode:	NULL,		/* VFS: Write dirty inode to disk. */
	put_inode:	NULL,		/* VFS: Called whenever the reference
					   count (i_count) of the inode is
					   going to be decreased but before the
					   actual decrease. */
	delete_inode:	NULL,		/* VFS: Delete inode from disk. Called
					   when i_count becomes 0 and i_nlink is
					   also 0. */
	put_super:	ntfs_put_super,	/* Syscall: umount. */
	write_super:	NULL,		/* Flush dirty super block to disk. */
	write_super_lockfs:	NULL,	/* ? */
	unlockfs:	NULL,		/* ? */
	statfs:		ntfs_statfs,	/* Syscall: statfs */
	remount_fs:	ntfs_remount,	/* Syscall: mount -o remount. */
	clear_inode:	ntfs_clear_big_inode,	/* VFS: Called when an inode is
						   removed from memory. */
	umount_begin:	NULL,		/* Forced umount. */
	/*
	 * These are NFSd support functions but NTFS is a standard fs so
	 * shouldn't need to implement these manually. At least we can try
	 * without and if it doesn't work in some way we can always implement
	 * something here.
	 */
	fh_to_dentry:	NULL,		/* Get dentry for given file handle. */
	dentry_to_fh:	NULL,		/* Get file handle for given dentry. */
	show_options:	ntfs_show_options, /* Show mount options in proc. */
};

/**
 * ntfs_fill_super - mount an ntfs files system
 * @sb:		super block of ntfs file system to mount
 * @opt:	string containing the mount options
 * @silent:	silence error output
 *
 * ntfs_fill_super() is called by the VFS to mount the device described by @sb
 * with the mount otions in @data with the NTFS file system.
 *
 * If @silent is true, remain silent even if errors are detected. This is used
 * during bootup, when the kernel tries to mount the root file system with all
 * registered file systems one after the other until one succeeds. This implies
 * that all file systems except the correct one will quite correctly and
 * expectedly return an error, but nobody wants to see error messages when in
 * fact this is what is supposed to happen.
 *
 * NOTE: @sb->s_flags contains the mount options flags.
 */
static int ntfs_fill_super(struct super_block *sb, void *opt, const int silent)
{
	extern int *blksize_size[];
	ntfs_volume *vol;
	struct buffer_head *bh;
	struct inode *tmp_ino;
	int old_blocksize, result;
	kdev_t dev = sb->s_dev;

	ntfs_debug("Entering.");
	/* Allocate a new ntfs_volume and place it in sb->u.generic_sbp. */
	sb->u.generic_sbp = kmalloc(sizeof(ntfs_volume), GFP_NOFS);
	vol = NTFS_SB(sb);
	if (!vol) {
		if (!silent)
			ntfs_error(sb, "Allocation of NTFS volume structure "
					"failed. Aborting mount...");
		return -ENOMEM;
	}
	/* Initialize ntfs_volume structure. */
	memset(vol, 0, sizeof(ntfs_volume));
	vol->sb = sb;
	vol->upcase = NULL;
	vol->mft_ino = NULL;
	init_rwsem(&vol->mftbmp_lock);
	INIT_LIST_HEAD(&vol->mftbmp_mapping.clean_pages);
	INIT_LIST_HEAD(&vol->mftbmp_mapping.dirty_pages);
	INIT_LIST_HEAD(&vol->mftbmp_mapping.locked_pages);
	vol->mftbmp_mapping.a_ops = NULL;
	vol->mftbmp_mapping.host = NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,6)
	vol->mftbmp_mapping.i_mmap = NULL;
	vol->mftbmp_mapping.i_mmap_shared = NULL;
#else
	INIT_LIST_HEAD(&vol->mftbmp_mapping.i_mmap);
	INIT_LIST_HEAD(&vol->mftbmp_mapping.i_mmap_shared);
#endif
	spin_lock_init(&vol->mftbmp_mapping.i_shared_lock);
	INIT_RUN_LIST(&vol->mftbmp_rl);
	vol->mftmirr_ino = NULL;
	vol->lcnbmp_ino = NULL;
	init_rwsem(&vol->lcnbmp_lock);
	vol->vol_ino = NULL;
	vol->root_ino = NULL;
	vol->secure_ino = NULL;
	vol->uid = vol->gid = 0;
	vol->on_errors = 0;
	vol->mft_zone_multiplier = 0;
	vol->nls_map = NULL;

	/*
	 * Default is group and other don't have write/execute access to files
	 * and write access to directories.
	 */
	vol->fmask = 0033;
	vol->dmask = 0022;

	/*
	 * Default is to show long file names (including POSIX file names), and
	 * not to show system files and short file names.
	 */
	vol->readdir_opts = SHOW_WIN32;

	/* Important to get the mount options dealt with now. */
	if (!parse_options(vol, (char*)opt))
		goto err_out_now;
	
	/* We are just a read-only fs at the moment. */
	sb->s_flags |= MS_RDONLY | MS_NOATIME | MS_NODIRATIME;

	/*
	 * TODO: Fail safety check. In the future we should really be able to
	 * cope with this being the case, but for now just bail out.
	 */
	if (get_hardsect_size(dev) > NTFS_BLOCK_SIZE) {
		if (!silent)
			ntfs_error(sb, "Device has unsupported hardsect_size.");
		goto err_out_now;
	}
	
	/* Setup the device access block size to NTFS_BLOCK_SIZE. */
	if (!blksize_size[major(dev)])
		old_blocksize = BLOCK_SIZE;
	else
		old_blocksize = blksize_size[major(dev)][minor(dev)];
	if (sb_set_blocksize(sb, NTFS_BLOCK_SIZE) != NTFS_BLOCK_SIZE) {
		if (!silent)
			ntfs_error(sb, "Unable to set block size.");
		goto set_blk_size_err_out_now;
	}

	/* Get the size of the device in units of NTFS_BLOCK_SIZE bytes. */
	vol->nr_blocks = sb->s_bdev->bd_inode->i_size >> NTFS_BLOCK_SIZE_BITS;

	/* Read the boot sector and return unlocked buffer head to it. */
	if (!(bh = read_ntfs_boot_sector(sb, silent))) {
		if (!silent)
			ntfs_error(sb, "Not an NTFS volume.");
		goto set_blk_size_err_out_now;
	}
	
	/*
	 * Extract the data from the boot sector and setup the ntfs super block
	 * using it.
	 */
	result = parse_ntfs_boot_sector(vol, (NTFS_BOOT_SECTOR*)bh->b_data);

	brelse(bh);

	if (!result) {
		if (!silent)
			ntfs_error(sb, "Unsupported NTFS filesystem.");
		goto set_blk_size_err_out_now;
	}

	/* 
	 * TODO: When we start coping with sector sizes different from
	 * NTFS_BLOCK_SIZE, we now probably need to set the blocksize of the
	 * device (probably to NTFS_BLOCK_SIZE).
	 */

	/* Setup remaining fields in the super block. */
	sb->s_magic = NTFS_SB_MAGIC;

	/*
	 * Ntfs allows 63 bits for the file size, i.e. correct would be:
	 * 	sb->s_maxbytes = ~0ULL >> 1;
	 * But the kernel uses a long as the page cache page index which on
	 * 32-bit architectures is only 32-bits. MAX_LFS_FILESIZE is kernel
	 * defined to the maximum the page cache page index can cope with
	 * without overflowing the index or to 2^63 - 1, whichever is smaller.
	 */
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	/*
	 * Now load the metadata required for the page cache and our address
	 * space operations to function. We do this by setting up a specialised
	 * read_inode method and then just calling iget() to obtain the inode
	 * for $MFT which is sufficient to allow our normal inode operations
	 * and associated address space operations to function.
	 */
	/*
	 * Poison vol->mft_ino so we know whether iget() called into our
	 * ntfs_read_inode_mount() method.
	 */
#define OGIN	((struct inode*)le32_to_cpu(0x4e49474f))	/* OGIN */
	vol->mft_ino = OGIN;
	sb->s_op = &ntfs_mount_sops;
	tmp_ino = iget(vol->sb, FILE_MFT);
	if (!tmp_ino || tmp_ino != vol->mft_ino || is_bad_inode(tmp_ino)) {
		if (!silent)
			ntfs_error(sb, "Failed to load essential metadata.");
		if (tmp_ino && vol->mft_ino == OGIN)
			ntfs_error(sb, "BUG: iget() did not call "
					"ntfs_read_inode_mount() method!\n");
		if (!tmp_ino)
			goto cond_iput_mft_ino_err_out_now;
		goto iput_tmp_ino_err_out_now;
	}
	/*
	 * Note: sb->s_op has already been set to &ntfs_sops by our specialized
	 * ntfs_read_inode_mount() method when it was invoked by iget().
	 */

	down(&ntfs_lock);

	/*
	 * The current mount is a compression user if the cluster size is
	 * less than or equal 4kiB.
	 */
	if (vol->cluster_size <= 4096 && !ntfs_nr_compression_users++) {
		result = allocate_compression_buffers();
		if (result) {
			ntfs_error(NULL, "Failed to allocate per CPU buffers "
					"for compression engine.");
			ntfs_nr_compression_users--;
			up(&ntfs_lock);
			goto iput_tmp_ino_err_out_now;
		}
	}

	/*
	 * Increment the number of mounts and generate the global default
	 * upcase table if necessary. Also temporarily increment the number of
	 * upcase users to avoid race conditions with concurrent (u)mounts.
	 */
	if (!ntfs_nr_mounts++)
		default_upcase = generate_default_upcase();
	ntfs_nr_upcase_users++;

	up(&ntfs_lock);

	/*
	 * From now on, ignore @silent parameter. If we fail below this line,
	 * it will be due to a corrupt fs or a system error, so we report it.
	 */

	/*
	 * Open the system files with normal access functions and complete
	 * setting up the ntfs super block.
	 */
	if (!load_system_files(vol)) {
		ntfs_error(sb, "Failed to load system files.");
		goto unl_upcase_iput_tmp_ino_err_out_now;
	}

	if ((sb->s_root = d_alloc_root(vol->root_ino))) {
		/* We increment i_count simulating an iget(). */
		atomic_inc(&vol->root_ino->i_count);
		ntfs_debug("Exiting, status successful.");
		/* Release the default upcase if it has no users. */
		down(&ntfs_lock);
		if (!--ntfs_nr_upcase_users && default_upcase) {
			ntfs_free(default_upcase);
			default_upcase = NULL;
		}
		up(&ntfs_lock);
		return 0;
	}
	ntfs_error(sb, "Failed to allocate root directory.");
	/* Clean up after the successful load_system_files() call from above. */
	iput(vol->vol_ino);
	vol->vol_ino = NULL;
	/* NTFS 3.0+ specific clean up. */
	if (vol->major_ver >= 3) {
		iput(vol->secure_ino);
		vol->secure_ino = NULL;
	}
	iput(vol->root_ino);
	vol->root_ino = NULL;
	iput(vol->lcnbmp_ino);
	vol->lcnbmp_ino = NULL;
	iput(vol->mftmirr_ino);
	vol->mftmirr_ino = NULL;
	truncate_inode_pages(&vol->mftbmp_mapping, 0);
	vol->mftbmp_mapping.a_ops = NULL;
	vol->mftbmp_mapping.host = NULL;
	ntfs_free(vol->mftbmp_rl.rl);
	vol->mftbmp_rl.rl = NULL;
	vol->upcase_len = 0;
	if (vol->upcase != default_upcase)
		ntfs_free(vol->upcase);
	vol->upcase = NULL;
	if (vol->nls_map) {
		unload_nls(vol->nls_map);
		vol->nls_map = NULL;
	}
	/* Error exit code path. */
unl_upcase_iput_tmp_ino_err_out_now:
	/*
	 * Decrease the number of mounts and destroy the global default upcase
	 * table if necessary.
	 */
	down(&ntfs_lock);
	ntfs_nr_mounts--;
	if (!--ntfs_nr_upcase_users && default_upcase) {
		ntfs_free(default_upcase);
		default_upcase = NULL;
	}
	if (vol->cluster_size <= 4096 && !--ntfs_nr_compression_users)
		free_compression_buffers();
	up(&ntfs_lock);
iput_tmp_ino_err_out_now:
	iput(tmp_ino);
cond_iput_mft_ino_err_out_now:
	if (vol->mft_ino && vol->mft_ino != OGIN && vol->mft_ino != tmp_ino) {
		iput(vol->mft_ino);
		vol->mft_ino = NULL;
	}
#undef OGIN
	/*
	 * This is needed to get ntfs_clear_inode() called for each inode we
	 * have ever called iget()/iput() on, otherwise we A) leak resources
	 * and B) a subsequent mount fails automatically due to iget() never
	 * calling down into our ntfs_read_inode{_mount}() methods again...
	 */
	if (invalidate_inodes(sb)) {
		ntfs_error(sb, "Busy inodes left. This is most likely a NTFS "
				"driver bug.");
		/* Copied from fs/super.c. I just love this message. (-; */
		printk("VFS: Busy inodes after umount. Self-destruct in 5 "
				"seconds.  Have a nice day...\n");
	}
set_blk_size_err_out_now:
	/* Errors at this stage are irrelevant. */
	sb_set_blocksize(sb, old_blocksize);
err_out_now:
	sb->u.generic_sbp = NULL;
	kfree(vol);
	ntfs_debug("Failed, returning -EINVAL.");
	return -EINVAL;
}

/*
 * This is a slab cache to optimize allocations and deallocations of Unicode
 * strings of the maximum length allowed by NTFS, which is NTFS_MAX_NAME_LEN
 * (255) Unicode characters + a terminating NULL Unicode character.
 */
kmem_cache_t *ntfs_name_cache;

/* Slab caches for efficient allocation/deallocation of of inodes. */
kmem_cache_t *ntfs_inode_cache;
kmem_cache_t *ntfs_big_inode_cache;

/* Init once constructor for the inode slab cache. */
static void ntfs_big_inode_init_once(void *foo, kmem_cache_t *cachep,
		unsigned long flags)
{
	ntfs_inode *ni = (ntfs_inode *)foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
			SLAB_CTOR_CONSTRUCTOR)
		inode_init_once(VFS_I(ni));
}

/*
 * Slab cache to optimize allocations and deallocations of attribute search
 * contexts.
 */
kmem_cache_t *ntfs_attr_ctx_cache;

/* A global default upcase table and a corresponding reference count. */
wchar_t *default_upcase = NULL;
unsigned long ntfs_nr_upcase_users = 0;

/* The number of mounted filesystems. */
unsigned long ntfs_nr_mounts = 0;

/* Driver wide semaphore. */
DECLARE_MUTEX(ntfs_lock);

static struct super_block *ntfs_get_sb(struct file_system_type *fs_type,
	int flags, char *dev_name, void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, ntfs_fill_super);
}

static struct file_system_type ntfs_fs_type = {
	owner:		THIS_MODULE,
	name:		"ntfs",
	get_sb:		ntfs_get_sb,
	fs_flags:	FS_REQUIRES_DEV,
};

/* Stable names for the slab caches. */
static const char *ntfs_attr_ctx_cache_name = "ntfs_attr_ctx_cache";
static const char *ntfs_name_cache_name = "ntfs_name_cache";
static const char *ntfs_inode_cache_name = "ntfs_inode_cache";
static const char *ntfs_big_inode_cache_name = "ntfs_big_inode_cache";

static int __init init_ntfs_fs(void)
{
	int err = 0;

	/* This may be ugly but it results in pretty output so who cares. (-8 */
	printk(KERN_INFO "NTFS driver " NTFS_VERSION " [Flags: R/"
#ifdef CONFIG_NTFS_RW
			"W"
#else
			"O"
#endif
#ifdef DEBUG
			" DEBUG"
#endif
#ifdef MODULE
			" MODULE"
#endif
			"]. Copyright (c) 2001 Anton Altaparmakov.\n");

	ntfs_debug("Debug messages are enabled.");

	ntfs_attr_ctx_cache = kmem_cache_create(ntfs_attr_ctx_cache_name,
			sizeof(attr_search_context), 0 /* offset */,
			SLAB_HWCACHE_ALIGN, NULL /* ctor */, NULL /* dtor */);
	if (!ntfs_attr_ctx_cache) {
		printk(KERN_CRIT "NTFS: Failed to create %s!\n",
				ntfs_attr_ctx_cache_name);
		goto ctx_err_out;
	}

	ntfs_name_cache = kmem_cache_create(ntfs_name_cache_name,
			(NTFS_MAX_NAME_LEN+1) * sizeof(uchar_t), 0,
			SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!ntfs_name_cache) {
		printk(KERN_CRIT "NTFS: Failed to create %s!\n",
				ntfs_name_cache_name);
		goto name_err_out;
	}

	ntfs_inode_cache = kmem_cache_create(ntfs_inode_cache_name,
			sizeof(ntfs_inode), 0, SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!ntfs_inode_cache) {
		printk(KERN_CRIT "NTFS: Failed to create %s!\n",
				ntfs_inode_cache_name);
		goto inode_err_out;
	}

	ntfs_big_inode_cache = kmem_cache_create(ntfs_big_inode_cache_name,
			sizeof(big_ntfs_inode), 0, SLAB_HWCACHE_ALIGN,
			ntfs_big_inode_init_once, NULL);
	if (!ntfs_big_inode_cache) {
		printk(KERN_CRIT "NTFS: Failed to create %s!\n",
				ntfs_big_inode_cache_name);
		goto big_inode_err_out;
	}

	/* Register the ntfs sysctls. */
	err = ntfs_sysctl(1);
	if (err) {
		printk(KERN_CRIT "NTFS: Failed to register NTFS sysctls!\n");
		goto sysctl_err_out;
	}

	err = register_filesystem(&ntfs_fs_type);
	if (!err) {
		ntfs_debug("NTFS driver registered successfully.");
		return 0; /* Success! */
	}
	printk(KERN_CRIT "NTFS: Failed to register NTFS file system driver!\n");

sysctl_err_out:
	kmem_cache_destroy(ntfs_big_inode_cache);
big_inode_err_out:
	kmem_cache_destroy(ntfs_inode_cache);
inode_err_out:
	kmem_cache_destroy(ntfs_name_cache);
name_err_out:
	kmem_cache_destroy(ntfs_attr_ctx_cache);
ctx_err_out:
	if (!err) {
		printk(KERN_CRIT "NTFS: Aborting NTFS file system driver "
				"registration...\n");
		err = -ENOMEM;
	}
	return err;
}

static void __exit exit_ntfs_fs(void)
{
	int err = 0;

	ntfs_debug("Unregistering NTFS driver.");

	unregister_filesystem(&ntfs_fs_type);

	if (kmem_cache_destroy(ntfs_big_inode_cache) && (err = 1))
		printk(KERN_CRIT "NTFS: Failed to destory %s.\n",
				ntfs_big_inode_cache_name);
	if (kmem_cache_destroy(ntfs_inode_cache) && (err = 1))
		printk(KERN_CRIT "NTFS: Failed to destory %s.\n",
				ntfs_inode_cache_name);
	if (kmem_cache_destroy(ntfs_name_cache) && (err = 1))
		printk(KERN_CRIT "NTFS: Failed to destory %s.\n",
				ntfs_name_cache_name);
	if (kmem_cache_destroy(ntfs_attr_ctx_cache) && (err = 1))
		printk(KERN_CRIT "NTFS: Failed to destory %s.\n",
				ntfs_attr_ctx_cache_name);
	if (err)
		printk(KERN_CRIT "NTFS: This causes memory to leak! There is "
				"probably a BUG in the driver! Please report "
				"you saw this message to "
				"linux-ntfs-dev@lists.sf.net\n");
	/* Unregister the ntfs sysctls. */
	ntfs_sysctl(0);
}

EXPORT_NO_SYMBOLS;
MODULE_AUTHOR("Anton Altaparmakov <aia21@cam.ac.uk>");
MODULE_DESCRIPTION("NTFS 1.2/3.x driver");
MODULE_LICENSE("GPL");
#ifdef DEBUG
MODULE_PARM(debug_msgs, "i");
MODULE_PARM_DESC(debug_msgs, "Enable debug messages.");
#endif

module_init(init_ntfs_fs)
module_exit(exit_ntfs_fs)

