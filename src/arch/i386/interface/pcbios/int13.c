/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <byteswap.h>
#include <errno.h>
#include <assert.h>
#include <ipxe/list.h>
#include <ipxe/blockdev.h>
#include <ipxe/io.h>
#include <ipxe/open.h>
#include <ipxe/uri.h>
#include <ipxe/process.h>
#include <ipxe/xfer.h>
#include <ipxe/retry.h>
#include <ipxe/timer.h>
#include <ipxe/acpi.h>
#include <ipxe/sanboot.h>
#include <realmode.h>
#include <bios.h>
#include <biosint.h>
#include <bootsector.h>
#include <int13.h>

/** @file
 *
 * INT 13 emulation
 *
 * This module provides a mechanism for exporting block devices via
 * the BIOS INT 13 disk interrupt interface.  
 *
 */

/**
 * Overall timeout for INT 13 commands (independent of underlying device
 *
 * Underlying devices should ideally never become totally stuck.
 * However, if they do, then the INT 13 mechanism provides no means
 * for the caller to cancel the operation, and the machine appears to
 * hang.  Use an overall timeout for all commands to avoid this
 * problem and bounce timeout failures to the caller.
 */
#define INT13_COMMAND_TIMEOUT ( 15 * TICKS_PER_SEC )

/** An INT 13 emulated drive */
struct int13_drive {
	/** Reference count */
	struct refcnt refcnt;
	/** List of all registered drives */
	struct list_head list;

	/** Block device URI */
	struct uri *uri;
	/** Underlying block device interface */
	struct interface block;

	/** BIOS in-use drive number (0x80-0xff) */
	unsigned int drive;
	/** BIOS natural drive number (0x80-0xff)
	 *
	 * This is the drive number that would have been assigned by
	 * 'naturally' appending the drive to the end of the BIOS
	 * drive list.
	 *
	 * If the emulated drive replaces a preexisting drive, this is
	 * the drive number that the preexisting drive gets remapped
	 * to.
	 */
	unsigned int natural_drive;

	/** Block device capacity */
	struct block_device_capacity capacity;

	/** Number of cylinders
	 *
	 * The cylinder number field in an INT 13 call is ten bits
	 * wide, giving a maximum of 1024 cylinders.  Conventionally,
	 * when the 7.8GB limit of a CHS address is exceeded, it is
	 * the number of cylinders that is increased beyond the
	 * addressable limit.
	 */
	unsigned int cylinders;
	/** Number of heads
	 *
	 * The head number field in an INT 13 call is eight bits wide,
	 * giving a maximum of 256 heads.  However, apparently all
	 * versions of MS-DOS up to and including Win95 fail with 256
	 * heads, so the maximum encountered in practice is 255.
	 */
	unsigned int heads;
	/** Number of sectors per track
	 *
	 * The sector number field in an INT 13 call is six bits wide,
	 * giving a maximum of 63 sectors, since sector numbering
	 * (unlike head and cylinder numbering) starts at 1, not 0.
	 */
	unsigned int sectors_per_track;

	/** Underlying device status, if in error */
	int block_rc;
	/** Status of last operation */
	int last_status;
};

/** Vector for chaining to other INT 13 handlers */
static struct segoff __text16 ( int13_vector );
#define int13_vector __use_text16 ( int13_vector )

/** Assembly wrapper */
extern void int13_wrapper ( void );

/** List of registered emulated drives */
static LIST_HEAD ( int13s );

/**
 * Number of BIOS drives
 *
 * Note that this is the number of drives in the system as a whole
 * (i.e. a mirror of the counter at 40:75), rather than a count of the
 * number of emulated drives.
 */
static uint8_t num_drives;

/** An INT 13 command */
struct int13_command {
	/** Status */
	int rc;
	/** INT 13 drive */
	struct int13_drive *int13;
	/** Underlying block device interface */
	struct interface block;
	/** Command timeout timer */
	struct retry_timer timer;
};

/**
 * Record INT 13 drive capacity
 *
 * @v command		INT 13 command
 * @v capacity		Block device capacity
 */
static void int13_command_capacity ( struct int13_command *command,
				     struct block_device_capacity *capacity ) {
	memcpy ( &command->int13->capacity, capacity,
		 sizeof ( command->int13->capacity ) );
}

/**
 * Close INT 13 command
 *
 * @v command		INT 13 command
 * @v rc		Reason for close
 */
static void int13_command_close ( struct int13_command *command, int rc ) {
	intf_restart ( &command->block, rc );
	stop_timer ( &command->timer );
	command->rc = rc;
}

/**
 * Handle INT 13 command timer expiry
 *
 * @v timer		Timer
 */
static void int13_command_expired ( struct retry_timer *timer,
				    int over __unused ) {
	struct int13_command *command =
		container_of ( timer, struct int13_command, timer );

	int13_command_close ( command, -ETIMEDOUT );
}

/** INT 13 command interface operations */
static struct interface_operation int13_command_op[] = {
	INTF_OP ( intf_close, struct int13_command *, int13_command_close ),
	INTF_OP ( block_capacity, struct int13_command *,
		  int13_command_capacity ),
};

/** INT 13 command interface descriptor */
static struct interface_descriptor int13_command_desc =
	INTF_DESC ( struct int13_command, block, int13_command_op );

/**
 * Prepare to issue INT 13 command
 *
 * @v command		INT 13 command
 * @v int13		Emulated drive
 * @ret rc		Return status code
 */
static int int13_command_start ( struct int13_command *command,
				 struct int13_drive *int13 ) {

	/* Sanity check */
	assert ( command->int13 == NULL );
	assert ( ! timer_running ( &command->timer ) );

	/* Initialise command */
	command->rc = -EINPROGRESS;
	command->int13 = int13;
	start_timer_fixed ( &command->timer, INT13_COMMAND_TIMEOUT );

	/* Wait for block control interface to become ready */
	while ( ( command->rc == -EINPROGRESS ) &&
		( xfer_window ( &int13->block ) == 0 ) ) {
		step();
	}

	return ( ( command->rc == -EINPROGRESS ) ?
		 int13->block_rc : command->rc );
}

/**
 * Wait for INT 13 command to complete
 *
 * @v command		INT 13 command
 * @ret rc		Return status code
 */
static int int13_command_wait ( struct int13_command *command ) {

	/* Sanity check */
	assert ( timer_running ( &command->timer ) );

	/* Wait for command to complete */
	while ( command->rc == -EINPROGRESS )
		step();

	assert ( ! timer_running ( &command->timer ) );
	return command->rc;
}

/**
 * Terminate INT 13 command
 *
 * @v command		INT 13 command
 */
static void int13_command_stop ( struct int13_command *command ) {
	stop_timer ( &command->timer );
	command->int13 = NULL;
}

/** The single active INT 13 command */
static struct int13_command int13_command = {
	.block = INTF_INIT ( int13_command_desc ),
	.timer = TIMER_INIT ( int13_command_expired ),
};

/**
 * Read from or write to INT 13 drive
 *
 * @v int13		Emulated drive
 * @v lba		Starting logical block address
 * @v count		Number of logical blocks
 * @v buffer		Data buffer
 * @v block_rw		Block read/write method
 * @ret rc		Return status code
 */
static int int13_rw ( struct int13_drive *int13, uint64_t lba,
		      unsigned int count, userptr_t buffer,
		      int ( * block_rw ) ( struct interface *control,
					   struct interface *data,
					   uint64_t lba, unsigned int count,
					   userptr_t buffer, size_t len ) ) {
	struct int13_command *command = &int13_command;
	unsigned int frag_count;
	size_t frag_len;
	int rc;

	while ( count ) {

		/* Determine fragment length */
		frag_count = count;
		if ( frag_count > int13->capacity.max_count )
			frag_count = int13->capacity.max_count;
		frag_len = ( int13->capacity.blksize * frag_count );

		/* Issue command */
		if ( ( ( rc = int13_command_start ( command, int13 ) ) != 0 ) ||
		     ( ( rc = block_rw ( &int13->block, &command->block, lba,
					 frag_count, buffer,
					 frag_len ) ) != 0 ) ||
		     ( ( rc = int13_command_wait ( command ) ) != 0 ) ) {
			int13_command_stop ( command );
			return rc;
		}
		int13_command_stop ( command );

		/* Move to next fragment */
		lba += frag_count;
		count -= frag_count;
		buffer = userptr_add ( buffer, frag_len );
	}

	return 0;
}

/**
 * Read INT 13 drive capacity
 *
 * @v int13		Emulated drive
 * @ret rc		Return status code
 */
static int int13_read_capacity ( struct int13_drive *int13 ) {
	struct int13_command *command = &int13_command;
	int rc;

	/* Issue command */
	if ( ( ( rc = int13_command_start ( command, int13 ) ) != 0 ) ||
	     ( ( rc = block_read_capacity ( &int13->block,
					    &command->block ) ) != 0 ) ||
	     ( ( rc = int13_command_wait ( command ) ) != 0 ) ) {
		int13_command_stop ( command );
		return rc;
	}

	int13_command_stop ( command );
	return 0;
}

/**
 * Guess INT 13 drive geometry
 *
 * @v int13		Emulated drive
 * @ret rc		Return status code
 *
 * Guesses the drive geometry by inspecting the partition table.
 */
static int int13_guess_geometry ( struct int13_drive *int13 ) {
	struct master_boot_record mbr;
	struct partition_table_entry *partition;
	unsigned int guessed_heads = 255;
	unsigned int guessed_sectors_per_track = 63;
	unsigned long blocks;
	unsigned long blocks_per_cyl;
	unsigned int i;
	int rc;

	/* Don't even try when the blksize is invalid for C/H/S access */
	if ( int13->capacity.blksize != INT13_BLKSIZE )
		return 0;

	/* Read partition table */
	if ( ( rc = int13_rw ( int13, 0, 1, virt_to_user ( &mbr ),
			       block_read ) ) != 0 ) {
		DBGC ( int13, "INT13 drive %02x could not read partition "
		       "table to guess geometry: %s\n",
		       int13->drive, strerror ( rc ) );
		return rc;
	}

	/* Scan through partition table and modify guesses for heads
	 * and sectors_per_track if we find any used partitions.
	 */
	for ( i = 0 ; i < 4 ; i++ ) {
		partition = &mbr.partitions[i];
		if ( ! partition->type )
			continue;
		guessed_heads = ( PART_HEAD ( partition->chs_end ) + 1 );
		guessed_sectors_per_track = PART_SECTOR ( partition->chs_end );
		DBGC ( int13, "INT13 drive %02x guessing C/H/S xx/%d/%d based "
		       "on partition %d\n", int13->drive, guessed_heads,
		       guessed_sectors_per_track, ( i + 1 ) );
	}

	/* Apply guesses if no geometry already specified */
	if ( ! int13->heads )
		int13->heads = guessed_heads;
	if ( ! int13->sectors_per_track )
		int13->sectors_per_track = guessed_sectors_per_track;
	if ( ! int13->cylinders ) {
		/* Avoid attempting a 64-bit divide on a 32-bit system */
		blocks = ( ( int13->capacity.blocks <= ULONG_MAX ) ?
			   int13->capacity.blocks : ULONG_MAX );
		blocks_per_cyl = ( int13->heads * int13->sectors_per_track );
		assert ( blocks_per_cyl != 0 );
		int13->cylinders = ( blocks / blocks_per_cyl );
		if ( int13->cylinders > 1024 )
			int13->cylinders = 1024;
	}

	return 0;
}

/**
 * Open (or reopen) INT 13 emulated drive underlying block device
 *
 * @v int13		Emulated drive
 * @ret rc		Return status code
 */
static int int13_reopen_block ( struct int13_drive *int13 ) {
	int rc;

	/* Close any existing block device */
	intf_restart ( &int13->block, -ECONNRESET );

	/* Open block device */
	if ( ( rc = xfer_open_uri ( &int13->block, int13->uri ) ) != 0 ) {
		DBGC ( int13, "INT13 drive %02x could not reopen block "
		       "device: %s\n", int13->drive, strerror ( rc ) );
		int13->block_rc = rc;
		return rc;
	}

	/* Clear block device error status */
	int13->block_rc = 0;

	/* Read device capacity */
	if ( ( rc = int13_read_capacity ( int13 ) ) != 0 )
		return rc;

	return 0;
}

/**
 * Update BIOS drive count
 */
static void int13_set_num_drives ( void ) {
	struct int13_drive *int13;

	/* Get current drive count */
	get_real ( num_drives, BDA_SEG, BDA_NUM_DRIVES );

	/* Ensure count is large enough to cover all of our emulated drives */
	list_for_each_entry ( int13, &int13s, list ) {
		if ( num_drives <= ( int13->drive & 0x7f ) )
			num_drives = ( ( int13->drive & 0x7f ) + 1 );
	}

	/* Update current drive count */
	put_real ( num_drives, BDA_SEG, BDA_NUM_DRIVES );
}

/**
 * Check number of drives
 */
static void int13_check_num_drives ( void ) {
	uint8_t check_num_drives;

	get_real ( check_num_drives, BDA_SEG, BDA_NUM_DRIVES );
	if ( check_num_drives != num_drives ) {
		int13_set_num_drives();
		DBG ( "INT13 fixing up number of drives from %d to %d\n",
		      check_num_drives, num_drives );
	}
}

/**
 * INT 13, 00 - Reset disk system
 *
 * @v int13		Emulated drive
 * @ret status		Status code
 */
static int int13_reset ( struct int13_drive *int13,
			 struct i386_all_regs *ix86 __unused ) {
	int rc;

	DBGC2 ( int13, "Reset drive\n" );

	/* Reopen underlying block device */
	if ( ( rc = int13_reopen_block ( int13 ) ) != 0 )
		return -INT13_STATUS_RESET_FAILED;

	return 0;
}

/**
 * INT 13, 01 - Get status of last operation
 *
 * @v int13		Emulated drive
 * @ret status		Status code
 */
static int int13_get_last_status ( struct int13_drive *int13,
				   struct i386_all_regs *ix86 __unused ) {
	DBGC2 ( int13, "Get status of last operation\n" );
	return int13->last_status;
}

/**
 * Read / write sectors
 *
 * @v int13		Emulated drive
 * @v al		Number of sectors to read or write (must be nonzero)
 * @v ch		Low bits of cylinder number
 * @v cl (bits 7:6)	High bits of cylinder number
 * @v cl (bits 5:0)	Sector number
 * @v dh		Head number
 * @v es:bx		Data buffer
 * @v block_rw		Block read/write method
 * @ret status		Status code
 * @ret al		Number of sectors read or written
 */
static int int13_rw_sectors ( struct int13_drive *int13,
			      struct i386_all_regs *ix86,
			      int ( * block_rw ) ( struct interface *control,
						   struct interface *data,
						   uint64_t lba,
						   unsigned int count,
						   userptr_t buffer,
						   size_t len ) ) {
	unsigned int cylinder, head, sector;
	unsigned long lba;
	unsigned int count;
	userptr_t buffer;
	int rc;

	/* Validate blocksize */
	if ( int13->capacity.blksize != INT13_BLKSIZE ) {
		DBGC ( int13, "\nINT 13 drive %02x invalid blocksize (%zd) "
		       "for non-extended read/write\n",
		       int13->drive, int13->capacity.blksize );
		return -INT13_STATUS_INVALID;
	}
	
	/* Calculate parameters */
	cylinder = ( ( ( ix86->regs.cl & 0xc0 ) << 2 ) | ix86->regs.ch );
	assert ( cylinder < int13->cylinders );
	head = ix86->regs.dh;
	assert ( head < int13->heads );
	sector = ( ix86->regs.cl & 0x3f );
	assert ( ( sector >= 1 ) && ( sector <= int13->sectors_per_track ) );
	lba = ( ( ( ( cylinder * int13->heads ) + head )
		  * int13->sectors_per_track ) + sector - 1 );
	count = ix86->regs.al;
	buffer = real_to_user ( ix86->segs.es, ix86->regs.bx );

	DBGC2 ( int13, "C/H/S %d/%d/%d = LBA %08lx <-> %04x:%04x (count %d)\n",
		cylinder, head, sector, lba, ix86->segs.es, ix86->regs.bx,
		count );

	/* Read from / write to block device */
	if ( ( rc = int13_rw ( int13, lba, count, buffer, block_rw ) ) != 0 ) {
		DBGC ( int13, "INT13 drive %02x I/O failed: %s\n",
		       int13->drive, strerror ( rc ) );
		return -INT13_STATUS_READ_ERROR;
	}

	return 0;
}

/**
 * INT 13, 02 - Read sectors
 *
 * @v int13		Emulated drive
 * @v al		Number of sectors to read (must be nonzero)
 * @v ch		Low bits of cylinder number
 * @v cl (bits 7:6)	High bits of cylinder number
 * @v cl (bits 5:0)	Sector number
 * @v dh		Head number
 * @v es:bx		Data buffer
 * @ret status		Status code
 * @ret al		Number of sectors read
 */
static int int13_read_sectors ( struct int13_drive *int13,
				struct i386_all_regs *ix86 ) {
	DBGC2 ( int13, "Read: " );
	return int13_rw_sectors ( int13, ix86, block_read );
}

/**
 * INT 13, 03 - Write sectors
 *
 * @v int13		Emulated drive
 * @v al		Number of sectors to write (must be nonzero)
 * @v ch		Low bits of cylinder number
 * @v cl (bits 7:6)	High bits of cylinder number
 * @v cl (bits 5:0)	Sector number
 * @v dh		Head number
 * @v es:bx		Data buffer
 * @ret status		Status code
 * @ret al		Number of sectors written
 */
static int int13_write_sectors ( struct int13_drive *int13,
				 struct i386_all_regs *ix86 ) {
	DBGC2 ( int13, "Write: " );
	return int13_rw_sectors ( int13, ix86, block_write );
}

/**
 * INT 13, 08 - Get drive parameters
 *
 * @v int13		Emulated drive
 * @ret status		Status code
 * @ret ch		Low bits of maximum cylinder number
 * @ret cl (bits 7:6)	High bits of maximum cylinder number
 * @ret cl (bits 5:0)	Maximum sector number
 * @ret dh		Maximum head number
 * @ret dl		Number of drives
 */
static int int13_get_parameters ( struct int13_drive *int13,
				  struct i386_all_regs *ix86 ) {
	unsigned int max_cylinder = int13->cylinders - 1;
	unsigned int max_head = int13->heads - 1;
	unsigned int max_sector = int13->sectors_per_track; /* sic */

	DBGC2 ( int13, "Get drive parameters\n" );

	ix86->regs.ch = ( max_cylinder & 0xff );
	ix86->regs.cl = ( ( ( max_cylinder >> 8 ) << 6 ) | max_sector );
	ix86->regs.dh = max_head;
	get_real ( ix86->regs.dl, BDA_SEG, BDA_NUM_DRIVES );
	return 0;
}

/**
 * INT 13, 15 - Get disk type
 *
 * @v int13		Emulated drive
 * @ret ah		Type code
 * @ret cx:dx		Sector count
 * @ret status		Status code / disk type
 */
static int int13_get_disk_type ( struct int13_drive *int13,
				 struct i386_all_regs *ix86 ) {
	uint32_t blocks;

	DBGC2 ( int13, "Get disk type\n" );
	blocks = ( ( int13->capacity.blocks <= 0xffffffffUL ) ?
		   int13->capacity.blocks : 0xffffffffUL );
	ix86->regs.cx = ( blocks >> 16 );
	ix86->regs.dx = ( blocks & 0xffff );
	return INT13_DISK_TYPE_HDD;
}

/**
 * INT 13, 41 - Extensions installation check
 *
 * @v int13		Emulated drive
 * @v bx		0x55aa
 * @ret bx		0xaa55
 * @ret cx		Extensions API support bitmap
 * @ret status		Status code / API version
 */
static int int13_extension_check ( struct int13_drive *int13 __unused,
				   struct i386_all_regs *ix86 ) {
	if ( ix86->regs.bx == 0x55aa ) {
		DBGC2 ( int13, "INT13 extensions installation check\n" );
		ix86->regs.bx = 0xaa55;
		ix86->regs.cx = INT13_EXTENSION_LINEAR;
		return INT13_EXTENSION_VER_1_X;
	} else {
		return -INT13_STATUS_INVALID;
	}
}

/**
 * Extended read / write
 *
 * @v int13		Emulated drive
 * @v ds:si		Disk address packet
 * @v block_rw		Block read/write method
 * @ret status		Status code
 */
static int int13_extended_rw ( struct int13_drive *int13,
			       struct i386_all_regs *ix86,
			       int ( * block_rw ) ( struct interface *control,
						    struct interface *data,
						    uint64_t lba,
						    unsigned int count,
						    userptr_t buffer,
						    size_t len ) ) {
	struct int13_disk_address addr;
	uint64_t lba;
	unsigned long count;
	userptr_t buffer;
	int rc;

	/* Read parameters from disk address structure */
	copy_from_real ( &addr, ix86->segs.ds, ix86->regs.si, sizeof ( addr ));
	lba = addr.lba;
	count = addr.count;
	buffer = real_to_user ( addr.buffer.segment, addr.buffer.offset );

	DBGC2 ( int13, "LBA %08llx <-> %04x:%04x (count %ld)\n",
		( ( unsigned long long ) lba ), addr.buffer.segment,
		addr.buffer.offset, count );
	
	/* Read from / write to block device */
	if ( ( rc = int13_rw ( int13, lba, count, buffer, block_rw ) ) != 0 ) {
		DBGC ( int13, "INT13 drive %02x extended I/O failed: %s\n",
		       int13->drive, strerror ( rc ) );
		return -INT13_STATUS_READ_ERROR;
	}

	return 0;
}

/**
 * INT 13, 42 - Extended read
 *
 * @v int13		Emulated drive
 * @v ds:si		Disk address packet
 * @ret status		Status code
 */
static int int13_extended_read ( struct int13_drive *int13,
				 struct i386_all_regs *ix86 ) {
	DBGC2 ( int13, "Extended read: " );
	return int13_extended_rw ( int13, ix86, block_read );
}

/**
 * INT 13, 43 - Extended write
 *
 * @v int13		Emulated drive
 * @v ds:si		Disk address packet
 * @ret status		Status code
 */
static int int13_extended_write ( struct int13_drive *int13,
				  struct i386_all_regs *ix86 ) {
	DBGC2 ( int13, "Extended write: " );
	return int13_extended_rw ( int13, ix86, block_write );
}

/**
 * INT 13, 48 - Get extended parameters
 *
 * @v int13		Emulated drive
 * @v ds:si		Drive parameter table
 * @ret status		Status code
 */
static int int13_get_extended_parameters ( struct int13_drive *int13,
					   struct i386_all_regs *ix86 ) {
	struct int13_disk_parameters params = {
		.bufsize = sizeof ( params ),
		.flags = INT13_FL_DMA_TRANSPARENT,
		.cylinders = int13->cylinders,
		.heads = int13->heads,
		.sectors_per_track = int13->sectors_per_track,
		.sectors = int13->capacity.blocks,
		.sector_size = int13->capacity.blksize,
	};
	
	DBGC2 ( int13, "Get extended drive parameters to %04x:%04x\n",
		ix86->segs.ds, ix86->regs.si );

	copy_to_real ( ix86->segs.ds, ix86->regs.si, &params,
		       sizeof ( params ) );
	return 0;
}

/**
 * INT 13 handler
 *
 */
static __asmcall void int13 ( struct i386_all_regs *ix86 ) {
	int command = ix86->regs.ah;
	unsigned int bios_drive = ix86->regs.dl;
	struct int13_drive *int13;
	int status;

	/* Check BIOS hasn't killed off our drive */
	int13_check_num_drives();

	list_for_each_entry ( int13, &int13s, list ) {

		if ( bios_drive != int13->drive ) {
			/* Remap any accesses to this drive's natural number */
			if ( bios_drive == int13->natural_drive ) {
				DBGC2 ( int13, "INT13,%02x (%02x) remapped to "
					"(%02x)\n", ix86->regs.ah,
					bios_drive, int13->drive );
				ix86->regs.dl = int13->drive;
				return;
			}
			continue;
		}
		
		DBGC2 ( int13, "INT13,%02x (%02x): ",
			ix86->regs.ah, int13->drive );

		switch ( command ) {
		case INT13_RESET:
			status = int13_reset ( int13, ix86 );
			break;
		case INT13_GET_LAST_STATUS:
			status = int13_get_last_status ( int13, ix86 );
			break;
		case INT13_READ_SECTORS:
			status = int13_read_sectors ( int13, ix86 );
			break;
		case INT13_WRITE_SECTORS:
			status = int13_write_sectors ( int13, ix86 );
			break;
		case INT13_GET_PARAMETERS:
			status = int13_get_parameters ( int13, ix86 );
			break;
		case INT13_GET_DISK_TYPE:
			status = int13_get_disk_type ( int13, ix86 );
			break;
		case INT13_EXTENSION_CHECK:
			status = int13_extension_check ( int13, ix86 );
			break;
		case INT13_EXTENDED_READ:
			status = int13_extended_read ( int13, ix86 );
			break;
		case INT13_EXTENDED_WRITE:
			status = int13_extended_write ( int13, ix86 );
			break;
		case INT13_GET_EXTENDED_PARAMETERS:
			status = int13_get_extended_parameters ( int13, ix86 );
			break;
		default:
			DBGC2 ( int13, "*** Unrecognised INT13 ***\n" );
			status = -INT13_STATUS_INVALID;
			break;
		}

		/* Store status for INT 13,01 */
		int13->last_status = status;

		/* Negative status indicates an error */
		if ( status < 0 ) {
			status = -status;
			DBGC ( int13, "INT13,%02x (%02x) failed with status "
			       "%02x\n", ix86->regs.ah, int13->drive, status );
		} else {
			ix86->flags &= ~CF;
		}
		ix86->regs.ah = status;

		/* Set OF to indicate to wrapper not to chain this call */
		ix86->flags |= OF;

		return;
	}
}

/**
 * Hook INT 13 handler
 *
 */
static void int13_hook_vector ( void ) {
	/* Assembly wrapper to call int13().  int13() sets OF if we
	 * should not chain to the previous handler.  (The wrapper
	 * clears CF and OF before calling int13()).
	 */
	__asm__  __volatile__ (
	       TEXT16_CODE ( "\nint13_wrapper:\n\t"
			     /* Preserve %ax and %dx for future reference */
			     "pushw %%bp\n\t"
			     "movw %%sp, %%bp\n\t"			     
			     "pushw %%ax\n\t"
			     "pushw %%dx\n\t"
			     /* Clear OF, set CF, call int13() */
			     "orb $0, %%al\n\t" 
			     "stc\n\t"
			     "pushl %0\n\t"
			     "pushw %%cs\n\t"
			     "call prot_call\n\t"
			     /* Chain if OF not set */
			     "jo 1f\n\t"
			     "pushfw\n\t"
			     "lcall *%%cs:int13_vector\n\t"
			     "\n1:\n\t"
			     /* Overwrite flags for iret */
			     "pushfw\n\t"
			     "popw 6(%%bp)\n\t"
			     /* Fix up %dl:
			      *
			      * INT 13,15 : do nothing
			      * INT 13,08 : load with number of drives
			      * all others: restore original value
			      */
			     "cmpb $0x15, -1(%%bp)\n\t"
			     "je 2f\n\t"
			     "movb -4(%%bp), %%dl\n\t"
			     "cmpb $0x08, -1(%%bp)\n\t"
			     "jne 2f\n\t"
			     "pushw %%ds\n\t"
			     "pushw %1\n\t"
			     "popw %%ds\n\t"
			     "movb %c2, %%dl\n\t"
			     "popw %%ds\n\t"
			     /* Return */
			     "\n2:\n\t"
			     "movw %%bp, %%sp\n\t"
			     "popw %%bp\n\t"
			     "iret\n\t" )
	       : : "i" ( int13 ), "i" ( BDA_SEG ), "i" ( BDA_NUM_DRIVES ) );

	hook_bios_interrupt ( 0x13, ( unsigned int ) int13_wrapper,
			      &int13_vector );
}

/**
 * Unhook INT 13 handler
 */
static void int13_unhook_vector ( void ) {
	unhook_bios_interrupt ( 0x13, ( unsigned int ) int13_wrapper,
				&int13_vector );
}

/**
 * Handle INT 13 emulated drive underlying block device closing
 *
 * @v int13		Emulated drive
 * @v rc		Reason for close
 */
static void int13_block_close ( struct int13_drive *int13, int rc ) {

	/* Any closing is an error from our point of view */
	if ( rc == 0 )
		rc = -ENOTCONN;

	DBGC ( int13, "INT13 drive %02x went away: %s\n",
	       int13->drive, strerror ( rc ) );

	/* Record block device error code */
	int13->block_rc = rc;

	/* Shut down interfaces */
	intf_restart ( &int13->block, rc );

	/* Further INT 13 calls will fail immediately.  The caller may
	 * use INT 13,00 to reset the drive.
	 */
}

/** INT 13 drive interface operations */
static struct interface_operation int13_block_op[] = {
	INTF_OP ( intf_close, struct int13_drive *, int13_block_close ),
};

/** INT 13 drive interface descriptor */
static struct interface_descriptor int13_block_desc =
	INTF_DESC ( struct int13_drive, block, int13_block_op );

/**
 * Free INT 13 emulated drive
 *
 * @v refcnt		Reference count
 */
static void int13_free ( struct refcnt *refcnt ) {
	struct int13_drive *int13 =
		container_of ( refcnt, struct int13_drive, refcnt );

	uri_put ( int13->uri );
	free ( int13 );
}

/**
 * Hook INT 13 emulated drive
 *
 * @v uri		URI
 * @v drive		Requested drive number
 * @ret drive		Assigned drive number, or negative error
 *
 * Registers the drive with the INT 13 emulation subsystem, and hooks
 * the INT 13 interrupt vector (if not already hooked).
 */
static int int13_hook ( struct uri *uri, unsigned int drive ) {
	struct int13_drive *int13;
	uint8_t num_drives;
	unsigned int natural_drive;
	int rc;

	/* Calculate drive number */
	get_real ( num_drives, BDA_SEG, BDA_NUM_DRIVES );
	natural_drive = ( num_drives | 0x80 );
	if ( drive == INT13_USE_NATURAL_DRIVE )
		drive = natural_drive;
	drive |= 0x80;

	/* Check that drive number is not in use */
	list_for_each_entry ( int13, &int13s, list ) {
		if ( int13->drive == drive ) {
			rc = -EADDRINUSE;
			goto err_in_use;
		}
	}

	/* Allocate and initialise structure */
	int13 = zalloc ( sizeof ( *int13 ) );
	if ( ! int13 ) {
		rc = -ENOMEM;
		goto err_zalloc;
	}
	ref_init ( &int13->refcnt, int13_free );
	intf_init ( &int13->block, &int13_block_desc, &int13->refcnt );
	int13->uri = uri_get ( uri );
	int13->drive = drive;
	int13->natural_drive = natural_drive;

	/* Open block device interface */
	if ( ( rc = int13_reopen_block ( int13 ) ) != 0 )
		goto err_reopen_block;

	/* Give drive a default geometry */
	if ( ( rc = int13_guess_geometry ( int13 ) ) != 0 )
		goto err_guess_geometry;

	DBGC ( int13, "INT13 drive %02x (naturally %02x) registered with C/H/S "
	       "geometry %d/%d/%d\n", int13->drive, int13->natural_drive,
	       int13->cylinders, int13->heads, int13->sectors_per_track );

	/* Hook INT 13 vector if not already hooked */
	if ( list_empty ( &int13s ) )
		int13_hook_vector();

	/* Add to list of emulated drives */
	list_add ( &int13->list, &int13s );

	/* Update BIOS drive count */
	int13_set_num_drives();

	return int13->drive;

 err_guess_geometry:
 err_reopen_block:
	intf_shutdown ( &int13->block, rc );
	ref_put ( &int13->refcnt );
 err_zalloc:
 err_in_use:
	return rc;
}

/**
 * Find INT 13 emulated drive by drive number
 *
 * @v drive		Drive number
 * @ret int13		Emulated drive, or NULL
 */
static struct int13_drive * int13_find ( unsigned int drive ) {
	struct int13_drive *int13;

	list_for_each_entry ( int13, &int13s, list ) {
		if ( int13->drive == drive )
			return int13;
	}
	return NULL;
}

/**
 * Unhook INT 13 emulated drive
 *
 * @v drive		Drive number
 *
 * Unregisters the drive from the INT 13 emulation subsystem.  If this
 * is the last emulated drive, the INT 13 vector is unhooked (if
 * possible).
 */
static void int13_unhook ( unsigned int drive ) {
	struct int13_drive *int13;

	/* Find drive */
	int13 = int13_find ( drive );
	if ( ! int13 ) {
		DBG ( "INT13 cannot find emulated drive %02x\n", drive );
		return;
	}

	/* Shut down interfaces */
	intf_shutdown ( &int13->block, 0 );

	/* Remove from list of emulated drives */
	list_del ( &int13->list );

	/* Should adjust BIOS drive count, but it's difficult
	 * to do so reliably.
	 */

	DBGC ( int13, "INT13 drive %02x unregsitered\n", int13->drive );

	/* Unhook INT 13 vector if no more drives */
	if ( list_empty ( &int13s ) )
		int13_unhook_vector();

	/* Drop list's reference to drive */
	ref_put ( &int13->refcnt );
}

/**
 * Attempt to boot from an INT 13 drive
 *
 * @v drive		Drive number
 * @ret rc		Return status code
 *
 * This boots from the specified INT 13 drive by loading the Master
 * Boot Record to 0000:7c00 and jumping to it.  INT 18 is hooked to
 * capture an attempt by the MBR to boot the next device.  (This is
 * the closest thing to a return path from an MBR).
 *
 * Note that this function can never return success, by definition.
 */
static int int13_boot ( unsigned int drive ) {
	struct memory_map memmap;
	int status, signature;
	int discard_c, discard_d;
	int rc;

	DBG ( "INT13 drive %02x booting\n", drive );

	/* Use INT 13 to read the boot sector */
	__asm__ __volatile__ ( REAL_CODE ( "pushw %%es\n\t"
					   "pushw $0\n\t"
					   "popw %%es\n\t"
					   "stc\n\t"
					   "sti\n\t"
					   "int $0x13\n\t"
					   "sti\n\t" /* BIOS bugs */
					   "jc 1f\n\t"
					   "xorl %%eax, %%eax\n\t"
					   "\n1:\n\t"
					   "movzwl %%es:0x7dfe, %%ebx\n\t"
					   "popw %%es\n\t" )
			       : "=a" ( status ), "=b" ( signature ),
				 "=c" ( discard_c ), "=d" ( discard_d )
			       : "a" ( 0x0201 ), "b" ( 0x7c00 ),
				 "c" ( 1 ), "d" ( drive ) );
	if ( status )
		return -EIO;

	/* Check signature is correct */
	if ( signature != be16_to_cpu ( 0x55aa ) ) {
		DBG ( "INT13 drive %02x invalid disk signature %#04x (should "
		      "be 0x55aa)\n", drive, cpu_to_be16 ( signature ) );
		return -ENOEXEC;
	}

	/* Dump out memory map prior to boot, if memmap debugging is
	 * enabled.  Not required for program flow, but we have so
	 * many problems that turn out to be memory-map related that
	 * it's worth doing.
	 */
	get_memmap ( &memmap );

	/* Jump to boot sector */
	if ( ( rc = call_bootsector ( 0x0, 0x7c00, drive ) ) != 0 ) {
		DBG ( "INT13 drive %02x boot returned: %s\n",
		      drive, strerror ( rc ) );
		return rc;
	}

	return -ECANCELED; /* -EIMPOSSIBLE */
}

/** A boot firmware table generated by iPXE */
union xbft_table {
	/** ACPI header */
	struct acpi_description_header acpi;
	/** Padding */
	char pad[768];
};

/** The boot firmware table generated by iPXE */
static union xbft_table __bss16 ( xbftab ) __attribute__ (( aligned ( 16 ) ));
#define xbftab __use_data16 ( xbftab )

/**
 * Describe INT 13 emulated drive for SAN-booted operating system
 *
 * @v drive		Drive number
 * @ret rc		Return status code
 */
static int int13_describe ( unsigned int drive ) {
	struct int13_drive *int13;
	struct segoff xbft_address;
	int rc;

	/* Find drive */
	int13 = int13_find ( drive );
	if ( ! int13 ) {
		DBG ( "INT13 cannot find emulated drive %02x\n", drive );
		return -ENODEV;
	}

	/* Clear table */
	memset ( &xbftab, 0, sizeof ( xbftab ) );

	/* Fill in common parameters */
	strncpy ( xbftab.acpi.oem_id, "FENSYS",
		  sizeof ( xbftab.acpi.oem_id ) );
	strncpy ( xbftab.acpi.oem_table_id, "iPXE",
		  sizeof ( xbftab.acpi.oem_table_id ) );

	/* Fill in remaining parameters */
	if ( ( rc = acpi_describe ( &int13->block, &xbftab.acpi,
				    sizeof ( xbftab ) ) ) != 0 ) {
		DBGC ( int13, "INT13 drive %02x could not create ACPI "
		       "description: %s\n", int13->drive, strerror ( rc ) );
		return rc;
	}

	/* Fix up ACPI checksum */
	acpi_fix_checksum ( &xbftab.acpi );
	xbft_address.segment = rm_ds;
	xbft_address.offset = __from_data16 ( &xbftab );
	DBGC ( int13, "INT13 drive %02x described using boot firmware "
	       "table:\n", int13->drive );
	DBGC_HDA ( int13, xbft_address, &xbftab,
		   le32_to_cpu ( xbftab.acpi.length ) );

	return 0;
}

PROVIDE_SANBOOT ( pcbios, san_hook, int13_hook );
PROVIDE_SANBOOT ( pcbios, san_unhook, int13_unhook );
PROVIDE_SANBOOT ( pcbios, san_boot, int13_boot );
PROVIDE_SANBOOT ( pcbios, san_describe, int13_describe );
