// SPDX-License-Identifier: GPL-2.0

/* Copyright (C) 2023-2024 Linaro Ltd. */

#include <linux/array_size.h>
#include <linux/log2.h>

#include "../ipa_data.h"
#include "../ipa_endpoint.h"
#include "../ipa_mem.h"
#include "../ipa_version.h"

/** enum ipa_resource_type - IPA resource types for an SoC having IPA v5.5 */
enum ipa_resource_type {
	/* Source resource types; first must have value 0 */
	IPA_RESOURCE_TYPE_SRC_PKT_CONTEXTS		= 0,
	IPA_RESOURCE_TYPE_SRC_DESCRIPTOR_LISTS,
	IPA_RESOURCE_TYPE_SRC_DESCRIPTOR_BUFF,
	IPA_RESOURCE_TYPE_SRC_HPS_DMARS,
	IPA_RESOURCE_TYPE_SRC_ACK_ENTRIES,

	/* Destination resource types; first must have value 0 */
	IPA_RESOURCE_TYPE_DST_DATA_SECTORS		= 0,
	IPA_RESOURCE_TYPE_DST_DPS_DMARS,
	IPA_RESOURCE_TYPE_DST_ULSO_SEGMENTS,
};

/* Resource groups used for an SoC having IPA v5.5 */
enum ipa_rsrc_group_id {
	/* Source resource group identifiers */
	IPA_RSRC_GROUP_SRC_UL				= 0,
	IPA_RSRC_GROUP_SRC_DL,
	IPA_RSRC_GROUP_SRC_UNUSED_2,
	IPA_RSRC_GROUP_SRC_UNUSED_3,
	IPA_RSRC_GROUP_SRC_URLLC,
	IPA_RSRC_GROUP_SRC_U_RX_QC,
	IPA_RSRC_GROUP_SRC_COUNT,	/* Last in set; not a source group */

	/* Destination resource group identifiers */
	IPA_RSRC_GROUP_DST_UL				= 0,
	IPA_RSRC_GROUP_DST_DL,
	IPA_RSRC_GROUP_DST_UNUSED_2,
	IPA_RSRC_GROUP_DST_UNUSED_3,
	IPA_RSRC_GROUP_DST_UNUSED_4,
	IPA_RSRC_GROUP_DST_UC,
	IPA_RSRC_GROUP_DST_DRB_IP,
	IPA_RSRC_GROUP_DST_COUNT,	/* Last; not a destination group */
};

/* QSB configuration data for an SoC having IPA v5.5 */
static const struct ipa_qsb_data ipa_qsb_data[] = {
	[IPA_QSB_MASTER_DDR] = {
		.max_writes		= 0,	/* Unlimited */
		.max_reads		= 12,
		.max_reads_beats	= 0,
	},
	[IPA_QSB_MASTER_PCIE] = {
		.max_writes		= 0,	/* Unlimited */
		.max_reads		= 8,
		.max_reads_beats	= 0,
	},
};

/* Endpoint configuration data for an SoC having IPA v5.5 */
static const struct ipa_gsi_endpoint_data ipa_gsi_endpoint_data[] = {
	[IPA_ENDPOINT_AP_COMMAND_TX] = {
		.ee_id		= GSI_EE_AP,
		.channel_id	= 12,
		.endpoint_id	= 14,
		.toward_ipa	= true,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 20,
		},
		.endpoint = {
			.config = {
				.resource_group	= IPA_RSRC_GROUP_SRC_UL,
				.dma_mode	= true,
				.dma_endpoint	= IPA_ENDPOINT_AP_LAN_RX,
				.tx = {
					.seq_type = IPA_SEQ_DMA,
				},
			},
		},
	},
	[IPA_ENDPOINT_AP_LAN_RX] = {
		.ee_id		= GSI_EE_AP,
		.channel_id	= 13,
		.endpoint_id	= 16,
		.toward_ipa	= false,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 9,
		},
		.endpoint = {
			.config = {
				.resource_group	= IPA_RSRC_GROUP_DST_UL,
				.aggregation	= true,
				.status_enable	= true,
				.rx = {
					.buffer_size	= 8192,
					.pad_align	= ilog2(sizeof(u32)),
					.aggr_time_limit = 500,
				},
			},
		},
	},
	[IPA_ENDPOINT_AP_MODEM_TX] = {
		.ee_id		= GSI_EE_AP,
		.channel_id	= 11,
		.endpoint_id	= 2,
		.toward_ipa	= true,
		.channel = {
			.tre_count	= 512,
			.event_count	= 512,
			.tlv_count	= 25,
		},
		.endpoint = {
			.filter_support	= true,
			.config = {
				.resource_group	= IPA_RSRC_GROUP_SRC_UL,
				.checksum       = true,
				.qmap		= true,
				.status_enable	= true,
				.tx = {
					.seq_type = IPA_SEQ_2_PASS_SKIP_LAST_UC,
					.status_endpoint =
						IPA_ENDPOINT_MODEM_AP_RX,
				},
			},
		},
	},
	[IPA_ENDPOINT_AP_MODEM_RX] = {
		.ee_id		= GSI_EE_AP,
		.channel_id	= 1,
		/* zorn (SM8650): this was 23, which is not the pipe GSI channel
		 * 1 belongs to.  Qualcomm's own table for IPA v5.5 pairs
		 * APPS_WAN_CONS with (pipe 24, channel 1) and
		 * APPS_WAN_COAL_CONS -- the coalescing consumer -- with
		 * (pipe 23, channel 4); every other endpoint here matches a
		 * real (pipe, channel) tuple, these two consumers did not.
		 *
		 * The endpoint id is what indexes the pipe's configuration
		 * registers, and it is what the modem is told to address: the
		 * ctrl_comm_dest_end_pt we send in INIT_DRIVER comes straight
		 * from here.  So the AP was configuring pipe 23 while driving
		 * pipe 24's channel, and the modem was sending downlink to a
		 * pipe the AP never armed a channel for -- which is why not one
		 * frame ever arrived and GSI raised no receive events at all,
		 * while uplink, whose (pipe 2, channel 11) is right, worked.
		 */
		.endpoint_id	= 24,
		.toward_ipa	= false,
		.channel = {
			.tre_count	= 256,
			.event_count	= 256,
			.tlv_count	= 9,
		},
		.endpoint = {
			.config = {
				.resource_group	= IPA_RSRC_GROUP_DST_DL,
				.checksum       = true,
				.qmap		= true,
				.aggregation	= true,
				.rx = {
					.buffer_size	= 8192,
					.aggr_time_limit = 500,
					.aggr_close_eof	= true,
				},
			},
		},
	},
	[IPA_ENDPOINT_MODEM_AP_TX] = {
		.ee_id		= GSI_EE_MODEM,
		.channel_id	= 0,
		.endpoint_id	= 12,
		.toward_ipa	= true,
		.endpoint = {
			.filter_support	= true,
		},
	},
	[IPA_ENDPOINT_MODEM_AP_RX] = {
		.ee_id		= GSI_EE_MODEM,
		.channel_id	= 7,
		/* Same mismatch as AP_MODEM_RX above: Q6_WAN_CONS is
		 * (pipe 22, channel 7), while pipe 21 is Q6_UL_NLO_ACK_CONS on
		 * channel 6.  This endpoint is what AP_MODEM_TX names as its
		 * status endpoint, so getting it wrong points IPA's transmit
		 * status reports at the wrong pipe.
		 */
		.endpoint_id	= 22,
		.toward_ipa	= false,
	},
	[IPA_ENDPOINT_MODEM_DL_NLO_TX] = {
		.ee_id		= GSI_EE_MODEM,
		.channel_id	= 2,
		.endpoint_id	= 15,
		.toward_ipa	= true,
		.endpoint = {
			.filter_support	= true,
		},
	},
};

/* Source resource configuration data for an SoC having IPA v5.5 */
static const struct ipa_resource ipa_resource_src[] = {
	[IPA_RESOURCE_TYPE_SRC_PKT_CONTEXTS] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 3,	.max = 9,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 4,	.max = 10,
		},
		.limits[IPA_RSRC_GROUP_SRC_URLLC] = {
			.min = 1,	.max = 63,
		},
		.limits[IPA_RSRC_GROUP_SRC_U_RX_QC] = {
			.min = 0,	.max = 63,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_DESCRIPTOR_LISTS] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 9,	.max = 9,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 12,	.max = 12,
		},
		.limits[IPA_RSRC_GROUP_SRC_URLLC] = {
			.min = 10,	.max = 10,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_DESCRIPTOR_BUFF] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 9,	.max = 9,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 24,	.max = 24,
		},
		.limits[IPA_RSRC_GROUP_SRC_URLLC] = {
			.min = 20,	.max = 20,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_HPS_DMARS] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 0,	.max = 63,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 0,	.max = 63,
		},
		.limits[IPA_RSRC_GROUP_SRC_URLLC] = {
			.min = 1,	.max = 63,
		},
		.limits[IPA_RSRC_GROUP_SRC_U_RX_QC] = {
			.min = 0,	.max = 63,
		},
	},
	[IPA_RESOURCE_TYPE_SRC_ACK_ENTRIES] = {
		.limits[IPA_RSRC_GROUP_SRC_UL] = {
			.min = 22,	.max = 22,
		},
		.limits[IPA_RSRC_GROUP_SRC_DL] = {
			.min = 16,	.max = 16,
		},
		.limits[IPA_RSRC_GROUP_SRC_URLLC] = {
			.min = 16,	.max = 16,
		},
	},
};

/* Destination resource configuration data for an SoC having IPA v5.5 */
static const struct ipa_resource ipa_resource_dst[] = {
	[IPA_RESOURCE_TYPE_DST_DATA_SECTORS] = {
		.limits[IPA_RSRC_GROUP_DST_UL] = {
			.min = 6,	.max = 6,
		},
		.limits[IPA_RSRC_GROUP_DST_DL] = {
			.min = 5,	.max = 5,
		},
		.limits[IPA_RSRC_GROUP_DST_DRB_IP] = {
			.min = 39,	.max = 39,
		},
	},
	[IPA_RESOURCE_TYPE_DST_DPS_DMARS] = {
		.limits[IPA_RSRC_GROUP_DST_UL] = {
			.min = 0,	.max = 3,
		},
		.limits[IPA_RSRC_GROUP_DST_DL] = {
			.min = 0,	.max = 3,
		},
	},
	[IPA_RESOURCE_TYPE_DST_ULSO_SEGMENTS] = {
		.limits[IPA_RSRC_GROUP_DST_UL] = {
			.min = 0,	.max = 63,
		},
		.limits[IPA_RSRC_GROUP_DST_DL] = {
			.min = 0,	.max = 63,
		},
	},
};

/* Resource configuration data for an SoC having IPA v5.5 */
static const struct ipa_resource_data ipa_resource_data = {
	.rsrc_group_dst_count	= IPA_RSRC_GROUP_DST_COUNT,
	.rsrc_group_src_count	= IPA_RSRC_GROUP_SRC_COUNT,
	.resource_src_count	= ARRAY_SIZE(ipa_resource_src),
	.resource_src		= ipa_resource_src,
	.resource_dst_count	= ARRAY_SIZE(ipa_resource_dst),
	.resource_dst		= ipa_resource_dst,
};

/* IPA-resident memory region data for an SoC having IPA v5.5 */
static const struct ipa_mem ipa_mem_local_data[] = {
	{
		.id		= IPA_MEM_UC_EVENT_RING,
		.offset		= 0x0000,
		.size		= 0x1000,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_UC_SHARED,
		.offset		= 0x1000,
		.size		= 0x0080,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_UC_INFO,
		.offset		= 0x1080,
		.size		= 0x0200,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_V4_FILTER_HASHED,
		.offset		= 0x1288,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V4_FILTER,
		.offset		= 0x1308,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_FILTER_HASHED,
		.offset		= 0x1388,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_FILTER,
		.offset		= 0x1408,
		.size		= 0x0078,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V4_ROUTE_HASHED,
		.offset		= 0x1488,
		.size		= 0x0098,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V4_ROUTE,
		.offset		= 0x1528,
		.size		= 0x0098,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_ROUTE_HASHED,
		.offset		= 0x15c8,
		.size		= 0x0098,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_V6_ROUTE,
		.offset		= 0x1668,
		.size		= 0x0098,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_MODEM_HEADER,
		.offset		= 0x1708,
		.size		= 0x0240,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_AP_HEADER,
		.offset		= 0x1948,
		.size		= 0x01e0,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_MODEM_PROC_CTX,
		.offset		= 0x1b40,
		.size		= 0x0b20,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_AP_PROC_CTX,
		.offset		= 0x2660,
		.size		= 0x0200,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_STATS_QUOTA_MODEM,
		.offset		= 0x2868,
		.size		= 0x0060,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_STATS_QUOTA_AP,
		.offset		= 0x28c8,
		.size		= 0x0048,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_STATS_TETHERING,
		.offset		= 0x2910,
		.size		= 0x03c0,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_AP_V4_FILTER,
		.offset		= 0x29b8,
		.size		= 0x0188,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_AP_V6_FILTER,
		.offset		= 0x2b40,
		.size		= 0x0228,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_STATS_FILTER_ROUTE,
		.offset		= 0x2cd0,
		.size		= 0x0ba0,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_STATS_DROP,
		.offset		= 0x3870,
		.size		= 0x0020,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_MODEM,
		.offset		= 0x3898,
		.size		= 0x0d48,
		.canary_count	= 2,
	},
	{
		.id		= IPA_MEM_NAT_TABLE,
		.offset		= 0x45e0,
		.size		= 0x0900,
		.canary_count	= 0,
	},
	{
		.id		= IPA_MEM_PDN_CONFIG,
		.offset		= 0x4ee8,
		.size		= 0x0100,
		.canary_count	= 2,
	},
};

/* Memory configuration data for an SoC having IPA v5.5 */
static const struct ipa_mem_data ipa_mem_data = {
	.local_count	= ARRAY_SIZE(ipa_mem_local_data),
	.local		= ipa_mem_local_data,
	/*
	 * zorn (SM8650): the modem programs IPA to touch IMEM at 0x14684080,
	 * which the upstream 0x14688000+0x2000 window does not cover, so the
	 * transfer takes a stage-1 translation fault on the IPA SID:
	 *   arm-smmu 15000000.iommu: Unhandled context fault: fsr=0x402
	 *     [Format=2 TF], iova=0x14684080, SID=0x4a0
	 * IPA then raises its fatal IRQ and the modem asserts in
	 * ipa_hal.c:3109 the instant DMS operating mode goes online.
	 * Downstream identity-maps 0x14683000+0x2000 into the same context
	 * bank (ipa_smmu_ap's qcom,additional-mapping), so widen the window to
	 * the union of both rather than moving it.
	 */
	.imem_addr	= 0x14683000,
	.imem_size	= 0x00007000,
	.smem_size	= 0x0000b000,
};

/* Interconnect rates are in 1000 byte/second units */
static const struct ipa_interconnect_data ipa_interconnect_data[] = {
	{
		.name			= "memory",
		.peak_bandwidth		= 1900000,	/* 1.9 GBps */
		.average_bandwidth	= 600000,	/* 600 MBps */
	},
	/* Average rate is unused for the next interconnect */
	{
		.name			= "config",
		.peak_bandwidth		= 76800,	/* 76.8 MBps */
		.average_bandwidth	= 0,		/* unused */
	},
};

/* Clock and interconnect configuration data for an SoC having IPA v5.5 */
static const struct ipa_power_data ipa_power_data = {
	.core_clock_rate	= 120 * 1000 * 1000,	/* Hz */
	.interconnect_count	= ARRAY_SIZE(ipa_interconnect_data),
	.interconnect_data	= ipa_interconnect_data,
};

/* Configuration data for an SoC having IPA v5.5. */
const struct ipa_data ipa_data_v5_5 = {
	.version		= IPA_VERSION_5_5,
	.qsb_count		= ARRAY_SIZE(ipa_qsb_data),
	.qsb_data		= ipa_qsb_data,
	.modem_route_count	= 11,
	.endpoint_count		= ARRAY_SIZE(ipa_gsi_endpoint_data),
	.endpoint_data		= ipa_gsi_endpoint_data,
	.resource_data		= &ipa_resource_data,
	.mem_data		= &ipa_mem_data,
	.power_data		= &ipa_power_data,
};
