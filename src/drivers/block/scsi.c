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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <byteswap.h>
#include <errno.h>
#include <ipxe/list.h>
#include <ipxe/blockdev.h>
#include <ipxe/scsi.h>

/** @file
 *
 * SCSI block device
 *
 */

/** Maximum number of command retries */
#define SCSICMD_MAX_RETRIES 10

/******************************************************************************
 *
 * Utility functions
 *
 ******************************************************************************
 */

/**
 * Parse SCSI LUN
 *
 * @v lun_string	LUN string representation
 * @v lun		LUN to fill in
 * @ret rc		Return status code
 */
int scsi_parse_lun ( const char *lun_string, struct scsi_lun *lun ) {
	char *p;
	int i;

	memset ( lun, 0, sizeof ( *lun ) );
	if ( lun_string ) {
		p = ( char * ) lun_string;
		for ( i = 0 ; i < 4 ; i++ ) {
			lun->u16[i] = htons ( strtoul ( p, &p, 16 ) );
			if ( *p == '\0' )
				break;
			if ( *p != '-' )
				return -EINVAL;
			p++;
		}
		if ( *p )
			return -EINVAL;
	}

	return 0;
}

/******************************************************************************
 *
 * Interface methods
 *
 ******************************************************************************
 */

/**
 * Issue SCSI command
 *
 * @v control		SCSI control interface
 * @v data		SCSI data interface
 * @v command		SCSI command
 * @ret tag		Command tag, or negative error
 */
int scsi_command ( struct interface *control, struct interface *data,
		   struct scsi_cmd *command ) {
	struct interface *dest;
	scsi_command_TYPE ( void * ) *op =
		intf_get_dest_op ( control, scsi_command, &dest );
	void *object = intf_object ( dest );
	int tap;

	if ( op ) {
		tap = op ( object, data, command );
	} else {
		/* Default is to fail to issue the command */
		tap = -EOPNOTSUPP;
	}

	intf_put ( dest );
	return tap;
}

/**
 * Report SCSI response
 *
 * @v interface		SCSI command interface
 * @v response		SCSI response
 */
void scsi_response ( struct interface *intf, struct scsi_rsp *response ) {
	struct interface *dest;
	scsi_response_TYPE ( void * ) *op =
		intf_get_dest_op ( intf, scsi_response, &dest );
	void *object = intf_object ( dest );

	if ( op ) {
		op ( object, response );
	} else {
		/* Default is to ignore the response */
	}

	intf_put ( dest );
}

/******************************************************************************
 *
 * SCSI devices and commands
 *
 ******************************************************************************
 */

/** A SCSI device */
struct scsi_device {
	/** Reference count */
	struct refcnt refcnt;
	/** Block control interface */
	struct interface block;
	/** SCSI control interface */
	struct interface scsi;

	/** SCSI LUN */
	struct scsi_lun lun;

	/** List of commands */
	struct list_head cmds;
};

/** A SCSI command */
struct scsi_command {
	/** Reference count */
	struct refcnt refcnt;
	/** SCSI device */
	struct scsi_device *scsidev;
	/** List of SCSI commands */
	struct list_head list;

	/** Block data interface */
	struct interface block;
	/** SCSI data interface */
	struct interface scsi;

	/** Command type */
	struct scsi_command_type *type;
	/** Starting logical block address */
	uint64_t lba;
	/** Number of blocks */
	unsigned int count;
	/** Data buffer */
	userptr_t buffer;
	/** Length of data buffer */
	size_t len;
	/** Command tag */
	uint32_t tag;

	/** Retry count */
	unsigned int retries;

	/** Private data */
	uint8_t priv[0];
};

/** A SCSI command type */
struct scsi_command_type {
	/** Name */
	const char *name;
	/** Additional working space */
	size_t priv_len;
	/**
	 * Construct SCSI command IU
	 *
	 * @v scsicmd		SCSI command
	 * @v command		SCSI command IU
	 */
	void ( * cmd ) ( struct scsi_command *scsicmd,
			 struct scsi_cmd *command );
	/**
	 * Handle SCSI command completion
	 *
	 * @v scsicmd		SCSI command
	 * @v rc		Reason for completion
	 */
	void ( * done ) ( struct scsi_command *scsicmd, int rc );
};

/**
 * Get reference to SCSI device
 *
 * @v scsidev		SCSI device
 * @ret scsidev		SCSI device
 */
static inline __attribute__ (( always_inline )) struct scsi_device *
scsidev_get ( struct scsi_device *scsidev ) {
	ref_get ( &scsidev->refcnt );
	return scsidev;
}

/**
 * Drop reference to SCSI device
 *
 * @v scsidev		SCSI device
 */
static inline __attribute__ (( always_inline )) void
scsidev_put ( struct scsi_device *scsidev ) {
	ref_put ( &scsidev->refcnt );
}

/**
 * Get reference to SCSI command
 *
 * @v scsicmd		SCSI command
 * @ret scsicmd		SCSI command
 */
static inline __attribute__ (( always_inline )) struct scsi_command *
scsicmd_get ( struct scsi_command *scsicmd ) {
	ref_get ( &scsicmd->refcnt );
	return scsicmd;
}

/**
 * Drop reference to SCSI command
 *
 * @v scsicmd		SCSI command
 */
static inline __attribute__ (( always_inline )) void
scsicmd_put ( struct scsi_command *scsicmd ) {
	ref_put ( &scsicmd->refcnt );
}

/**
 * Get SCSI command private data
 *
 * @v scsicmd		SCSI command
 * @ret priv		Private data
 */
static inline __attribute__ (( always_inline )) void *
scsicmd_priv ( struct scsi_command *scsicmd ) {
	return scsicmd->priv;
}

/**
 * Free SCSI command
 *
 * @v refcnt		Reference count
 */
static void scsicmd_free ( struct refcnt *refcnt ) {
	struct scsi_command *scsicmd =
		container_of ( refcnt, struct scsi_command, refcnt );

	/* Remove from list of commands */
	list_del ( &scsicmd->list );
	scsidev_put ( scsicmd->scsidev );

	/* Free command */
	free ( scsicmd );
}

/**
 * Close SCSI command
 *
 * @v scsicmd		SCSI command
 * @v rc		Reason for close
 */
static void scsicmd_close ( struct scsi_command *scsicmd, int rc ) {
	struct scsi_device *scsidev = scsicmd->scsidev;

	if ( rc != 0 ) {
		DBGC ( scsidev, "SCSI %p tag %08x closed: %s\n",
		       scsidev, scsicmd->tag, strerror ( rc ) );
	}

	/* Shut down interfaces */
	intf_shutdown ( &scsicmd->scsi, rc );
	intf_shutdown ( &scsicmd->block, rc );
}

/**
 * Construct and issue SCSI command
 *
 * @ret rc		Return status code
 */
static int scsicmd_command ( struct scsi_command *scsicmd ) {
	struct scsi_device *scsidev = scsicmd->scsidev;
	struct scsi_cmd command;
	int tag;
	int rc;

	/* Construct command */
	memset ( &command, 0, sizeof ( command ) );
	memcpy ( &command.lun, &scsidev->lun, sizeof ( command.lun ) );
	scsicmd->type->cmd ( scsicmd, &command );

	/* Issue command */
	if ( ( tag = scsi_command ( &scsidev->scsi, &scsicmd->scsi,
				    &command ) ) < 0 ) {
		rc = tag;
		DBGC ( scsidev, "SCSI %p could not issue command: %s\n",
		       scsidev, strerror ( rc ) );
		return rc;
	}

	/* Record tag */
	if ( scsicmd->tag ) {
		DBGC ( scsidev, "SCSI %p tag %08x is now tag %08x\n",
		       scsidev, scsicmd->tag, tag );
	}
	scsicmd->tag = tag;
	DBGC2 ( scsidev, "SCSI %p tag %08x %s " SCSI_CDB_FORMAT "\n",
		scsidev, scsicmd->tag, scsicmd->type->name,
		SCSI_CDB_DATA ( command.cdb ) );

	return 0;
}

/**
 * Handle SCSI command completion
 *
 * @v scsicmd		SCSI command
 * @v rc		Reason for close
 */
static void scsicmd_done ( struct scsi_command *scsicmd, int rc ) {
	struct scsi_device *scsidev = scsicmd->scsidev;

	/* Restart SCSI interface */
	intf_restart ( &scsicmd->scsi, rc );

	/* SCSI targets have an annoying habit of returning occasional
	 * pointless "error" messages such as "power-on occurred", so
	 * we have to be prepared to retry commands.
	 */
	if ( ( rc != 0 ) && ( scsicmd->retries++ < SCSICMD_MAX_RETRIES ) ) {
		/* Retry command */
		DBGC ( scsidev, "SCSI %p tag %08x failed: %s\n",
		       scsidev, scsicmd->tag, strerror ( rc ) );
		DBGC ( scsidev, "SCSI %p tag %08x retrying (retry %d)\n",
		       scsidev, scsicmd->tag, scsicmd->retries );
		if ( ( rc = scsicmd_command ( scsicmd ) ) == 0 )
			return;
	}

	/* If we didn't (successfully) reissue the command, hand over
	 * to the command completion handler.
	 */
	scsicmd->type->done ( scsicmd, rc );
}

/**
 * Handle SCSI response
 *
 * @v scsicmd		SCSI command
 * @v response		SCSI response
 */
static void scsicmd_response ( struct scsi_command *scsicmd,
			       struct scsi_rsp *response ) {
	struct scsi_device *scsidev = scsicmd->scsidev;
	size_t overrun;
	size_t underrun;

	if ( response->status == 0 ) {
		scsicmd_done ( scsicmd, 0 );
	} else {
		DBGC ( scsidev, "SCSI %p tag %08x status %02x",
		       scsidev, scsicmd->tag, response->status );
		if ( response->overrun > 0 ) {
			overrun = response->overrun;
			DBGC ( scsidev, " overrun +%zd", overrun );
		} else if ( response->overrun < 0 ) {
			underrun = -(response->overrun);
			DBGC ( scsidev, " underrun -%zd", underrun );
		}
		DBGC ( scsidev, " sense %02x:%02x:%08x\n",
		       response->sense.code, response->sense.key,
		       ntohl ( response->sense.info ) );
		scsicmd_done ( scsicmd, -EIO );
	}
}

/**
 * Construct SCSI READ command
 *
 * @v scsicmd		SCSI command
 * @v command		SCSI command IU
 */
static void scsicmd_read_cmd ( struct scsi_command *scsicmd,
			       struct scsi_cmd *command ) {

	if ( ( scsicmd->lba + scsicmd->count ) > SCSI_MAX_BLOCK_10 ) {
		/* Use READ (16) */
		command->cdb.read16.opcode = SCSI_OPCODE_READ_16;
		command->cdb.read16.lba = cpu_to_be64 ( scsicmd->lba );
		command->cdb.read16.len = cpu_to_be32 ( scsicmd->count );
	} else {
		/* Use READ (10) */
		command->cdb.read10.opcode = SCSI_OPCODE_READ_10;
		command->cdb.read10.lba = cpu_to_be32 ( scsicmd->lba );
		command->cdb.read10.len = cpu_to_be16 ( scsicmd->count );
	}
	command->data_in = scsicmd->buffer;
	command->data_in_len = scsicmd->len;
}

/** SCSI READ command type */
static struct scsi_command_type scsicmd_read = {
	.name = "READ",
	.cmd = scsicmd_read_cmd,
	.done = scsicmd_close,
};

/**
 * Construct SCSI WRITE command
 *
 * @v scsicmd		SCSI command
 * @v command		SCSI command IU
 */
static void scsicmd_write_cmd ( struct scsi_command *scsicmd,
				struct scsi_cmd *command ) {

	if ( ( scsicmd->lba + scsicmd->count ) > SCSI_MAX_BLOCK_10 ) {
		/* Use WRITE (16) */
		command->cdb.write16.opcode = SCSI_OPCODE_WRITE_16;
		command->cdb.write16.lba = cpu_to_be64 ( scsicmd->lba );
		command->cdb.write16.len = cpu_to_be32 ( scsicmd->count );
	} else {
		/* Use WRITE (10) */
		command->cdb.write10.opcode = SCSI_OPCODE_WRITE_10;
		command->cdb.write10.lba = cpu_to_be32 ( scsicmd->lba );
		command->cdb.write10.len = cpu_to_be16 ( scsicmd->count );
	}
	command->data_out = scsicmd->buffer;
	command->data_out_len = scsicmd->len;
}

/** SCSI WRITE command type */
static struct scsi_command_type scsicmd_write = {
	.name = "WRITE",
	.cmd = scsicmd_write_cmd,
	.done = scsicmd_close,
};

/** SCSI READ CAPACITY private data */
struct scsi_read_capacity_private {
	/** Use READ CAPACITY (16) */
	int use16;
	/** Data buffer for READ CAPACITY commands */
	union {
		/** Data buffer for READ CAPACITY (10) */
		struct scsi_capacity_10 capacity10;
		/** Data buffer for READ CAPACITY (16) */
		struct scsi_capacity_16 capacity16;
	} capacity;
};

/**
 * Construct SCSI READ CAPACITY command
 *
 * @v scsicmd		SCSI command
 * @v command		SCSI command IU
 */
static void scsicmd_read_capacity_cmd ( struct scsi_command *scsicmd,
					struct scsi_cmd *command ) {
	struct scsi_read_capacity_private *priv = scsicmd_priv ( scsicmd );
	struct scsi_cdb_read_capacity_16 *readcap16 = &command->cdb.readcap16;
	struct scsi_cdb_read_capacity_10 *readcap10 = &command->cdb.readcap10;
	struct scsi_capacity_16 *capacity16 = &priv->capacity.capacity16;
	struct scsi_capacity_10 *capacity10 = &priv->capacity.capacity10;

	if ( priv->use16 ) {
		/* Use READ CAPACITY (16) */
		readcap16->opcode = SCSI_OPCODE_SERVICE_ACTION_IN;
		readcap16->service_action =
			SCSI_SERVICE_ACTION_READ_CAPACITY_16;
		readcap16->len = cpu_to_be32 ( sizeof ( *capacity16 ) );
		command->data_in = virt_to_user ( capacity16 );
		command->data_in_len = sizeof ( *capacity16 );
	} else {
		/* Use READ CAPACITY (10) */
		readcap10->opcode = SCSI_OPCODE_READ_CAPACITY_10;
		command->data_in = virt_to_user ( capacity10 );
		command->data_in_len = sizeof ( *capacity10 );
	}
}

/**
 * Handle SCSI READ CAPACITY command completion
 *
 * @v scsicmd		SCSI command
 * @v rc		Reason for completion
 */
static void scsicmd_read_capacity_done ( struct scsi_command *scsicmd,
					 int rc ) {
	struct scsi_read_capacity_private *priv = scsicmd_priv ( scsicmd );
	struct scsi_capacity_16 *capacity16 = &priv->capacity.capacity16;
	struct scsi_capacity_10 *capacity10 = &priv->capacity.capacity10;
	struct block_device_capacity capacity;

	/* Close if command failed */
	if ( rc != 0 ) {
		scsicmd_close ( scsicmd, rc );
		return;
	}

	/* Extract capacity */
	if ( priv->use16 ) {
		capacity.blocks = ( be64_to_cpu ( capacity16->lba ) + 1 );
		capacity.blksize = be32_to_cpu ( capacity16->blksize );
	} else {
		capacity.blocks = ( be32_to_cpu ( capacity10->lba ) + 1 );
		capacity.blksize = be32_to_cpu ( capacity10->blksize );

		/* If capacity range was exceeded (i.e. capacity.lba
		 * was 0xffffffff, meaning that blockdev->blocks is
		 * now zero), use READ CAPACITY (16) instead.  READ
		 * CAPACITY (16) is not mandatory, so we can't just
		 * use it straight off.
		 */
		if ( capacity.blocks == 0 ) {
			priv->use16 = 1;
			if ( ( rc = scsicmd_command ( scsicmd ) ) != 0 ) {
				scsicmd_close ( scsicmd, rc );
				return;
			}
			return;
		}
	}
	capacity.max_count = -1U;

	/* Return capacity to caller */
	block_capacity ( &scsicmd->block, &capacity );

	/* Close command */
	scsicmd_close ( scsicmd, 0 );
}

/** SCSI READ CAPACITY command type */
static struct scsi_command_type scsicmd_read_capacity = {
	.name = "READ CAPACITY",
	.priv_len = sizeof ( struct scsi_read_capacity_private ),
	.cmd = scsicmd_read_capacity_cmd,
	.done = scsicmd_read_capacity_done,
};

/** SCSI command block interface operations */
static struct interface_operation scsicmd_block_op[] = {
	INTF_OP ( intf_close, struct scsi_command *, scsicmd_close ),
};

/** SCSI command block interface descriptor */
static struct interface_descriptor scsicmd_block_desc =
	INTF_DESC_PASSTHRU ( struct scsi_command, block,
			     scsicmd_block_op, scsi );

/** SCSI command SCSI interface operations */
static struct interface_operation scsicmd_scsi_op[] = {
	INTF_OP ( intf_close, struct scsi_command *, scsicmd_done ),
	INTF_OP ( scsi_response, struct scsi_command *, scsicmd_response ),
};

/** SCSI command SCSI interface descriptor */
static struct interface_descriptor scsicmd_scsi_desc =
	INTF_DESC_PASSTHRU ( struct scsi_command, scsi,
			     scsicmd_scsi_op, block );

/**
 * Create SCSI command
 *
 * @v scsidev		SCSI device
 * @v block		Block data interface
 * @v type		SCSI command type
 * @v lba		Starting logical block address
 * @v count		Number of blocks to transfer
 * @v buffer		Data buffer
 * @v len		Length of data buffer
 * @ret rc		Return status code
 */
static int scsidev_command ( struct scsi_device *scsidev,
			     struct interface *block,
			     struct scsi_command_type *type,
			     uint64_t lba, unsigned int count,
			     userptr_t buffer, size_t len ) {
	struct scsi_command *scsicmd;
	int rc;

	/* Allocate and initialise structure */
	scsicmd = zalloc ( sizeof ( *scsicmd ) + type->priv_len );
	if ( ! scsicmd ) {
		rc = -ENOMEM;
		goto err_zalloc;
	}
	ref_init ( &scsicmd->refcnt, scsicmd_free );
	intf_init ( &scsicmd->block, &scsicmd_block_desc, &scsicmd->refcnt );
	intf_init ( &scsicmd->scsi, &scsicmd_scsi_desc,
		    &scsicmd->refcnt );
	scsicmd->scsidev = scsidev_get ( scsidev );
	list_add ( &scsicmd->list, &scsidev->cmds );
	scsicmd->type = type;
	scsicmd->lba = lba;
	scsicmd->count = count;
	scsicmd->buffer = buffer;
	scsicmd->len = len;

	/* Issue SCSI command */
	if ( ( rc = scsicmd_command ( scsicmd ) ) != 0 )
		goto err_command;

	/* Attach to parent interface, mortalise self, and return */
	intf_plug_plug ( &scsicmd->block, block );
	ref_put ( &scsicmd->refcnt );
	return 0;

 err_command:
	scsicmd_close ( scsicmd, rc );
	ref_put ( &scsicmd->refcnt );
 err_zalloc:
	return rc;
}

/**
 * Issue SCSI block read
 *
 * @v scsidev		SCSI device
 * @v block		Block data interface
 * @v lba		Starting logical block address
 * @v count		Number of blocks to transfer
 * @v buffer		Data buffer
 * @v len		Length of data buffer
 * @ret rc		Return status code

 */
static int scsidev_read ( struct scsi_device *scsidev,
			  struct interface *block,
			  uint64_t lba, unsigned int count,
			  userptr_t buffer, size_t len ) {
	return scsidev_command ( scsidev, block, &scsicmd_read,
				 lba, count, buffer, len );
}

/**
 * Issue SCSI block write
 *
 * @v scsidev		SCSI device
 * @v block		Block data interface
 * @v lba		Starting logical block address
 * @v count		Number of blocks to transfer
 * @v buffer		Data buffer
 * @v len		Length of data buffer
 * @ret rc		Return status code
 */
static int scsidev_write ( struct scsi_device *scsidev,
			   struct interface *block,
			   uint64_t lba, unsigned int count,
			   userptr_t buffer, size_t len ) {
	return scsidev_command ( scsidev, block, &scsicmd_write,
				 lba, count, buffer, len );
}

/**
 * Read SCSI device capacity
 *
 * @v scsidev		SCSI device
 * @v block		Block data interface
 * @ret rc		Return status code
 */
static int scsidev_read_capacity ( struct scsi_device *scsidev,
				   struct interface *block ) {
	return scsidev_command ( scsidev, block, &scsicmd_read_capacity,
				 0, 0, UNULL, 0 );
}

/**
 * Close SCSI device
 *
 * @v scsidev		SCSI device
 * @v rc		Reason for close
 */
static void scsidev_close ( struct scsi_device *scsidev, int rc ) {
	struct scsi_command *scsicmd;
	struct scsi_command *tmp;

	/* Shut down interfaces */
	intf_shutdown ( &scsidev->block, rc );
	intf_shutdown ( &scsidev->scsi, rc );

	/* Shut down any remaining commands */
	list_for_each_entry_safe ( scsicmd, tmp, &scsidev->cmds, list ) {
		scsicmd_get ( scsicmd );
		scsicmd_close ( scsicmd, rc );
		scsicmd_put ( scsicmd );
	}
}

/** SCSI device block interface operations */
static struct interface_operation scsidev_block_op[] = {
	INTF_OP ( block_read, struct scsi_device *, scsidev_read ),
	INTF_OP ( block_write, struct scsi_device *, scsidev_write ),
	INTF_OP ( block_read_capacity, struct scsi_device *,
		  scsidev_read_capacity ),
	INTF_OP ( intf_close, struct scsi_device *, scsidev_close ),
};

/** SCSI device block interface descriptor */
static struct interface_descriptor scsidev_block_desc =
	INTF_DESC_PASSTHRU ( struct scsi_device, block,
			     scsidev_block_op, scsi );

/** SCSI device SCSI interface operations */
static struct interface_operation scsidev_scsi_op[] = {
	INTF_OP ( intf_close, struct scsi_device *, scsidev_close ),
};

/** SCSI device SCSI interface descriptor */
static struct interface_descriptor scsidev_scsi_desc =
	INTF_DESC_PASSTHRU ( struct scsi_device, scsi,
			     scsidev_scsi_op, block );

/**
 * Open SCSI device
 *
 * @v block		Block control interface
 * @v scsi		SCSI control interface
 * @v lun		SCSI LUN
 * @ret rc		Return status code
 */
int scsi_open ( struct interface *block, struct interface *scsi,
		struct scsi_lun *lun ) {
	struct scsi_device *scsidev;

	/* Allocate and initialise structure */
	scsidev = zalloc ( sizeof ( *scsidev ) );
	if ( ! scsidev )
		return -ENOMEM;
	ref_init ( &scsidev->refcnt, NULL );
	intf_init ( &scsidev->block, &scsidev_block_desc, &scsidev->refcnt );
	intf_init ( &scsidev->scsi, &scsidev_scsi_desc, &scsidev->refcnt );
	INIT_LIST_HEAD ( &scsidev->cmds );
	memcpy ( &scsidev->lun, lun, sizeof ( scsidev->lun ) );
	DBGC ( scsidev, "SCSI %p created for LUN " SCSI_LUN_FORMAT "\n",
	       scsidev, SCSI_LUN_DATA ( scsidev->lun ) );

	/* Attach to SCSI and parent and interfaces, mortalise self,
	 * and return
	 */
	intf_plug_plug ( &scsidev->scsi, scsi );
	intf_plug_plug ( &scsidev->block, block );
	ref_put ( &scsidev->refcnt );
	return 0;
}
