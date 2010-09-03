/*
 * Copyright (C) 2009 Fen Systems Ltd <mbrown@fensystems.co.uk>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

FILE_LICENCE ( BSD2 );

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ipxe/scsi.h>
#include <ipxe/xfer.h>
#include <ipxe/features.h>
#include <ipxe/srp.h>

/**
 * @file
 *
 * SCSI RDMA Protocol
 *
 */

FEATURE ( FEATURE_PROTOCOL, "SRP", DHCP_EB_FEATURE_SRP, 1 );

/** Maximum length of any initiator-to-target IU that we will send
 *
 * The longest IU is a SRP_CMD with no additional CDB and two direct
 * data buffer descriptors, which comes to 80 bytes.
 */
#define SRP_MAX_I_T_IU_LEN 80

/** An SRP device */
struct srp_device {
	/** Reference count */
	struct refcnt refcnt;

	/** SCSI command issuing interface */
	struct interface scsi;
	/** Underlying data transfer interface */
	struct interface socket;

	/** RDMA memory handle */
	uint32_t memory_handle;
	/** Login completed successfully */
	int logged_in;

	/** Initiator port ID (for boot firmware table) */
	union srp_port_id initiator;
	/** Target port ID (for boot firmware table) */
	union srp_port_id target;
	/** SCSI LUN (for boot firmware table) */
	struct scsi_lun lun;

	/** List of active commands */
	struct list_head commands;
};

/** An SRP command */
struct srp_command {
	/** Reference count */
	struct refcnt refcnt;
	/** SRP device */
	struct srp_device *srpdev;
	/** List of active commands */
	struct list_head list;

	/** SCSI command interface */
	struct interface scsi;
	/** Command tag */
	uint32_t tag;
};

/**
 * Get reference to SRP device
 *
 * @v srpdev		SRP device
 * @ret srpdev		SRP device
 */
static inline __attribute__ (( always_inline )) struct srp_device *
srpdev_get ( struct srp_device *srpdev ) {
	ref_get ( &srpdev->refcnt );
	return srpdev;
}

/**
 * Drop reference to SRP device
 *
 * @v srpdev		SRP device
 */
static inline __attribute__ (( always_inline )) void
srpdev_put ( struct srp_device *srpdev ) {
	ref_put ( &srpdev->refcnt );
}

/**
 * Get reference to SRP command
 *
 * @v srpcmd		SRP command
 * @ret srpcmd		SRP command
 */
static inline __attribute__ (( always_inline )) struct srp_command *
srpcmd_get ( struct srp_command *srpcmd ) {
	ref_get ( &srpcmd->refcnt );
	return srpcmd;
}

/**
 * Drop reference to SRP command
 *
 * @v srpcmd		SRP command
 */
static inline __attribute__ (( always_inline )) void
srpcmd_put ( struct srp_command *srpcmd ) {
	ref_put ( &srpcmd->refcnt );
}

/**
 * Free SRP command
 *
 * @v refcnt		Reference count
 */
static void srpcmd_free ( struct refcnt *refcnt ) {
	struct srp_command *srpcmd =
		container_of ( refcnt, struct srp_command, refcnt );

	assert ( list_empty ( &srpcmd->list ) );

	srpdev_put ( srpcmd->srpdev );
	free ( srpcmd );
}

/**
 * Close SRP command
 *
 * @v srpcmd		SRP command
 * @v rc		Reason for close
 */
static void srpcmd_close ( struct srp_command *srpcmd, int rc ) {
	struct srp_device *srpdev = srpcmd->srpdev;

	if ( rc != 0 ) {
		DBGC ( srpdev, "SRP %p tag %08x closed: %s\n",
		       srpdev, srpcmd->tag, strerror ( rc ) );
	}

	/* Remove from list of commands */
	if ( ! list_empty ( &srpcmd->list ) ) {
		list_del ( &srpcmd->list );
		INIT_LIST_HEAD ( &srpcmd->list );
		srpcmd_put ( srpcmd );
	}

	/* Shut down interfaces */
	intf_shutdown ( &srpcmd->scsi, rc );
}

/**
 * Close SRP device
 *
 * @v srpdev		SRP device
 * @v rc		Reason for close
 */
static void srpdev_close ( struct srp_device *srpdev, int rc ) {
	struct srp_command *srpcmd;
	struct srp_command *tmp;

	if ( rc != 0 ) {
		DBGC ( srpdev, "SRP %p closed: %s\n",
		       srpdev, strerror ( rc ) );
	}

	/* Shut down interfaces */
	intf_shutdown ( &srpdev->socket, rc );
	intf_shutdown ( &srpdev->scsi, rc );

	/* Shut down any active commands */
	list_for_each_entry_safe ( srpcmd, tmp, &srpdev->commands, list ) {
		srpcmd_get ( srpcmd );
		srpcmd_close ( srpcmd, rc );
		srpcmd_put ( srpcmd );
	}
}

/**
 * Identify SRP command by tag
 *
 * @v srpdev		SRP device
 * @v tag		Command tag
 * @ret srpcmd		SRP command, or NULL
 */
static struct srp_command * srp_find_tag ( struct srp_device *srpdev,
					   uint32_t tag ) {
	struct srp_command *srpcmd;

	list_for_each_entry ( srpcmd, &srpdev->commands, list ) {
		if ( srpcmd->tag == tag )
			return srpcmd;
	}
	return NULL;
}

/**
 * Choose an SRP command tag
 *
 * @v srpdev		SRP device
 * @ret tag		New tag, or negative error
 */
static int srp_new_tag ( struct srp_device *srpdev ) {
	static uint16_t tag_idx;
	unsigned int i;

	for ( i = 0 ; i < 65536 ; i++ ) {
		tag_idx++;
		if ( srp_find_tag ( srpdev, tag_idx ) == NULL )
			return tag_idx;
	}
	return -EADDRINUSE;
}

/**
 * Transmit SRP login request
 *
 * @v srpdev		SRP device
 * @v initiator		Initiator port ID
 * @v target		Target port ID
 * @v tag		Command tag
 * @ret rc		Return status code
 */
static int srp_login ( struct srp_device *srpdev, union srp_port_id *initiator,
		       union srp_port_id *target, uint32_t tag ) {
	struct io_buffer *iobuf;
	struct srp_login_req *login_req;
	int rc;

	/* Allocate I/O buffer */
	iobuf = xfer_alloc_iob ( &srpdev->socket, sizeof ( *login_req ) );
	if ( ! iobuf )
		return -ENOMEM;

	/* Construct login request IU */
	login_req = iob_put ( iobuf, sizeof ( *login_req ) );
	memset ( login_req, 0, sizeof ( *login_req ) );
	login_req->type = SRP_LOGIN_REQ;
	login_req->tag.dwords[0] = htonl ( SRP_TAG_MAGIC );
	login_req->tag.dwords[1] = htonl ( tag );
	login_req->max_i_t_iu_len = htonl ( SRP_MAX_I_T_IU_LEN );
	login_req->required_buffer_formats = SRP_LOGIN_REQ_FMT_DDBD;
	memcpy ( &login_req->initiator, initiator,
		 sizeof ( login_req->initiator ) );
	memcpy ( &login_req->target, target, sizeof ( login_req->target ) );

	DBGC ( srpdev, "SRP %p tag %08x LOGIN_REQ:\n", srpdev, tag );
	DBGC_HDA ( srpdev, 0, iobuf->data, iob_len ( iobuf ) );

	/* Send login request IU */
	if ( ( rc = xfer_deliver_iob ( &srpdev->socket, iobuf ) ) != 0 ) {
		DBGC ( srpdev, "SRP %p tag %08x could not send LOGIN_REQ: "
		       "%s\n", srpdev, tag, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Receive SRP login response
 *
 * @v srpdev		SRP device
 * @v data		SRP IU
 * @v len		Length of SRP IU
 * @ret rc		Return status code
 */
static int srp_login_rsp ( struct srp_device *srpdev,
			   const void *data, size_t len ) {
	const struct srp_login_rsp *login_rsp = data;

	/* Sanity check */
	if ( len < sizeof ( *login_rsp ) ) {
		DBGC ( srpdev, "SRP %p LOGIN_RSP too short (%zd bytes)\n",
		       srpdev, len );
		return -EINVAL;
	}
	DBGC ( srpdev, "SRP %p tag %08x LOGIN_RSP:\n",
	       srpdev, ntohl ( login_rsp->tag.dwords[1] ) );
	DBGC_HDA ( srpdev, 0, data, len );

	/* Mark as logged in */
	srpdev->logged_in = 1;
	DBGC ( srpdev, "SRP %p logged in\n", srpdev );

	/* Notify of window change */
	xfer_window_changed ( &srpdev->scsi );

	return 0;
}

/**
 * Receive SRP login rejection
 *
 * @v srpdev		SRP device
 * @v data		SRP IU
 * @v len		Length of SRP IU
 * @ret rc		Return status code
 */
static int srp_login_rej ( struct srp_device *srpdev,
			   const void *data, size_t len ) {
	const struct srp_login_rej *login_rej = data;

	/* Sanity check */
	if ( len < sizeof ( *login_rej ) ) {
		DBGC ( srpdev, "SRP %p LOGIN_REJ too short (%zd bytes)\n",
		       srpdev, len );
		return -EINVAL;
	}
	DBGC ( srpdev, "SRP %p tag %08x LOGIN_REJ:\n",
	       srpdev, ntohl ( login_rej->tag.dwords[1] ) );
	DBGC_HDA ( srpdev, 0, data, len );

	/* Login rejection always indicates an error */
	DBGC ( srpdev, "SRP %p login rejected (reason %08x)\n",
	       srpdev, ntohl ( login_rej->reason ) );
	return -EPERM;
}

/**
 * Transmit SRP SCSI command
 *
 * @v srpdev		SRP device
 * @v command		SCSI command
 * @v tag		Command tag
 * @ret rc		Return status code
 */
static int srp_cmd ( struct srp_device *srpdev,
		     struct scsi_cmd *command,
		     uint32_t tag ) {
	struct io_buffer *iobuf;
	struct srp_cmd *cmd;
	struct srp_memory_descriptor *data_out;
	struct srp_memory_descriptor *data_in;
	int rc;

	/* Sanity check */
	if ( ! srpdev->logged_in ) {
		DBGC ( srpdev, "SRP %p tag %08x cannot send CMD before "
		       "login completes\n", srpdev, tag );
		return -EBUSY;
	}

	/* Allocate I/O buffer */
	iobuf = xfer_alloc_iob ( &srpdev->socket, SRP_MAX_I_T_IU_LEN );
	if ( ! iobuf )
		return -ENOMEM;

	/* Construct base portion */
	cmd = iob_put ( iobuf, sizeof ( *cmd ) );
	memset ( cmd, 0, sizeof ( *cmd ) );
	cmd->type = SRP_CMD;
	cmd->tag.dwords[0] = htonl ( SRP_TAG_MAGIC );
	cmd->tag.dwords[1] = htonl ( tag );
	memcpy ( &cmd->lun, &command->lun, sizeof ( cmd->lun ) );
	memcpy ( &cmd->cdb, &command->cdb, sizeof ( cmd->cdb ) );

	/* Construct data-out descriptor, if present */
	if ( command->data_out ) {
		cmd->data_buffer_formats |= SRP_CMD_DO_FMT_DIRECT;
		data_out = iob_put ( iobuf, sizeof ( *data_out ) );
		data_out->address =
		    cpu_to_be64 ( user_to_phys ( command->data_out, 0 ) );
		data_out->handle = ntohl ( srpdev->memory_handle );
		data_out->len = ntohl ( command->data_out_len );
	}

	/* Construct data-in descriptor, if present */
	if ( command->data_in ) {
		cmd->data_buffer_formats |= SRP_CMD_DI_FMT_DIRECT;
		data_in = iob_put ( iobuf, sizeof ( *data_in ) );
		data_in->address =
		     cpu_to_be64 ( user_to_phys ( command->data_in, 0 ) );
		data_in->handle = ntohl ( srpdev->memory_handle );
		data_in->len = ntohl ( command->data_in_len );
	}

	DBGC2 ( srpdev, "SRP %p tag %08x CMD " SCSI_CDB_FORMAT "\n",
		srpdev, tag, SCSI_CDB_DATA ( cmd->cdb ) );

	/* Send IU */
	if ( ( rc = xfer_deliver_iob ( &srpdev->socket, iobuf ) ) != 0 ) {
		DBGC ( srpdev, "SRP %p tag %08x could not send CMD: %s\n",
		       srpdev, tag, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/**
 * Receive SRP SCSI response
 *
 * @v srpdev		SRP device
 * @v data		SRP IU
 * @v len		Length of SRP IU
 * @ret rc		Returns status code
 */
static int srp_rsp ( struct srp_device *srpdev,
		     const void *data, size_t len ) {
	const struct srp_rsp *rsp = data;
	struct srp_command *srpcmd;
	struct scsi_rsp response;
	const void *sense;
	ssize_t data_out_residual_count;
	ssize_t data_in_residual_count;

	/* Sanity check */
	if ( len < sizeof ( *rsp ) ) {
		DBGC ( srpdev, "SRP %p RSP too short (%zd bytes)\n",
		       srpdev, len );
		return -EINVAL;
	}
	DBGC2 ( srpdev, "SRP %p tag %08x RSP stat %02x dores %08x dires "
		"%08x valid %02x%s%s%s%s%s%s\n",
		srpdev, ntohl ( rsp->tag.dwords[1] ), rsp->status,
		ntohl ( rsp->data_out_residual_count ),
		ntohl ( rsp->data_in_residual_count ), rsp->valid,
		( ( rsp->valid & SRP_RSP_VALID_DIUNDER ) ? " diunder" : "" ),
		( ( rsp->valid & SRP_RSP_VALID_DIOVER ) ? " diover" : "" ),
		( ( rsp->valid & SRP_RSP_VALID_DOUNDER ) ? " dounder" : "" ),
		( ( rsp->valid & SRP_RSP_VALID_DOOVER ) ? " doover" : "" ),
		( ( rsp->valid & SRP_RSP_VALID_SNSVALID ) ? " sns" : "" ),
		( ( rsp->valid & SRP_RSP_VALID_RSPVALID ) ? " rsp" : "" ) );

	/* Identify command by tag */
	srpcmd = srp_find_tag ( srpdev, ntohl ( rsp->tag.dwords[1] ) );
	if ( ! srpcmd ) {
		DBGC ( srpdev, "SRP %p tag %08x unrecognised RSP\n",
		       srpdev, ntohl ( rsp->tag.dwords[1] ) );
		return -ENOENT;
	}

	/* Hold command reference for remainder of function */
	srpcmd_get ( srpcmd );

	/* Build SCSI response */
	memset ( &response, 0, sizeof ( response ) );
	response.status = rsp->status;
	data_out_residual_count = ntohl ( rsp->data_out_residual_count );
	data_in_residual_count = ntohl ( rsp->data_in_residual_count );
	if ( rsp->valid & SRP_RSP_VALID_DOOVER ) {
		response.overrun = data_out_residual_count;
	} else if ( rsp->valid & SRP_RSP_VALID_DOUNDER ) {
		response.overrun = -(data_out_residual_count);
	} else if ( rsp->valid & SRP_RSP_VALID_DIOVER ) {
		response.overrun = data_in_residual_count;
	} else if ( rsp->valid & SRP_RSP_VALID_DIUNDER ) {
		response.overrun = -(data_in_residual_count);
	}
	sense = srp_rsp_sense_data ( rsp );
	if ( sense )
		memcpy ( &response.sense, sense, sizeof ( response.sense ) );

	/* Report SCSI response */
	scsi_response ( &srpcmd->scsi, &response );

	/* Close SCSI command */
	srpcmd_close ( srpcmd, 0 );

	/* Drop temporary command reference */
	srpcmd_put ( srpcmd );

	return 0;
}

/**
 * Receive SRP unrecognised response IU
 *
 * @v srpdev		SRP device
 * @v data		SRP IU
 * @v len		Length of SRP IU
 * @ret rc		Returns status code
 */
static int srp_unrecognised ( struct srp_device *srpdev,
			      const void *data, size_t len ) {
	const struct srp_common *common = data;

	DBGC ( srpdev, "SRP %p tag %08x unrecognised IU type %02x:\n",
	       srpdev, ntohl ( common->tag.dwords[1] ), common->type );
	DBGC_HDA ( srpdev, 0, data, len );

	return -ENOTSUP;
}

/** SRP command SCSI interface operations */
static struct interface_operation srpcmd_scsi_op[] = {
	INTF_OP ( intf_close, struct srp_command *, srpcmd_close ),
};

/** SRP command SCSI interface descriptor */
static struct interface_descriptor srpcmd_scsi_desc =
	INTF_DESC ( struct srp_command, scsi, srpcmd_scsi_op );

/**
 * Issue SRP SCSI command
 *
 * @v srpdev		SRP device
 * @v parent		Parent interface
 * @v command		SCSI command
 * @ret tag		Command tag, or negative error
 */
static int srpdev_scsi_command ( struct srp_device *srpdev,
				 struct interface *parent,
				 struct scsi_cmd *command ) {
	struct srp_command *srpcmd;
	int tag;
	int rc;

	/* Allocate command tag */
	tag = srp_new_tag ( srpdev );
	if ( tag < 0 ) {
		rc = tag;
		goto err_tag;
	}

	/* Allocate and initialise structure */
	srpcmd = zalloc ( sizeof ( *srpcmd ) );
	if ( ! srpcmd ) {
		rc = -ENOMEM;
		goto err_zalloc;
	}
	ref_init ( &srpcmd->refcnt, srpcmd_free );
	intf_init ( &srpcmd->scsi, &srpcmd_scsi_desc, &srpcmd->refcnt );
	srpcmd->srpdev = srpdev_get ( srpdev );
	list_add ( &srpcmd->list, &srpdev->commands );
	srpcmd->tag = tag;

	/* Send command IU */
	if ( ( rc = srp_cmd ( srpdev, command, srpcmd->tag ) ) != 0 )
		goto err_cmd;

	/* Attach to parent interface, leave reference with command
	 * list, and return.
	 */
	intf_plug_plug ( &srpcmd->scsi, parent );
	return srpcmd->tag;

 err_cmd:
	srpcmd_close ( srpcmd, rc );
 err_zalloc:
 err_tag:
	return rc;
}

/**
 * Receive data from SRP socket
 *
 * @v srpdev		SRP device
 * @v iobuf		Datagram I/O buffer
 * @v meta		Data transfer metadata
 * @ret rc		Return status code
 */
static int srpdev_deliver ( struct srp_device *srpdev,
			    struct io_buffer *iobuf,
			    struct xfer_metadata *meta __unused ) {
	struct srp_common *common = iobuf->data;
	int ( * type ) ( struct srp_device *srp, const void *data, size_t len );
	int rc;

	/* Sanity check */
	if ( iob_len ( iobuf ) < sizeof ( *common ) ) {
		DBGC ( srpdev, "SRP %p IU too short (%zd bytes)\n",
		       srpdev, iob_len ( iobuf ) );
		rc = -EINVAL;
		goto err;
	}

	/* Determine IU type */
	switch ( common->type ) {
	case SRP_LOGIN_RSP:
		type = srp_login_rsp;
		break;
	case SRP_LOGIN_REJ:
		type = srp_login_rej;
		break;
	case SRP_RSP:
		type = srp_rsp;
		break;
	default:
		type = srp_unrecognised;
		break;
	}

	/* Handle IU */
	if ( ( rc = type ( srpdev, iobuf->data, iob_len ( iobuf ) ) ) != 0 )
		goto err;

	free_iob ( iobuf );
	return 0;

 err:
	DBGC ( srpdev, "SRP %p closing due to received IU (%s):\n",
	       srpdev, strerror ( rc ) );
	DBGC_HDA ( srpdev, 0, iobuf->data, iob_len ( iobuf ) );
	free_iob ( iobuf );
	srpdev_close ( srpdev, rc );
	return rc;
}

/**
 * Check SRP device flow-control window
 *
 * @v srpdev		SRP device
 * @ret len		Length of window
 */
static size_t srpdev_window ( struct srp_device *srpdev ) {
	return ( srpdev->logged_in ? ~( ( size_t ) 0 ) : 0 );
}

/**
 * A (transport-independent) sBFT created by iPXE
 */
struct ipxe_sbft {
	/** The table header */
	struct sbft_table table;
	/** The SCSI subtable */
	struct sbft_scsi_subtable scsi;
	/** The SRP subtable */
	struct sbft_srp_subtable srp;
} __attribute__ (( packed, aligned ( 16 ) ));

/**
 * Describe SRP device in an ACPI table
 *
 * @v srpdev		SRP device
 * @v acpi		ACPI table
 * @v len		Length of ACPI table
 * @ret rc		Return status code
 */
static int srpdev_describe ( struct srp_device *srpdev,
			     struct acpi_description_header *acpi,
			     size_t len ) {
	struct ipxe_sbft *sbft =
		container_of ( acpi, struct ipxe_sbft, table.acpi );
	int rc;

	/* Sanity check */
	if ( len < sizeof ( *sbft ) )
		return -ENOBUFS;

	/* Populate table */
	sbft->table.acpi.signature = cpu_to_le32 ( SBFT_SIG );
	sbft->table.acpi.length = cpu_to_le32 ( sizeof ( *sbft ) );
	sbft->table.acpi.revision = 1;
	sbft->table.scsi_offset =
		cpu_to_le16 ( offsetof ( typeof ( *sbft ), scsi ) );
	memcpy ( &sbft->scsi.lun, &srpdev->lun, sizeof ( sbft->scsi.lun ) );
	sbft->table.srp_offset =
		cpu_to_le16 ( offsetof ( typeof ( *sbft ), srp ) );
	memcpy ( &sbft->srp.initiator, &srpdev->initiator,
		 sizeof ( sbft->srp.initiator ) );
	memcpy ( &sbft->srp.target, &srpdev->target,
		 sizeof ( sbft->srp.target ) );

	/* Ask transport layer to describe transport-specific portions */
	if ( ( rc = acpi_describe ( &srpdev->socket, acpi, len ) ) != 0 ) {
		DBGC ( srpdev, "SRP %p cannot describe transport layer: %s\n",
		       srpdev, strerror ( rc ) );
		return rc;
	}

	return 0;
}

/** SRP device socket interface operations */
static struct interface_operation srpdev_socket_op[] = {
	INTF_OP ( xfer_deliver, struct srp_device *, srpdev_deliver ),
	INTF_OP ( intf_close, struct srp_device *, srpdev_close ),
};

/** SRP device socket interface descriptor */
static struct interface_descriptor srpdev_socket_desc =
	INTF_DESC ( struct srp_device, socket, srpdev_socket_op );

/** SRP device SCSI interface operations */
static struct interface_operation srpdev_scsi_op[] = {
	INTF_OP ( scsi_command, struct srp_device *, srpdev_scsi_command ),
	INTF_OP ( xfer_window, struct srp_device *, srpdev_window ),
	INTF_OP ( intf_close, struct srp_device *, srpdev_close ),
	INTF_OP ( acpi_describe, struct srp_device *, srpdev_describe ),
};

/** SRP device SCSI interface descriptor */
static struct interface_descriptor srpdev_scsi_desc =
	INTF_DESC ( struct srp_device, scsi, srpdev_scsi_op );

/**
 * Open SRP device
 *
 * @v block		Block control interface
 * @v socket		Socket interface
 * @v initiator		Initiator port ID
 * @v target		Target port ID
 * @v memory_handle	RDMA memory handle
 * @v lun		SCSI LUN
 * @ret rc		Return status code
 */
int srp_open ( struct interface *block, struct interface *socket,
	       union srp_port_id *initiator, union srp_port_id *target,
	       uint32_t memory_handle, struct scsi_lun *lun ) {
	struct srp_device *srpdev;
	int tag;
	int rc;

	/* Allocate and initialise structure */
	srpdev = zalloc ( sizeof ( *srpdev ) );
	if ( ! srpdev ) {
		rc = -ENOMEM;
		goto err_zalloc;
	}
	ref_init ( &srpdev->refcnt, NULL );
	intf_init ( &srpdev->scsi, &srpdev_scsi_desc, &srpdev->refcnt );
	intf_init ( &srpdev->socket, &srpdev_socket_desc, &srpdev->refcnt );
	INIT_LIST_HEAD ( &srpdev->commands );
	srpdev->memory_handle = memory_handle;
	DBGC ( srpdev, "SRP %p %08x%08x%08x%08x->%08x%08x%08x%08x\n", srpdev,
	       ntohl ( initiator->dwords[0] ), ntohl ( initiator->dwords[1] ),
	       ntohl ( initiator->dwords[2] ), ntohl ( initiator->dwords[3] ),
	       ntohl ( target->dwords[0] ), ntohl ( target->dwords[1] ),
	       ntohl ( target->dwords[2] ), ntohl ( target->dwords[3] ) );

	/* Preserve parameters required for boot firmware table */
	memcpy ( &srpdev->initiator, initiator, sizeof ( srpdev->initiator ) );
	memcpy ( &srpdev->target, target, sizeof ( srpdev->target ) );
	memcpy ( &srpdev->lun, lun, sizeof ( srpdev->lun ) );

	/* Attach to socket interface and initiate login */
	intf_plug_plug ( &srpdev->socket, socket );
	tag = srp_new_tag ( srpdev );
	assert ( tag >= 0 ); /* Cannot fail when no commands in progress */
	if ( ( rc = srp_login ( srpdev, initiator, target, tag ) ) != 0 )
		goto err_login;

	/* Attach SCSI device to parent interface */
	if ( ( rc = scsi_open ( block, &srpdev->scsi, lun ) ) != 0 ) {
		DBGC ( srpdev, "SRP %p could not create SCSI device: %s\n",
		       srpdev, strerror ( rc ) );
		goto err_scsi_open;
	}

	/* Mortalise self and return */
	ref_put ( &srpdev->refcnt );
	return 0;

 err_scsi_open:
 err_login:
	srpdev_close ( srpdev, rc );
	ref_put ( &srpdev->refcnt );
 err_zalloc:
	return rc;
}
