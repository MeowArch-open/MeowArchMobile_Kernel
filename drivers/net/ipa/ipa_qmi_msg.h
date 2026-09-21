/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2024 Linaro Ltd.
 */
#ifndef _IPA_QMI_MSG_H_
#define _IPA_QMI_MSG_H_

/* === Only "ipa_qmi" and "ipa_qmi_msg.c" should include this file === */

#include <linux/types.h>

#include <linux/soc/qcom/qmi.h>

/* Request/response/indication QMI message ids used for IPA.  Receiving
 * end issues a response for requests; indications require no response.
 */
#define IPA_QMI_INDICATION_REGISTER	0x20	/* modem -> AP request */
#define IPA_QMI_INIT_DRIVER		0x21	/* AP -> modem request */
#define IPA_QMI_INIT_COMPLETE		0x22	/* AP -> modem indication */
#define IPA_QMI_DRIVER_INIT_COMPLETE	0x35	/* modem -> AP request */

/* Messages this modem sends that mainline has no implementation for.  Ids
 * confirmed against Qualcomm's ipa_qmi_service_v01.h and against what actually
 * arrives on the wire (see the ipa_qmi.c snoop).
 *
 *   0x23 INSTALL_FILTER_RULE     modem -> AP request, ~1.5 KB of filter specs
 *   0x24 FILTER_INSTALLED_NOTIF  modem -> AP request
 *   0x27 CONFIG                  modem -> AP request
 */
#define IPA_QMI_INSTALL_FILTER_RULE	0x23	/* modem -> AP request */
#define IPA_QMI_FILTER_INSTALLED_NOTIF	0x24	/* modem -> AP request */
#define IPA_QMI_CONFIG			0x27	/* modem -> AP request */

/* Wire layout of one ipa_filter_spec_ex_type_v01, as this MPSS encodes it:
 *
 *     0   u32  ip_type
 *     4   ...  filter_rule (struct ipa_filter_rule_type_v01, 150 bytes)
 *   154   u32  filter_action
 *   158   u8   is_routing_table_index_valid
 *   159   u32  route_table_index
 *   163   u8   is_mux_id_valid
 *   164   u32  mux_id
 *   168   u32  rule_id
 *   172   u8   is_rule_hashable
 *
 * Only the rule id matters to us -- it is the identifier the modem expects back
 * in the response -- so the rest stays opaque.  The element size was confirmed
 * from a captured request: 9 specs in 1558 bytes, one byte of count and no
 * remainder.
 */
#define IPA_QMI_FILTER_SPEC_EX_TLV	0x15
#define IPA_QMI_FILTER_SPEC_EX_SZ	173
#define IPA_QMI_FILTER_SPEC_EX_RULE_ID	168

/* QMI_IPA_MAX_FILTERS_EX_V01 */
#define IPA_QMI_MAX_FILTERS		128

/* Request message for the FILTER_INSTALLED_NOTIF request (AP -> modem).
 *
 * After the modem asks the AP to install its uplink filter specs, the AP is
 * expected to report back where they went: the pipe they were installed on, an
 * overall status, and a handle-to-index map.  Only the first three fields are
 * mandatory; the rule id list is how the AP acknowledges specs that arrived
 * through filter_spec_ex_list.
 */
struct ipa_filter_index_map {
	u32			filter_handle;
	u32			filter_index;
};

struct ipa_fltr_installed_notif_req {
	u32			source_pipe_index;
	u16			install_status;
	u32			filter_index_list_len;
	struct ipa_filter_index_map filter_index_list[IPA_QMI_MAX_FILTERS];

	u8			rule_id_valid;
	u32			rule_id_len;
	u32			rule_id[IPA_QMI_MAX_FILTERS];
};

#define IPA_QMI_FILTER_INSTALLED_NOTIF_REQ_SZ				\
	(3 + 4 + 3 + 2 + 3 + 1 + IPA_QMI_MAX_FILTERS * 8		\
	 + 3 + 1 + IPA_QMI_MAX_FILTERS * 4)

extern const struct qmi_elem_info ipa_fltr_installed_notif_req_ei[];

/* The maximum size required for message types.  These sizes include
 * the message data, along with type (1 byte) and length (2 byte)
 * information for each field.  The qmi_send_*() interfaces require
 * the message size to be provided.
 */
#define IPA_QMI_INDICATION_REGISTER_REQ_SZ	20	/* -> server handle */
#define IPA_QMI_INDICATION_REGISTER_RSP_SZ	7	/* <- server handle */
#define IPA_QMI_INIT_DRIVER_REQ_SZ		162	/* client handle -> */
#define IPA_QMI_INIT_DRIVER_RSP_SZ		25	/* client handle <- */
#define IPA_QMI_INIT_COMPLETE_IND_SZ		7	/* <- server handle */
#define IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ	4	/* -> server handle */
#define IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ	7	/* <- server handle */

/* Maximum size of messages we expect the AP to receive (max of above)
 *
 * zorn (SM8650, MPSS.DE.5.0): these were 8 and 25, sized for the four messages
 * mainline implements.  QRTR is a datagram socket, so anything longer than
 * recv_buf_size is *truncated* by qrtr_recvmsg() and the rest is discarded --
 * a larger request from the modem (e.g. INSTALL_FILTER_RULE, whose filter spec
 * list runs to hundreds of bytes) can never be read even in principle.  Use a
 * page so we can at least see what the modem sends.
 */
#define IPA_QMI_SERVER_MAX_RCV_SZ		4096
#define IPA_QMI_CLIENT_MAX_RCV_SZ		4096

/* Request message for the IPA_QMI_INDICATION_REGISTER request */
struct ipa_indication_register_req {
	u8 master_driver_init_complete_valid;
	u8 master_driver_init_complete;
	u8 data_usage_quota_reached_valid;
	u8 data_usage_quota_reached;
	u8 ipa_mhi_ready_ind_valid;
	u8 ipa_mhi_ready_ind;
	u8 endpoint_desc_ind_valid;
	u8 endpoint_desc_ind;
	u8 bw_change_ind_valid;
	u8 bw_change_ind;
};

/* The response to a IPA_QMI_INDICATION_REGISTER request consists only of
 * a standard QMI response.
 */
struct ipa_indication_register_rsp {
	struct qmi_response_type_v01 rsp;
};

/* Request message for the IPA_QMI_DRIVER_INIT_COMPLETE request */
struct ipa_driver_init_complete_req {
	u8 status;
};

/* The response to a IPA_QMI_DRIVER_INIT_COMPLETE request consists only
 * of a standard QMI response.
 */
struct ipa_driver_init_complete_rsp {
	struct qmi_response_type_v01 rsp;
};

/* The message for the IPA_QMI_INIT_COMPLETE_IND indication consists
 * only of a standard QMI response.
 */
struct ipa_init_complete_ind {
	struct qmi_response_type_v01 status;
};

/* The AP tells the modem its platform type.  We assume Android. */
enum ipa_platform_type {
	IPA_QMI_PLATFORM_TYPE_INVALID		= 0x0,	/* Invalid */
	IPA_QMI_PLATFORM_TYPE_TN		= 0x1,	/* Data card */
	IPA_QMI_PLATFORM_TYPE_LE		= 0x2,	/* Data router */
	IPA_QMI_PLATFORM_TYPE_MSM_ANDROID	= 0x3,	/* Android MSM */
	IPA_QMI_PLATFORM_TYPE_MSM_WINDOWS	= 0x4,	/* Windows MSM */
	IPA_QMI_PLATFORM_TYPE_MSM_QNX_V01	= 0x5,	/* QNX MSM */
};

/* This defines the start and end offset of a range of memory.  The start
 * value is a byte offset relative to the start of IPA shared memory.  The
 * end value is the last addressable unit *within* the range.  Typically
 * the end value is in units of bytes, however it can also be a maximum
 * array index value.
 */
struct ipa_mem_bounds {
	u32 start;
	u32 end;
};

/* This defines the location and size of an array.  The start value
 * is an offset relative to the start of IPA shared memory.  The
 * size of the array is implied by the number of entries (the entry
 * size is assumed to be known).
 */
struct ipa_mem_array {
	u32 start;
	u32 count;
};

/* This defines the location and size of a range of memory.  The
 * start is an offset relative to the start of IPA shared memory.
 * This differs from the ipa_mem_bounds structure in that the size
 * (in bytes) of the memory region is specified rather than the
 * offset of its last byte.
 */
struct ipa_mem_range {
	u32 start;
	u32 size;
};

/* The message for the IPA_QMI_INIT_DRIVER request contains information
 * from the AP that affects modem initialization.
 */
struct ipa_init_modem_driver_req {
	u8			platform_type_valid;
	u32			platform_type;	/* enum ipa_platform_type */

	/* Modem header table information.  This defines the IPA shared
	 * memory in which the modem may insert header table entries.
	 */
	u8			hdr_tbl_info_valid;
	struct ipa_mem_bounds	hdr_tbl_info;

	/* Routing table information.  These define the location and maximum
	 * *index* (not byte) for the modem portion of non-hashable IPv4 and
	 * IPv6 routing tables.  The start values are byte offsets relative
	 * to the start of IPA shared memory.
	 */
	u8			v4_route_tbl_info_valid;
	struct ipa_mem_bounds	v4_route_tbl_info;
	u8			v6_route_tbl_info_valid;
	struct ipa_mem_bounds	v6_route_tbl_info;

	/* Filter table information.  These define the location of the
	 * non-hashable IPv4 and IPv6 filter tables.  The start values are
	 * byte offsets relative to the start of IPA shared memory.
	 */
	u8			v4_filter_tbl_start_valid;
	u32			v4_filter_tbl_start;
	u8			v6_filter_tbl_start_valid;
	u32			v6_filter_tbl_start;

	/* Modem memory information.  This defines the location and
	 * size of memory available for the modem to use.
	 */
	u8			modem_mem_info_valid;
	struct ipa_mem_range	modem_mem_info;

	/* This defines the destination endpoint on the AP to which
	 * the modem driver can send control commands.  Must be less
	 * than ipa_endpoint_max().
	 */
	u8			ctrl_comm_dest_end_pt_valid;
	u32			ctrl_comm_dest_end_pt;

	/* This defines whether the modem should load the microcontroller
	 * or not.  It is unnecessary to reload it if the modem is being
	 * restarted.
	 *
	 * NOTE: this field is named "is_ssr_bootup" elsewhere.
	 */
	u8			skip_uc_load_valid;
	u8			skip_uc_load;

	/* Processing context memory information.  This defines the memory in
	 * which the modem may insert header processing context table entries.
	 */
	u8			hdr_proc_ctx_tbl_info_valid;
	struct ipa_mem_bounds	hdr_proc_ctx_tbl_info;

	/* Compression command memory information.  This defines the memory
	 * in which the modem may insert compression/decompression commands.
	 */
	u8			zip_tbl_info_valid;
	struct ipa_mem_bounds	zip_tbl_info;

	/* Routing table information.  These define the location and maximum
	 * *index* (not byte) for the modem portion of hashable IPv4 and IPv6
	 * routing tables (if supported by hardware).  The start values are
	 * byte offsets relative to the start of IPA shared memory.
	 */
	u8			v4_hash_route_tbl_info_valid;
	struct ipa_mem_bounds	v4_hash_route_tbl_info;
	u8			v6_hash_route_tbl_info_valid;
	struct ipa_mem_bounds	v6_hash_route_tbl_info;

	/* Filter table information.  These define the location and size
	 * of hashable IPv4 and IPv6 filter tables (if supported by hardware).
	 * The start values are byte offsets relative to the start of IPA
	 * shared memory.
	 */
	u8			v4_hash_filter_tbl_start_valid;
	u32			v4_hash_filter_tbl_start;
	u8			v6_hash_filter_tbl_start_valid;
	u32			v6_hash_filter_tbl_start;

	/* Statistics information.  These define the locations of the
	 * first and last statistics sub-regions.  (IPA v4.0 and above)
	 */
	u8			hw_stats_quota_base_addr_valid;
	u32			hw_stats_quota_base_addr;
	u8			hw_stats_quota_size_valid;
	u32			hw_stats_quota_size;
	u8			hw_stats_drop_base_addr_valid;
	u32			hw_stats_drop_base_addr;
	u8			hw_stats_drop_size_valid;
	u32			hw_stats_drop_size;
};

/* The response to a IPA_QMI_INIT_DRIVER request begins with a standard
 * QMI response, but contains other information as well.  Currently we
 * simply wait for the INIT_DRIVER transaction to complete and
 * ignore any other data that might be returned.
 */
struct ipa_init_modem_driver_rsp {
	struct qmi_response_type_v01	rsp;

	/* This defines the destination endpoint on the modem to which
	 * the AP driver can send control commands.  Must be less than
	 * ipa_endpoint_max().
	 */
	u8				ctrl_comm_dest_end_pt_valid;
	u32				ctrl_comm_dest_end_pt;

	/* This defines the default endpoint.  The AP driver is not
	 * required to configure the hardware with this value.  Must
	 * be less than ipa_endpoint_max().
	 */
	u8				default_end_pt_valid;
	u32				default_end_pt;

	/* This defines whether a second handshake is required to complete
	 * initialization.
	 */
	u8				modem_driver_init_pending_valid;
	u8				modem_driver_init_pending;
};

/* Message structure definitions defined in "ipa_qmi_msg.c" */
extern const struct qmi_elem_info ipa_indication_register_req_ei[];
extern const struct qmi_elem_info ipa_indication_register_rsp_ei[];
extern const struct qmi_elem_info ipa_driver_init_complete_req_ei[];
extern const struct qmi_elem_info ipa_driver_init_complete_rsp_ei[];
extern const struct qmi_elem_info ipa_init_complete_ind_ei[];
extern const struct qmi_elem_info ipa_mem_bounds_ei[];
extern const struct qmi_elem_info ipa_mem_array_ei[];
extern const struct qmi_elem_info ipa_mem_range_ei[];
extern const struct qmi_elem_info ipa_init_modem_driver_req_ei[];
extern const struct qmi_elem_info ipa_init_modem_driver_rsp_ei[];

#endif /* !_IPA_QMI_MSG_H_ */
