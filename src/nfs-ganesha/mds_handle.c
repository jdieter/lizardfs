/*
   Copyright 2017 Skytechnology sp. z o.o.

   This file is part of LizardFS.

   LizardFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LizardFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LizardFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "fsal.h"
#include "fsal_api.h"
#include "fsal_types.h"
#include "fsal_up.h"
#include "FSAL/fsal_commonlib.h"
#include "pnfs_utils.h"

#include "context_wrap.h"
#include "lzfs_internal.h"
#include "protocol/MFSCommunication.h"

/*! \brief Grant a layout segment.
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 lzfs_fsal_layoutget(struct fsal_obj_handle *obj_pub, struct req_op_context *req_ctx,
                                    XDR *loc_body, const struct fsal_layoutget_arg *arg,
                                    struct fsal_layoutget_res *res) {
	struct lzfs_fsal_handle *lzfs_hdl;
	struct lzfs_fsal_ds_wire ds_wire;
	struct gsh_buffdesc ds_desc = {.addr = &ds_wire, .len = sizeof(struct lzfs_fsal_ds_wire)};
	struct pnfs_deviceid deviceid = DEVICE_ID_INIT_ZERO(FSAL_ID_EXPERIMENTAL);
	nfl_util4 layout_util = 0;
	nfsstat4 nfs_status = NFS4_OK;

	lzfs_hdl = container_of(obj_pub, struct lzfs_fsal_handle, handle);

	if (arg->type != LAYOUT4_NFSV4_1_FILES) {
		LogMajor(COMPONENT_PNFS, "Unsupported layout type: %x", arg->type);

		return NFS4ERR_UNKNOWN_LAYOUTTYPE;
	}

	LogDebug(COMPONENT_PNFS, "will issue layout offset: %" PRIu64 " length: %" PRIu64,
	         res->segment.offset, res->segment.length);

	deviceid.devid = lzfs_hdl->inode;
#if (BYTE_ORDER != BIG_ENDIAN)
	ds_wire.inode = bswap_32(lzfs_hdl->inode);
#else
	ds_wire.inode = lzfs_hdl->inode;
#endif
	layout_util = MFSCHUNKSIZE;

	nfs_status = FSAL_encode_file_layout(loc_body, &deviceid, layout_util, 0, 0,
	                                     &req_ctx->ctx_export->export_id, 1, &ds_desc);
	if (nfs_status) {
		LogMajor(COMPONENT_PNFS, "Failed to encode nfsv4_1_file_layout.");
		return nfs_status;
	}

	res->return_on_close = true;
	res->last_segment = true;

	return nfs_status;
}

/*! \brief Potentially return one layout segment
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 lzfs_fsal_layoutreturn(struct fsal_obj_handle *obj_pub,
                                       struct req_op_context *req_ctx, XDR *lrf_body,
                                       const struct fsal_layoutreturn_arg *arg) {
	if (arg->lo_type != LAYOUT4_NFSV4_1_FILES) {
		LogDebug(COMPONENT_PNFS, "Unsupported layout type: %x", arg->lo_type);
		return NFS4ERR_UNKNOWN_LAYOUTTYPE;
	}

	return NFS4_OK;
}

/*! \brief Commit a segment of a layout
 *
 * \see fsal_api.h for more information
 */
static nfsstat4 lzfs_fsal_layoutcommit(struct fsal_obj_handle *obj_pub,
                                       struct req_op_context *req_ctx, XDR *lou_body,
                                       const struct fsal_layoutcommit_arg *arg,
                                       struct fsal_layoutcommit_res *res) {
	struct lzfs_fsal_export *lzfs_export;
	struct lzfs_fsal_handle *lzfs_hdl;
	struct liz_attr_reply lzfs_old;
	int rc;

	// FIXME(haze): Does this function make sense for our implementation ?

	/* Sanity check on type */
	if (arg->type != LAYOUT4_NFSV4_1_FILES) {
		LogCrit(COMPONENT_PNFS, "Unsupported layout type: %x", arg->type);
		return NFS4ERR_UNKNOWN_LAYOUTTYPE;
	}

	lzfs_export = container_of(req_ctx->fsal_export, struct lzfs_fsal_export, export);
	lzfs_hdl = container_of(obj_pub, struct lzfs_fsal_handle, handle);

	rc = liz_cred_getattr(lzfs_export->lzfs_instance, op_ctx->creds, lzfs_hdl->inode, &lzfs_old);
	if (rc < 0) {
		LogCrit(COMPONENT_PNFS, "Error '%s' in attempt to get attributes of file %lli.",
		        liz_error_string(liz_last_err()), (long long)lzfs_hdl->inode);
		return lzfs_nfs4_last_err();
	}

	struct stat attr;
	int mask = 0;

	memset(&attr, 0, sizeof(attr));

	if (arg->new_offset && lzfs_old.attr.st_size < arg->last_write + 1) {
		mask |= LIZ_SET_ATTR_SIZE;
		attr.st_size = arg->last_write + 1;
		res->size_supplied = true;
		res->new_size = arg->last_write + 1;
	}

	if (arg->time_changed && (arg->new_time.seconds > lzfs_old.attr.st_mtim.tv_sec ||
	                          (arg->new_time.seconds == lzfs_old.attr.st_mtim.tv_sec &&
	                           arg->new_time.nseconds > lzfs_old.attr.st_mtim.tv_nsec))) {
		attr.st_mtim.tv_sec = arg->new_time.seconds;
		attr.st_mtim.tv_sec = arg->new_time.nseconds;
		mask |= LIZ_SET_ATTR_MTIME;
	}

	liz_attr_reply_t reply;
	rc = liz_cred_setattr(lzfs_export->lzfs_instance, op_ctx->creds, lzfs_hdl->inode, &attr, mask,
	                      &reply);

	if (rc < 0) {
		LogCrit(COMPONENT_PNFS, "Error '%s' in attempt to set attributes of file %lli.",
		        liz_error_string(liz_last_err()), (long long)lzfs_hdl->inode);
		return lzfs_nfs4_last_err();
	}

	res->commit_done = true;

	return NFS4_OK;
}

void lzfs_fsal_handle_ops_pnfs(struct fsal_obj_ops *ops) {
	ops->layoutget = lzfs_fsal_layoutget;
	ops->layoutreturn = lzfs_fsal_layoutreturn;
	ops->layoutcommit = lzfs_fsal_layoutcommit;
}
