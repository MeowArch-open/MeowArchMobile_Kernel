// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018-2024 Linaro Ltd.
 */

#include <linux/idr.h>
#include <linux/printk.h>
#include <linux/qrtr.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "ipa.h"
#include "ipa_mem.h"
#include "ipa_modem.h"
#include "ipa_qmi_msg.h"

/**
 * DOC: AP/Modem QMI Handshake
 *
 * The AP and modem perform a "handshake" at initialization time to ensure
 * both sides know when everything is ready to begin operating.  The AP
 * driver (this code) uses two QMI handles (endpoints) for this; a client
 * using a service on the modem, and server to service modem requests (and
 * to supply an indication message from the AP).  Once the handshake is
 * complete, the AP and modem may begin IPA operation.  This occurs
 * only when the AP IPA driver, modem IPA driver, and IPA microcontroller
 * are ready.
 *
 * The QMI service on the modem expects to receive an INIT_DRIVER request from
 * the AP, which contains parameters used by the modem during initialization.
 * The AP sends this request as soon as it is knows the modem side service
 * is available.  The modem responds to this request, and if this response
 * contains a success result, the AP knows the modem IPA driver is ready.
 *
 * The modem is responsible for loading firmware on the IPA microcontroller.
 * This occurs only during the initial modem boot.  The modem sends a
 * separate DRIVER_INIT_COMPLETE request to the AP to report that the
 * microcontroller is ready.  The AP may assume the microcontroller is
 * ready and remain so (even if the modem reboots) once it has received
 * and responded to this request.
 *
 * There is one final exchange involved in the handshake.  It is required
 * on the initial modem boot, but optional (but in practice does occur) on
 * subsequent boots.  The modem expects to receive a final INIT_COMPLETE
 * indication message from the AP when it is about to begin its normal
 * operation.  The AP will only send this message after it has received
 * and responded to an INDICATION_REGISTER request from the modem.
 *
 * So in summary:
 * - Whenever the AP learns the modem has booted and its IPA QMI service
 *   is available, it sends an INIT_DRIVER request to the modem.  The
 *   modem supplies a success response when it is ready to operate.
 * - On the initial boot, the modem sets up the IPA microcontroller, and
 *   sends a DRIVER_INIT_COMPLETE request to the AP when this is done.
 * - When the modem is ready to receive an INIT_COMPLETE indication from
 *   the AP, it sends an INDICATION_REGISTER request to the AP.
 * - On the initial modem boot, everything is ready when:
 *	- AP has received a success response from its INIT_DRIVER request
 *	- AP has responded to a DRIVER_INIT_COMPLETE request
 *	- AP has responded to an INDICATION_REGISTER request from the modem
 *	- AP has sent an INIT_COMPLETE indication to the modem
 * - On subsequent modem boots, everything is ready when:
 *	- AP has received a success response from its INIT_DRIVER request
 *	- AP has responded to a DRIVER_INIT_COMPLETE request
 * - The INDICATION_REGISTER request and INIT_COMPLETE indication are
 *   optional for non-initial modem boots, and have no bearing on the
 *   determination of when things are "ready"
 */

#define IPA_HOST_SERVICE_SVC_ID		0x31
#define IPA_HOST_SVC_VERS		1
#define IPA_HOST_SERVICE_INS_ID		1

#define IPA_MODEM_SERVICE_SVC_ID	0x31
#define IPA_MODEM_SERVICE_INS_ID	2
#define IPA_MODEM_SVC_VERS		1

#define QMI_INIT_DRIVER_TIMEOUT		60000	/* A minute in milliseconds */

/* Send an INIT_COMPLETE indication message to the modem */
static void ipa_server_init_complete(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa = container_of(ipa_qmi, struct ipa, qmi);
	struct qmi_handle *qmi = &ipa_qmi->server_handle;
	/* zorn (SM8650): this used to be &ipa_qmi->modem_sq, the modem's QMI
	 * *server* -- the peer our client sends INIT_DRIVER to.  But the peer
	 * that asked for this indication is the modem's *client*, on a different
	 * port (35 rather than 31 here).  Sent to the server, the indication
	 * arrives somewhere that never registered for it and the modem answers
	 * it with FAILURE / MALFORMED_MSG, which mainline never sees because
	 * qmi_send_indication() cancels the transaction immediately.
	 */
	struct sockaddr_qrtr *sq = &ipa_qmi->indication_sq;
	struct ipa_init_complete_ind ind = { };
	int ret;

	ind.status.result = QMI_RESULT_SUCCESS_V01;
	ind.status.error = QMI_ERR_NONE_V01;

	ret = qmi_send_indication(qmi, sq, IPA_QMI_INIT_COMPLETE,
				   IPA_QMI_INIT_COMPLETE_IND_SZ,
				   ipa_init_complete_ind_ei, &ind);
	if (ret)
		dev_err(ipa->dev,
			"error %d sending init complete indication\n", ret);
	else
		ipa_qmi->indication_sent = true;
}

/* If requested (and not already sent) send the INIT_COMPLETE indication */
static void ipa_qmi_indication(struct ipa_qmi *ipa_qmi)
{
	if (!ipa_qmi->indication_requested)
		return;

	if (ipa_qmi->indication_sent)
		return;

	ipa_server_init_complete(ipa_qmi);
}

/* Determine whether everything is ready to start normal operation.
 * We know everything (else) is ready when we know the IPA driver on
 * the modem is ready, and the microcontroller is ready.
 *
 * When the modem boots (or reboots), the handshake sequence starts
 * with the AP sending the modem an INIT_DRIVER request.  Within
 * that request, the uc_loaded flag will be zero (false) for an
 * initial boot, non-zero (true) for a subsequent (SSR) boot.
 */
static void ipa_qmi_ready(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa;
	int ret;

	/* We aren't ready until the modem and microcontroller are */
	if (!ipa_qmi->modem_ready || !ipa_qmi->uc_ready)
		return;

	/* Send the indication message if it was requested */
	ipa_qmi_indication(ipa_qmi);

	/* The initial boot requires us to send the indication. */
	if (ipa_qmi->initial_boot) {
		if (!ipa_qmi->indication_sent)
			return;

		/* The initial modem boot completed successfully */
		ipa_qmi->initial_boot = false;
	}

	/* We're ready.  Start up normal operation */
	ipa = container_of(ipa_qmi, struct ipa, qmi);
	ret = ipa_modem_start(ipa);
	if (ret)
		dev_err(ipa->dev, "error %d starting modem\n", ret);
}

/* All QMI clients from the modem node are gone (modem shut down or crashed). */
static void ipa_server_bye(struct qmi_handle *qmi, unsigned int node)
{
	struct ipa_qmi *ipa_qmi;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);

	/* The modem client and server go away at the same time */
	memset(&ipa_qmi->modem_sq, 0, sizeof(ipa_qmi->modem_sq));
	memset(&ipa_qmi->indication_sq, 0, sizeof(ipa_qmi->indication_sq));

	/* initial_boot doesn't change when modem reboots */
	/* uc_ready doesn't change when modem reboots */
	ipa_qmi->modem_ready = false;
	ipa_qmi->indication_requested = false;
	ipa_qmi->indication_sent = false;
}

static void ipa_qmi_snoop(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
			  const void *buf, size_t len);

static const struct qmi_ops ipa_server_ops = {
	.bye		= ipa_server_bye,
	.msg_handler	= ipa_qmi_snoop,
};

/* Callback function to handle an INDICATION_REGISTER request message from the
 * modem.  This informs the AP that the modem is now ready to receive the
 * INIT_COMPLETE indication message.
 */
static void ipa_server_indication_register(struct qmi_handle *qmi,
					   struct sockaddr_qrtr *sq,
					   struct qmi_txn *txn,
					   const void *decoded)
{
	struct ipa_indication_register_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	/* Remember who asked, so the indication goes back to them */
	ipa_qmi->indication_sq = *sq;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INDICATION_REGISTER,
				IPA_QMI_INDICATION_REGISTER_RSP_SZ,
				ipa_indication_register_rsp_ei, &rsp);
	if (!ret) {
		ipa_qmi->indication_requested = true;
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
	} else {
		dev_err(ipa->dev,
			"error %d sending register indication response\n", ret);
	}
}

/* Respond to a DRIVER_INIT_COMPLETE request message from the modem. */
static void ipa_server_driver_init_complete(struct qmi_handle *qmi,
					    struct sockaddr_qrtr *sq,
					    struct qmi_txn *txn,
					    const void *decoded)
{
	struct ipa_driver_init_complete_rsp rsp = { };
	struct ipa_qmi *ipa_qmi;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(qmi, struct ipa_qmi, server_handle);
	ipa = container_of(ipa_qmi, struct ipa, qmi);

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_DRIVER_INIT_COMPLETE,
				IPA_QMI_DRIVER_INIT_COMPLETE_RSP_SZ,
				ipa_driver_init_complete_rsp_ei, &rsp);
	if (!ret) {
		ipa_qmi->uc_ready = true;
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
	} else {
		dev_err(ipa->dev,
			"error %d sending init complete response\n", ret);
	}
}

/* The server handles two request message types sent by the modem. */
static const struct qmi_msg_handler ipa_server_msg_handlers[] = {
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_INDICATION_REGISTER,
		.ei		= ipa_indication_register_req_ei,
		.decoded_size	= IPA_QMI_INDICATION_REGISTER_REQ_SZ,
		.fn		= ipa_server_indication_register,
	},
	{
		.type		= QMI_REQUEST,
		.msg_id		= IPA_QMI_DRIVER_INIT_COMPLETE,
		.ei		= ipa_driver_init_complete_req_ei,
		.decoded_size	= IPA_QMI_DRIVER_INIT_COMPLETE_REQ_SZ,
		.fn		= ipa_server_driver_init_complete,
	},
	{ },
};

/* Handle an INIT_DRIVER response message from the modem. */
static void ipa_client_init_driver(struct qmi_handle *qmi,
				   struct sockaddr_qrtr *sq,
				   struct qmi_txn *txn, const void *decoded)
{
	txn->result = 0;	/* IPA_QMI_INIT_DRIVER request was successful */
	complete(&txn->completion);
}

/* The client handles one response message type sent by the modem. */
static const struct qmi_msg_handler ipa_client_msg_handlers[] = {
	{
		.type		= QMI_RESPONSE,
		.msg_id		= IPA_QMI_INIT_DRIVER,
		.ei		= ipa_init_modem_driver_rsp_ei,
		.decoded_size	= IPA_QMI_INIT_DRIVER_RSP_SZ,
		.fn		= ipa_client_init_driver,
	},
	{ },
};

/* zorn (SM8650, MPSS.DE.5.0): this MPSS expects IPA QMI messages mainline does
 * not implement -- its image asserts on QMI_IPA_INSTALL_FILTER_RULE_RESP_V01,
 * QMI_IPA_FILTER_INSTALLED_NOTIF_RESP_V01 and QMI_IPA_CONFIG_RESP_V01, and
 * carries ipa_qmi_server_process_install_filter_ex_req.  qmi_invoke_handler()
 * drops a message with no matching handler *silently*, so nothing in the log
 * says which ids actually arrive.
 *
 * qmi_ops.msg_handler is handed every raw message on a handle, in place of the
 * (static) qmi_handle_message().  Log the header there, dump anything we have
 * no handler for, then redo that function's dispatch -- all of it is either
 * public in <linux/soc/qcom/qmi.h> or exported.
 */
/* A bare "everything went fine" response: just the mandatory result TLV, which
 * is the only mandatory field in the responses this modem is waiting for.  Used
 * to acknowledge requests we have no real implementation for.
 */
struct ipa_generic_rsp {
	struct qmi_response_type_v01 rsp;
};

static const struct qmi_elem_info ipa_generic_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_generic_rsp, rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_generic_rsp, rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
	},
};

#define IPA_QMI_GENERIC_RSP_SZ	7	/* 3 byte TLV header + result + error */

/* Response to INSTALL_FILTER_RULE.  When the modem passes its filter specs in
 * filter_spec_ex_list (rather than the older filter_spec_list, whose specs each
 * carry their own identifier), the identifier of a spec *is* its rule id, and
 * what it wants back is the list of rule ids that were accepted.
 */
struct ipa_install_filter_rule_rsp {
	struct qmi_response_type_v01 rsp;
	u8 rule_id_valid;
	u32 rule_id_len;
	u32 rule_id[IPA_QMI_MAX_FILTERS];
};

static const struct qmi_elem_info ipa_install_filter_rule_rsp_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	=
			sizeof_field(struct ipa_install_filter_rule_rsp, rsp),
		.tlv_type	= 0x02,
		.offset		= offsetof(struct ipa_install_filter_rule_rsp,
					   rsp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_install_filter_rule_rsp,
					   rule_id_valid),
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_install_filter_rule_rsp,
					   rule_id_len),
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= IPA_QMI_MAX_FILTERS,
		.elem_size	= sizeof(u32),
		.array_type	= VAR_LEN_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct ipa_install_filter_rule_rsp,
					   rule_id),
	},
	{
		.data_type	= QMI_EOTI,
	},
};

#define IPA_QMI_INSTALL_FILTER_RULE_RSP_SZ \
	(IPA_QMI_GENERIC_RSP_SZ + 4 + IPA_QMI_MAX_FILTERS * sizeof(u32))

/* Tell the modem where its filter rules ended up.
 *
 * The response to INSTALL_FILTER_RULE only says "accepted"; this request is how
 * the AP reports the pipe they went on, an overall status and the indices.  We
 * have no indices to report -- nothing was installed -- so the map goes out
 * empty and the rule ids are echoed instead.  The modem's reply is logged, so
 * whether it accepts that is visible rather than guessed at.
 */
static void ipa_qmi_send_filter_installed_notif(struct ipa_qmi *ipa_qmi,
						const u32 *rule_id, u32 count)
{
	struct ipa *ipa = container_of(ipa_qmi, struct ipa, qmi);
	struct qmi_handle *qmi = &ipa_qmi->client_handle;
	struct ipa_fltr_installed_notif_req *req;
	struct qmi_txn txn;
	int ret;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return;

	req->source_pipe_index =
		ipa->name_map[IPA_ENDPOINT_AP_MODEM_TX]->endpoint_id;
	req->install_status = QMI_RESULT_SUCCESS_V01;
	req->filter_index_list_len = 0;
	if (count) {
		req->rule_id_valid = 1;
		req->rule_id_len = count;
		memcpy(req->rule_id, rule_id, count * sizeof(*rule_id));
	}

	ret = qmi_txn_init(qmi, &txn, NULL, NULL);
	if (ret < 0) {
		pr_err("ipa qmi: error %d starting filter installed notif\n",
		       ret);
		kfree(req);
		return;
	}

	ret = qmi_send_request(qmi, &ipa_qmi->modem_sq, &txn,
			       IPA_QMI_FILTER_INSTALLED_NOTIF,
			       IPA_QMI_FILTER_INSTALLED_NOTIF_REQ_SZ,
			       ipa_fltr_installed_notif_req_ei, req);
	/* The reply is only interesting for its result, which the snoop logs */
	qmi_txn_cancel(&txn);

	if (ret)
		pr_err("ipa qmi: error %d sending filter installed notif\n",
		       ret);
	else
		pr_info("ipa qmi: filter installed notif sent, source pipe %u, %u rule ids\n",
			req->source_pipe_index, count);

	kfree(req);
}

/* Answer INSTALL_FILTER_RULE by echoing the rule id of every spec.
 *
 * We install nothing: these are uplink rules for the AP's own pipes, and
 * mainline already routes all AP uplink to the modem with a catch-all filter --
 * which is why uplink works today.  What the modem is blocked on is the
 * response itself.  Walking the request by hand rather than through an EI array
 * keeps us out of the business of modelling ipa_filter_rule_type_v01.
 */
static void ipa_qmi_answer_install_filter_rule(struct qmi_handle *qmi,
					       struct sockaddr_qrtr *sq,
					       struct qmi_txn *txn,
					       const void *buf, size_t len)
{
	struct ipa_install_filter_rule_rsp *rsp;
	const u8 *msg = buf;
	size_t off = sizeof(struct qmi_header);
	u32 count = 0;
	int ret;

	rsp = kzalloc(sizeof(*rsp), GFP_KERNEL);
	if (!rsp)
		return;

	while (off + 3 <= len) {
		u16 tlv_len = get_unaligned_le16(msg + off + 1);
		u8 tlv_type = msg[off];
		const u8 *val = msg + off + 3;

		if (off + 3 + tlv_len > len)
			break;

		if (tlv_type == IPA_QMI_FILTER_SPEC_EX_TLV && tlv_len) {
			u32 specs = val[0];	/* one byte of count */
			u32 i;

			if (1 + specs * IPA_QMI_FILTER_SPEC_EX_SZ != tlv_len) {
				pr_warn("ipa qmi: %u filter specs in %u bytes, not %u each -- not echoing rule ids\n",
					specs, tlv_len,
					IPA_QMI_FILTER_SPEC_EX_SZ);
				break;
			}

			for (i = 0; i < specs && i < IPA_QMI_MAX_FILTERS; i++)
				rsp->rule_id[count++] =
					get_unaligned_le32(val + 1 +
						i * IPA_QMI_FILTER_SPEC_EX_SZ +
						IPA_QMI_FILTER_SPEC_EX_RULE_ID);
		}

		off += 3 + tlv_len;
	}

	rsp->rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp->rsp.error = QMI_ERR_NONE_V01;
	if (count) {
		rsp->rule_id_valid = 1;
		rsp->rule_id_len = count;
	}

	ret = qmi_send_response(qmi, sq, txn, IPA_QMI_INSTALL_FILTER_RULE,
				IPA_QMI_INSTALL_FILTER_RULE_RSP_SZ,
				ipa_install_filter_rule_rsp_ei, rsp);
	if (ret)
		pr_err("ipa qmi: error %d answering install filter rule\n", ret);
	else
		pr_info("ipa qmi: install filter rule: echoed %u rule ids %*ph\n",
			count, (int)min_t(u32, count, 16) * 4, rsp->rule_id);

	if (!ret)
		ipa_qmi_send_filter_installed_notif(
			container_of(qmi, struct ipa_qmi, server_handle),
			rsp->rule_id, count);

	kfree(rsp);
}

/* Answer a request we do not implement with a success result.
 *
 * zorn (SM8650, MPSS.DE.5.0): this MPSS sends CONFIG (0x27) and
 * INSTALL_FILTER_RULE (0x23) during its IPA init and blocks its Data Services
 * layer until both are answered -- which is why a data call comes up, carries
 * uplink, delivers no downlink at all, and ends in
 * "nr5g_rrc_misc.c: DS wait timer expired".  We have no filter table
 * implementation to offer, but the modem writes its own rules into the table
 * regions negotiated by INIT_DRIVER, so acknowledging may be enough to let it
 * proceed.  Everything about the request is dumped first, so a real
 * implementation can be written from the captured bytes.
 */
static void ipa_qmi_ack_unhandled(struct qmi_handle *qmi,
				  struct sockaddr_qrtr *sq,
				  struct qmi_txn *txn, u16 msg_id)
{
	struct ipa_generic_rsp rsp = { };
	int ret;

	rsp.rsp.result = QMI_RESULT_SUCCESS_V01;
	rsp.rsp.error = QMI_ERR_NONE_V01;

	ret = qmi_send_response(qmi, sq, txn, msg_id, IPA_QMI_GENERIC_RSP_SZ,
				ipa_generic_rsp_ei, &rsp);
	if (ret)
		pr_err("ipa qmi: error %d acking msg_id 0x%04x\n", ret, msg_id);
	else
		pr_info("ipa qmi: acked msg_id 0x%04x with success\n", msg_id);
}

static void ipa_qmi_dispatch(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
			     struct qmi_txn *txn, const void *buf, size_t len)
{
	const struct qmi_msg_handler *handler;
	const struct qmi_header *hdr = buf;
	void *dest;
	int ret;

	if (!qmi->handlers)
		return;

	for (handler = qmi->handlers; handler->fn; handler++)
		if (handler->type == hdr->type &&
		    handler->msg_id == le16_to_cpu(hdr->msg_id))
			break;

	if (!handler->fn) {
		u16 msg_id = le16_to_cpu(hdr->msg_id);

		pr_warn("ipa qmi: UNHANDLED type %u msg_id 0x%04x, %zu bytes\n",
			hdr->type, msg_id, len);
		if (hdr->type != QMI_REQUEST)
			return;
		if (msg_id == IPA_QMI_INSTALL_FILTER_RULE)
			ipa_qmi_answer_install_filter_rule(qmi, sq, txn, buf,
							   len);
		else
			ipa_qmi_ack_unhandled(qmi, sq, txn, msg_id);
		return;
	}

	dest = kzalloc(handler->decoded_size, GFP_KERNEL);
	if (!dest)
		return;

	ret = qmi_decode_message(buf, len, handler->ei, dest);
	if (ret < 0)
		pr_err("ipa qmi: cannot decode type %u msg_id 0x%04x\n",
		       hdr->type, le16_to_cpu(hdr->msg_id));
	else
		handler->fn(qmi, sq, txn, dest);

	kfree(dest);
}

static void ipa_qmi_snoop(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
			  const void *buf, size_t len)
{
	const struct qmi_header *hdr = buf;
	const u8 *msg = buf;
	struct qmi_txn tmp_txn;
	struct qmi_txn *txn;
	int ret;

	if (len < sizeof(*hdr)) {
		pr_err("ipa qmi: short packet, %zu bytes\n", len);
		return;
	}

	pr_info("ipa qmi: rx %s %u:%u type %u txn %u msg_id 0x%04x msg_len %u (%zu bytes)\n",
		qmi->handlers == ipa_server_msg_handlers ? "srv" : "cli",
		sq->sq_node, sq->sq_port, hdr->type, le16_to_cpu(hdr->txn_id),
		le16_to_cpu(hdr->msg_id), le16_to_cpu(hdr->msg_len), len);
	print_hex_dump(KERN_INFO, "ipa qmi: ", DUMP_PREFIX_OFFSET, 16, 1, buf,
		       min_t(size_t, len, 2048), false);

	if (hdr->type != QMI_RESPONSE) {
		memset(&tmp_txn, 0, sizeof(tmp_txn));
		tmp_txn.id = le16_to_cpu(hdr->txn_id);
		ipa_qmi_dispatch(qmi, sq, &tmp_txn, buf, len);
		return;
	}

	mutex_lock(&qmi->txn_lock);
	txn = idr_find(&qmi->txns, le16_to_cpu(hdr->txn_id));
	if (!txn) {
		mutex_unlock(&qmi->txn_lock);
		/* An indication we sent gets its transaction cancelled the
		 * moment it is on the wire, so a reply to one lands here.  The
		 * modem only replies at all when it could not decode what we
		 * sent, and the reply says why -- worth reading rather than
		 * dropping on the floor.
		 */
		if (len >= sizeof(*hdr) + 7 && msg[sizeof(*hdr)] == 0x02)
			pr_warn("ipa qmi: msg_id 0x%04x came back with result %u error %u\n",
				le16_to_cpu(hdr->msg_id),
				get_unaligned_le16(msg + sizeof(*hdr) + 3),
				get_unaligned_le16(msg + sizeof(*hdr) + 5));
		else
			pr_warn("ipa qmi: response to unknown txn %u, msg_id 0x%04x\n",
				le16_to_cpu(hdr->txn_id),
				le16_to_cpu(hdr->msg_id));
		return;
	}

	mutex_lock(&txn->lock);
	mutex_unlock(&qmi->txn_lock);

	if (txn->dest && txn->ei) {
		ret = qmi_decode_message(buf, len, txn->ei, txn->dest);
		if (ret < 0)
			pr_err("ipa qmi: cannot decode response\n");
		txn->result = ret;
		complete(&txn->completion);
	} else {
		ipa_qmi_dispatch(qmi, sq, txn, buf, len);
	}

	mutex_unlock(&txn->lock);
}

/* Return a pointer to an init modem driver request structure, which contains
 * configuration parameters for the modem.  The modem may be started multiple
 * times, but generally these parameters don't change so we can reuse the
 * request structure once it's initialized.  The only exception is the
 * skip_uc_load field, which will be set only after the microcontroller has
 * reported it has completed its initialization.
 */
static const struct ipa_init_modem_driver_req *
init_modem_driver_req(struct ipa_qmi *ipa_qmi)
{
	struct ipa *ipa = container_of(ipa_qmi, struct ipa, qmi);
	u32 modem_route_count = ipa->modem_route_count;
	static struct ipa_init_modem_driver_req req;
	const struct ipa_mem *mem;

	/* The microcontroller is initialized on the first boot */
	req.skip_uc_load_valid = 1;
	req.skip_uc_load = ipa->uc_loaded ? 1 : 0;

	/* We only have to initialize most of it once */
	if (req.platform_type_valid)
		return &req;

	req.platform_type_valid = 1;
	req.platform_type = IPA_QMI_PLATFORM_TYPE_MSM_ANDROID;

	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_HEADER);
	if (mem->size) {
		req.hdr_tbl_info_valid = 1;
		req.hdr_tbl_info.start = ipa->mem_offset + mem->offset;
		req.hdr_tbl_info.end = req.hdr_tbl_info.start + mem->size - 1;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V4_ROUTE);
	req.v4_route_tbl_info_valid = 1;
	req.v4_route_tbl_info.start = ipa->mem_offset + mem->offset;
	req.v4_route_tbl_info.end = modem_route_count - 1;

	mem = ipa_mem_find(ipa, IPA_MEM_V6_ROUTE);
	req.v6_route_tbl_info_valid = 1;
	req.v6_route_tbl_info.start = ipa->mem_offset + mem->offset;
	req.v6_route_tbl_info.end = modem_route_count - 1;

	mem = ipa_mem_find(ipa, IPA_MEM_V4_FILTER);
	req.v4_filter_tbl_start_valid = 1;
	req.v4_filter_tbl_start = ipa->mem_offset + mem->offset;

	mem = ipa_mem_find(ipa, IPA_MEM_V6_FILTER);
	req.v6_filter_tbl_start_valid = 1;
	req.v6_filter_tbl_start = ipa->mem_offset + mem->offset;

	mem = ipa_mem_find(ipa, IPA_MEM_MODEM);
	if (mem->size) {
		req.modem_mem_info_valid = 1;
		req.modem_mem_info.start = ipa->mem_offset + mem->offset;
		req.modem_mem_info.size = mem->size;
	}

	req.ctrl_comm_dest_end_pt_valid = 1;
	req.ctrl_comm_dest_end_pt =
		ipa->name_map[IPA_ENDPOINT_AP_MODEM_RX]->endpoint_id;

	/* skip_uc_load_valid and skip_uc_load are set above */

	mem = ipa_mem_find(ipa, IPA_MEM_MODEM_PROC_CTX);
	if (mem->size) {
		req.hdr_proc_ctx_tbl_info_valid = 1;
		req.hdr_proc_ctx_tbl_info.start =
			ipa->mem_offset + mem->offset;
		req.hdr_proc_ctx_tbl_info.end =
			req.hdr_proc_ctx_tbl_info.start + mem->size - 1;
	}

	/* Nothing to report for the compression table (zip_tbl_info) */

	mem = ipa_mem_find(ipa, IPA_MEM_V4_ROUTE_HASHED);
	if (mem->size) {
		req.v4_hash_route_tbl_info_valid = 1;
		req.v4_hash_route_tbl_info.start =
				ipa->mem_offset + mem->offset;
		req.v4_hash_route_tbl_info.end = modem_route_count - 1;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V6_ROUTE_HASHED);
	if (mem->size) {
		req.v6_hash_route_tbl_info_valid = 1;
		req.v6_hash_route_tbl_info.start =
			ipa->mem_offset + mem->offset;
		req.v6_hash_route_tbl_info.end = modem_route_count - 1;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V4_FILTER_HASHED);
	if (mem->size) {
		req.v4_hash_filter_tbl_start_valid = 1;
		req.v4_hash_filter_tbl_start = ipa->mem_offset + mem->offset;
	}

	mem = ipa_mem_find(ipa, IPA_MEM_V6_FILTER_HASHED);
	if (mem->size) {
		req.v6_hash_filter_tbl_start_valid = 1;
		req.v6_hash_filter_tbl_start = ipa->mem_offset + mem->offset;
	}

	/* The stats fields are only valid for IPA v4.0+ */
	if (ipa->version >= IPA_VERSION_4_0) {
		mem = ipa_mem_find(ipa, IPA_MEM_STATS_QUOTA_MODEM);
		if (mem->size) {
			req.hw_stats_quota_base_addr_valid = 1;
			req.hw_stats_quota_base_addr =
				ipa->mem_offset + mem->offset;
			req.hw_stats_quota_size_valid = 1;
			req.hw_stats_quota_size = ipa->mem_offset + mem->size;
		}

		/* If the DROP stats region is defined, include it */
		mem = ipa_mem_find(ipa, IPA_MEM_STATS_DROP);
		if (mem && mem->size) {
			req.hw_stats_drop_base_addr_valid = 1;
			req.hw_stats_drop_base_addr =
				ipa->mem_offset + mem->offset;
			req.hw_stats_drop_size_valid = 1;
			req.hw_stats_drop_size = ipa->mem_offset + mem->size;
		}
	}

	return &req;
}

/* Send an INIT_DRIVER request to the modem, and wait for it to complete. */
static void ipa_client_init_driver_work(struct work_struct *work)
{
	unsigned long timeout = msecs_to_jiffies(QMI_INIT_DRIVER_TIMEOUT);
	const struct ipa_init_modem_driver_req *req;
	struct ipa_qmi *ipa_qmi;
	struct qmi_handle *qmi;
	struct qmi_txn txn;
	struct device *dev;
	struct ipa *ipa;
	int ret;

	ipa_qmi = container_of(work, struct ipa_qmi, init_driver_work);
	qmi = &ipa_qmi->client_handle;

	ipa = container_of(ipa_qmi, struct ipa, qmi);
	dev = ipa->dev;

	ret = qmi_txn_init(qmi, &txn, NULL, NULL);
	if (ret < 0) {
		dev_err(dev, "error %d preparing init driver request\n", ret);
		return;
	}

	/* Send the request, and if successful wait for its response */
	req = init_modem_driver_req(ipa_qmi);
	ret = qmi_send_request(qmi, &ipa_qmi->modem_sq, &txn,
			       IPA_QMI_INIT_DRIVER, IPA_QMI_INIT_DRIVER_REQ_SZ,
			       ipa_init_modem_driver_req_ei, req);
	if (ret)
		dev_err(dev, "error %d sending init driver request\n", ret);
	else if ((ret = qmi_txn_wait(&txn, timeout)))
		dev_err(dev, "error %d awaiting init driver response\n", ret);

	if (!ret) {
		ipa_qmi->modem_ready = true;
		ipa_qmi_ready(ipa_qmi);		/* We might be ready now */
	} else {
		/* If any error occurs we need to cancel the transaction */
		qmi_txn_cancel(&txn);
	}
}

/* The modem server is now available.  We will send an INIT_DRIVER request
 * to the modem, but can't wait for it to complete in this callback thread.
 * Schedule a worker on the global workqueue to do that for us.
 */
static int
ipa_client_new_server(struct qmi_handle *qmi, struct qmi_service *svc)
{
	struct ipa_qmi *ipa_qmi;

	ipa_qmi = container_of(qmi, struct ipa_qmi, client_handle);

	ipa_qmi->modem_sq.sq_family = AF_QIPCRTR;
	ipa_qmi->modem_sq.sq_node = svc->node;
	ipa_qmi->modem_sq.sq_port = svc->port;

	schedule_work(&ipa_qmi->init_driver_work);

	return 0;
}

static const struct qmi_ops ipa_client_ops = {
	.new_server	= ipa_client_new_server,
	.msg_handler	= ipa_qmi_snoop,
};

/* Set up for QMI message exchange */
int ipa_qmi_setup(struct ipa *ipa)
{
	struct ipa_qmi *ipa_qmi = &ipa->qmi;
	int ret;

	ipa_qmi->initial_boot = true;

	/* The server handle is used to handle the DRIVER_INIT_COMPLETE
	 * request on the first modem boot.  It also receives the
	 * INDICATION_REGISTER request on the first boot and (optionally)
	 * subsequent boots.  The INIT_COMPLETE indication message is
	 * sent over the server handle if requested.
	 */
	ret = qmi_handle_init(&ipa_qmi->server_handle,
			      IPA_QMI_SERVER_MAX_RCV_SZ, &ipa_server_ops,
			      ipa_server_msg_handlers);
	if (ret)
		return ret;

	ret = qmi_add_server(&ipa_qmi->server_handle, IPA_HOST_SERVICE_SVC_ID,
			     IPA_HOST_SVC_VERS, IPA_HOST_SERVICE_INS_ID);
	if (ret)
		goto err_server_handle_release;

	/* The client handle is only used for sending an INIT_DRIVER request
	 * to the modem, and receiving its response message.
	 */
	ret = qmi_handle_init(&ipa_qmi->client_handle,
			      IPA_QMI_CLIENT_MAX_RCV_SZ, &ipa_client_ops,
			      ipa_client_msg_handlers);
	if (ret)
		goto err_server_handle_release;

	/* We need this ready before the service lookup is added */
	INIT_WORK(&ipa_qmi->init_driver_work, ipa_client_init_driver_work);

	ret = qmi_add_lookup(&ipa_qmi->client_handle, IPA_MODEM_SERVICE_SVC_ID,
			     IPA_MODEM_SVC_VERS, IPA_MODEM_SERVICE_INS_ID);
	if (ret)
		goto err_client_handle_release;

	return 0;

err_client_handle_release:
	/* Releasing the handle also removes registered lookups */
	qmi_handle_release(&ipa_qmi->client_handle);
	memset(&ipa_qmi->client_handle, 0, sizeof(ipa_qmi->client_handle));
err_server_handle_release:
	/* Releasing the handle also removes registered services */
	qmi_handle_release(&ipa_qmi->server_handle);
	memset(&ipa_qmi->server_handle, 0, sizeof(ipa_qmi->server_handle));

	return ret;
}

/* Tear down IPA QMI handles */
void ipa_qmi_teardown(struct ipa *ipa)
{
	cancel_work_sync(&ipa->qmi.init_driver_work);

	qmi_handle_release(&ipa->qmi.client_handle);
	memset(&ipa->qmi.client_handle, 0, sizeof(ipa->qmi.client_handle));

	qmi_handle_release(&ipa->qmi.server_handle);
	memset(&ipa->qmi.server_handle, 0, sizeof(ipa->qmi.server_handle));
}
