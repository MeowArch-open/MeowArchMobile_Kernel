// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022, Linaro Ltd
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/auxiliary_bus.h>
#include <linux/devm-helpers.h>
#include <linux/ctype.h>
#include <linux/hex.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/math.h>
#include <linux/units.h>

#define BATTMGR_CHEMISTRY_LEN	4
#define BATTMGR_STRING_LEN	128

enum qcom_battmgr_variant {
	QCOM_BATTMGR_SC8280XP,
	QCOM_BATTMGR_SM8350,
	QCOM_BATTMGR_SM8550,
	QCOM_BATTMGR_SM8650,
	QCOM_BATTMGR_X1E80100,
};

#define BATTMGR_BAT_STATUS		0x1

#define BATTMGR_REQUEST_NOTIFICATION	0x4

#define BATTMGR_NOTIFICATION		0x7
#define NOTIF_BAT_PROPERTY		0x30
#define NOTIF_USB_PROPERTY		0x32
#define NOTIF_WLS_PROPERTY		0x34
#define NOTIF_XM_PROPERTY		0x50
#define NOTIF_BAT_STATUS		0x80
#define NOTIF_BAT_INFO			0x81
#define NOTIF_BAT_CHARGING_STATE	0x83

#define BATTMGR_BAT_INFO		0x9

#define BATTMGR_BAT_DISCHARGE_TIME	0xc

#define BATTMGR_BAT_CHARGE_TIME		0xd

#define BATTMGR_BAT_PROPERTY_GET	0x30
#define BATTMGR_BAT_PROPERTY_SET	0x31
#define BATT_STATUS			0
#define BATT_HEALTH			1
#define BATT_PRESENT			2
#define BATT_CHG_TYPE			3
#define BATT_CAPACITY			4
#define BATT_SOH			5
#define BATT_VOLT_OCV			6
#define BATT_VOLT_NOW			7
#define BATT_VOLT_MAX			8
#define BATT_CURR_NOW			9
#define BATT_CHG_CTRL_LIM		10
#define BATT_CHG_CTRL_LIM_MAX		11
#define BATT_TEMP			12
#define BATT_TECHNOLOGY			13
#define BATT_CHG_COUNTER		14
#define BATT_CYCLE_COUNT		15
#define BATT_CHG_FULL_DESIGN		16
#define BATT_CHG_FULL			17
#define BATT_MODEL_NAME			18
#define BATT_TTF_AVG			19
#define BATT_TTE_AVG			20
#define BATT_RESISTANCE			21
#define BATT_POWER_NOW			22
#define BATT_POWER_AVG			23
#define BATT_CHG_CTRL_EN		24
#define BATT_CHG_CTRL_START_THR		25
#define BATT_CHG_CTRL_END_THR		26

/* Xiaomi SM8650 inserts CONSTANT_CURRENT at 12, shifting later IDs. */
#define SM8650_BATT_TEMP		13
#define SM8650_BATT_TECHNOLOGY		14
#define SM8650_BATT_CYCLE_COUNT	16
#define SM8650_BATT_CHG_FULL_DESIGN	17
#define SM8650_BATT_CHG_FULL		18
#define SM8650_BATT_MODEL_NAME		19

/* Match the stock zorn driver's low-SOC sanity threshold. */
#define SM8650_SHUTDOWN_VOLTAGE_UV	2900000
#define SM8650_SOC_ZERO_MARGIN_UV	50000

#define BATTMGR_USB_PROPERTY_GET	0x32
#define BATTMGR_USB_PROPERTY_SET	0x33
#define USB_ONLINE			0
#define USB_VOLT_NOW			1
#define USB_VOLT_MAX			2
#define USB_CURR_NOW			3
#define USB_CURR_MAX			4
#define USB_INPUT_CURR_LIMIT		5
#define USB_TYPE			6
#define USB_ADAP_TYPE			7
#define USB_MOISTURE_DET_EN		8
#define USB_MOISTURE_DET_STS		9
#define USB_TEMP			10
#define USB_REAL_TYPE			11
#define USB_TYPEC_COMPLIANT		12

#define QTI_USB_TYPE_HVDCP		0x80
#define QTI_USB_TYPE_HVDCP_3		0x81
#define QTI_USB_TYPE_HVDCP_3P5		0x82
#define QTI_USB_TYPE_USB_FLOAT		0x83
#define QTI_USB_TYPE_HVDCP_3_CLASSB	0x84

#define BATTMGR_XM_PROPERTY_GET		0x50
#define BATTMGR_XM_PROPERTY_SET		0x51
#define XM_PROP_CONNECTOR_TEMP		2
#define XM_PROP_AUTHENTIC		4
#define XM_PROP_BATTERY_ADAPT_POWER_MATCH 5
#define XM_PROP_VERIFY_PROCESS		19
#define XM_PROP_VDM_CHARGER_VERSION	20
#define XM_PROP_VDM_CHARGER_VOLTAGE	21
#define XM_PROP_VDM_CHARGER_TEMP	22
#define XM_PROP_VDM_SESSION_SEED	23
#define XM_PROP_VDM_AUTHENTICATION	24
#define XM_PROP_VDM_VERIFIED		25
#define XM_PROP_VDM_REMOVE_COMPENSATION 26
#define XM_PROP_VDM_REVERSE_AUTHENTICATION 27
#define XM_PROP_ADAPTER_ID		29
#define XM_PROP_ADAPTER_SVID		30
#define XM_PROP_PD_VERIFIED		31
#define XM_PROP_UVDM_STATE		33
#define XM_PROP_APDO_MAX		60
#define XM_PROP_POWER_MAX		61

#define USBPD_UVDM_CHARGER_VERSION	1
#define USBPD_UVDM_CHARGER_VOLTAGE	2
#define USBPD_UVDM_CHARGER_TEMP	3
#define USBPD_UVDM_SESSION_SEED	4
#define USBPD_UVDM_AUTHENTICATION	5
#define USBPD_UVDM_VERIFIED		6
#define USBPD_UVDM_REMOVE_COMPENSATION	7
#define USBPD_UVDM_REVERSE_AUTHENTICATION 8
#define USBPD_UVDM_WORDS		4
#define USBPD_UVDM_HEX_LEN		(USBPD_UVDM_WORDS * 8)
#define USBPD_XIAOMI_SVID		0x2717
#define USBPD_AUTH_SESSION_TIMEOUT	(120 * HZ)

enum qcom_battmgr_auth_stage {
	QCOM_BATTMGR_AUTH_IDLE,
	QCOM_BATTMGR_AUTH_STARTED,
	QCOM_BATTMGR_AUTH_VERSION,
	QCOM_BATTMGR_AUTH_VOLTAGE,
	QCOM_BATTMGR_AUTH_TEMPERATURE,
	QCOM_BATTMGR_AUTH_SEED,
	QCOM_BATTMGR_AUTH_CHALLENGE,
	QCOM_BATTMGR_AUTH_RESPONSE,
	QCOM_BATTMGR_AUTH_VERIFIED,
	QCOM_BATTMGR_AUTH_REVERSE,
	QCOM_BATTMGR_AUTH_COMMITTED,
};

#define BATTMGR_WLS_PROPERTY_GET	0x34
#define BATTMGR_WLS_PROPERTY_SET	0x35
#define WLS_ONLINE			0
#define WLS_VOLT_NOW			1
#define WLS_VOLT_MAX			2
#define WLS_CURR_NOW			3
#define WLS_CURR_MAX			4
#define WLS_TYPE			5
#define WLS_BOOST_EN			6

#define BATTMGR_CHG_CTRL_LIMIT_EN	0x48
#define CHARGE_CTRL_START_THR_MIN	50
#define CHARGE_CTRL_START_THR_MAX	95
#define CHARGE_CTRL_END_THR_MIN		55
#define CHARGE_CTRL_END_THR_MAX		100
#define CHARGE_CTRL_DELTA_SOC		5

struct qcom_battmgr_enable_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 power_state;
	__le32 low_capacity;
	__le32 high_capacity;
};

struct qcom_battmgr_property_request {
	struct pmic_glink_hdr hdr;
	__le32 battery;
	__le32 property;
	__le32 value;
};

struct qcom_battmgr_xm_words_request {
	struct pmic_glink_hdr hdr;
	__le32 property;
	__le32 data[USBPD_UVDM_WORDS];
};

enum qcom_battmgr_response_kind {
	QCOM_BATTMGR_RESPONSE_NONE,
	QCOM_BATTMGR_RESPONSE_SCALAR,
	QCOM_BATTMGR_RESPONSE_XM_WORDS,
};

struct qcom_battmgr_update_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
};

struct qcom_battmgr_charge_time_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 percent;
	__le32 reserved;
};

struct qcom_battmgr_discharge_time_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 rate; /* 0 for current rate */
	__le32 reserved;
};

struct qcom_battmgr_charge_ctrl_request {
	struct pmic_glink_hdr hdr;
	__le32 enable;
	__le32 target_soc;
	__le32 delta_soc;
};

struct qcom_battmgr_message {
	struct pmic_glink_hdr hdr;
	union {
		struct {
			__le32 property;
			__le32 value;
			__le32 result;
		} intval;
		struct {
			__le32 property;
			char model[BATTMGR_STRING_LEN];
		} strval;
		struct {
			/*
			 * 0: mWh
			 * 1: mAh
			 */
			__le32 power_unit;
			__le32 design_capacity;
			__le32 last_full_capacity;
			/*
			 * 0 nonrechargable
			 * 1 rechargable
			 */
			__le32 battery_tech;
			__le32 design_voltage; /* mV */
			__le32 capacity_low;
			__le32 capacity_warning;
			__le32 cycle_count;
			/* thousandth of percent */
			__le32 accuracy;
			__le32 max_sample_time_ms;
			__le32 min_sample_time_ms;
			__le32 max_average_interval_ms;
			__le32 min_average_interval_ms;
			/* granularity between low and warning */
			__le32 capacity_granularity1;
			/* granularity between warning and full */
			__le32 capacity_granularity2;
			/*
			 * 0: no
			 * 1: cold
			 * 2: hot
			 */
			__le32 swappable;
			__le32 capabilities;
			char model_number[BATTMGR_STRING_LEN];
			char serial_number[BATTMGR_STRING_LEN];
			char battery_type[BATTMGR_STRING_LEN];
			char oem_info[BATTMGR_STRING_LEN];
			char battery_chemistry[BATTMGR_CHEMISTRY_LEN];
			char uid[BATTMGR_STRING_LEN];
			__le32 critical_bias;
			u8 day;
			u8 month;
			__le16 year;
			__le32 battery_id;
		} info;
		struct {
			/*
			 * BIT(0) discharging
			 * BIT(1) charging
			 * BIT(2) critical low
			 */
			__le32 battery_state;
			/* mWh or mAh, based on info->power_unit */
			__le32 capacity;
			__le32 rate;
			/* mv */
			__le32 battery_voltage;
			/*
			 * BIT(0) power online
			 * BIT(1) discharging
			 * BIT(2) charging
			 * BIT(3) battery critical
			 */
			__le32 power_state;
			/*
			 * 1: AC
			 * 2: USB
			 * 3: Wireless
			 */
			__le32 charging_source;
			__le32 temperature;
		} status;
		__le32 time;
		__le32 notification;
	};
};

#define BATTMGR_CHARGING_SOURCE_AC	1
#define BATTMGR_CHARGING_SOURCE_USB	2
#define BATTMGR_CHARGING_SOURCE_WIRELESS 3

enum qcom_battmgr_unit {
	QCOM_BATTMGR_UNIT_mWh = 0,
	QCOM_BATTMGR_UNIT_mAh = 1
};

struct qcom_battmgr_info {
	bool valid;

	bool present;
	unsigned int charge_type;
	unsigned int design_capacity;
	unsigned int last_full_capacity;
	unsigned int voltage_max_design;
	unsigned int voltage_max;
	unsigned int capacity_low;
	unsigned int capacity_warning;
	unsigned int cycle_count;
	unsigned int charge_count;
	unsigned int charge_ctrl_start;
	unsigned int charge_ctrl_end;
	char model_number[BATTMGR_STRING_LEN];
	char serial_number[BATTMGR_STRING_LEN];
	char oem_info[BATTMGR_STRING_LEN];
	unsigned char technology;
	unsigned char day;
	unsigned char month;
	unsigned short year;
};

struct qcom_battmgr_status {
	unsigned int status;
	unsigned int health;
	unsigned int capacity;
	unsigned int percent;
	int current_now;
	int power_now;
	unsigned int voltage_now;
	unsigned int voltage_ocv;
	int temperature;
	unsigned int resistance;
	unsigned int soh_percent;

	unsigned int discharge_time;
	unsigned int charge_time;
};

struct qcom_battmgr_ac {
	bool online;
};

struct qcom_battmgr_usb {
	bool online;
	unsigned int voltage_now;
	unsigned int voltage_max;
	unsigned int current_now;
	unsigned int current_max;
	unsigned int current_limit;
	unsigned int usb_type;
	unsigned int adapter_type_raw;
	unsigned int real_type_raw;
	int temperature;
	unsigned int typec_compliant;
};

struct qcom_battmgr_wireless {
	bool online;
	unsigned int voltage_now;
	unsigned int voltage_max;
	unsigned int current_now;
	unsigned int current_max;
};

struct qcom_battmgr {
	struct device *dev;
	struct pmic_glink_client *client;

	enum qcom_battmgr_variant variant;

	struct power_supply *ac_psy;
	struct power_supply *bat_psy;
	struct power_supply *usb_psy;
	struct power_supply *wls_psy;

	enum qcom_battmgr_unit unit;

	int error;
	struct completion ack;
	int request_opcode;
	int request_property;
	enum qcom_battmgr_response_kind request_kind;
	u32 response_value;
	u32 response_words[USBPD_UVDM_WORDS];

	bool service_up;
	struct mutex auth_lock;
	enum qcom_battmgr_auth_stage auth_stage;
	unsigned long auth_deadline;

	struct qcom_battmgr_info info;
	struct qcom_battmgr_status status;
	struct qcom_battmgr_ac ac;
	struct qcom_battmgr_usb usb;
	struct qcom_battmgr_wireless wireless;
	u32 xm_cache[XM_PROP_POWER_MAX + 1];
	bool xm_valid[XM_PROP_POWER_MAX + 1];

	struct work_struct enable_work;

	/*
	 * @lock is used to prevent concurrent power supply requests to the
	 * firmware, as it then stops responding.
	 */
	struct mutex lock;
};

static void qcom_battmgr_sm8650_auth_invalidate(struct qcom_battmgr *battmgr)
{
	if (battmgr->variant != QCOM_BATTMGR_SM8650)
		return;

	/* The GLINK response callback can run while battmgr->lock is held. */
	WRITE_ONCE(battmgr->auth_stage, QCOM_BATTMGR_AUTH_IDLE);
	WRITE_ONCE(battmgr->auth_deadline, 0);
}

static bool qcom_battmgr_sm8650_auth_live(struct qcom_battmgr *battmgr)
{
	enum qcom_battmgr_auth_stage stage = READ_ONCE(battmgr->auth_stage);

	if (stage == QCOM_BATTMGR_AUTH_IDLE ||
	    stage == QCOM_BATTMGR_AUTH_COMMITTED)
		return false;

	if (time_after_eq(jiffies, READ_ONCE(battmgr->auth_deadline))) {
		qcom_battmgr_sm8650_auth_invalidate(battmgr);
		return false;
	}

	return true;
}

static void qcom_battmgr_sm8650_auth_advance(
	struct qcom_battmgr *battmgr, enum qcom_battmgr_auth_stage stage)
{
	enum qcom_battmgr_auth_stage auth_stage = READ_ONCE(battmgr->auth_stage);

	if (stage > auth_stage &&
	    cmpxchg(&battmgr->auth_stage, auth_stage, stage) != auth_stage)
		return;
	if (READ_ONCE(battmgr->auth_stage) != QCOM_BATTMGR_AUTH_IDLE)
		WRITE_ONCE(battmgr->auth_deadline,
			   jiffies + USBPD_AUTH_SESSION_TIMEOUT);
}

static int qcom_battmgr_request(struct qcom_battmgr *battmgr, void *data, size_t len)
{
	const struct qcom_battmgr_xm_words_request *words_req = data;
	const struct qcom_battmgr_property_request *property_req = data;
	const struct pmic_glink_hdr *hdr = data;
	enum qcom_battmgr_response_kind kind = QCOM_BATTMGR_RESPONSE_NONE;
	unsigned long left;
	int property = -1;
	int opcode;
	int ret;

	reinit_completion(&battmgr->ack);

	battmgr->error = 0;
	opcode = le32_to_cpu(hdr->opcode);
	if (len == sizeof(*property_req) &&
	    (opcode == BATTMGR_BAT_PROPERTY_GET ||
	     opcode == BATTMGR_USB_PROPERTY_GET ||
	     opcode == BATTMGR_WLS_PROPERTY_GET ||
	     opcode == BATTMGR_XM_PROPERTY_GET ||
	     opcode == BATTMGR_XM_PROPERTY_SET)) {
		property = le32_to_cpu(property_req->property);
		kind = QCOM_BATTMGR_RESPONSE_SCALAR;
	} else if (len == sizeof(*words_req) &&
		   (opcode == BATTMGR_XM_PROPERTY_GET ||
		    opcode == BATTMGR_XM_PROPERTY_SET)) {
		property = le32_to_cpu(words_req->property);
		kind = QCOM_BATTMGR_RESPONSE_XM_WORDS;
	}

	WRITE_ONCE(battmgr->request_opcode, opcode);
	WRITE_ONCE(battmgr->request_property, property);
	WRITE_ONCE(battmgr->request_kind, kind);

	ret = pmic_glink_send(battmgr->client, data, len);
	if (ret < 0)
		goto out_clear_request;

	left = wait_for_completion_timeout(&battmgr->ack, HZ);
	if (!left) {
		ret = -ETIMEDOUT;
		goto out_clear_request;
	}

	ret = battmgr->error;

out_clear_request:
	WRITE_ONCE(battmgr->request_opcode, -1);
	WRITE_ONCE(battmgr->request_property, -1);
	WRITE_ONCE(battmgr->request_kind, QCOM_BATTMGR_RESPONSE_NONE);
	return ret;
}

static int qcom_battmgr_request_property(struct qcom_battmgr *battmgr, int opcode,
					 int property, u32 value)
{
	struct qcom_battmgr_property_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.battery = cpu_to_le32(0),
		.property = cpu_to_le32(property),
		.value = cpu_to_le32(value),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_xm_request_words(struct qcom_battmgr *battmgr,
					 int opcode, unsigned int property,
					 const u32 *data)
{
	struct qcom_battmgr_xm_words_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.property = cpu_to_le32(property),
	};
	int i, ret;

	if (data)
		for (i = 0; i < USBPD_UVDM_WORDS; i++)
			request.data[i] = cpu_to_le32(data[i]);

	ret = qcom_battmgr_request(battmgr, &request, sizeof(request));
	memzero_explicit(&request, sizeof(request));

	return ret;
}

static int qcom_battmgr_update_status(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_update_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_STATUS),
		.battery_id = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_info(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_update_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_INFO),
		.battery_id = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_charge_time(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_charge_time_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_CHARGE_TIME),
		.battery_id = cpu_to_le32(0),
		.percent = cpu_to_le32(100),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_discharge_time(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_discharge_time_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_DISCHARGE_TIME),
		.battery_id = cpu_to_le32(0),
		.rate = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static const u8 sm8350_bat_prop_map[] = {
	[POWER_SUPPLY_PROP_STATUS] = BATT_STATUS,
	[POWER_SUPPLY_PROP_HEALTH] = BATT_HEALTH,
	[POWER_SUPPLY_PROP_PRESENT] = BATT_PRESENT,
	[POWER_SUPPLY_PROP_CHARGE_TYPE] = BATT_CHG_TYPE,
	[POWER_SUPPLY_PROP_CAPACITY] = BATT_CAPACITY,
	[POWER_SUPPLY_PROP_VOLTAGE_OCV] = BATT_VOLT_OCV,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = BATT_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = BATT_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = BATT_CURR_NOW,
	[POWER_SUPPLY_PROP_TEMP] = BATT_TEMP,
	[POWER_SUPPLY_PROP_TECHNOLOGY] = BATT_TECHNOLOGY,
	[POWER_SUPPLY_PROP_CHARGE_COUNTER] =  BATT_CHG_COUNTER,
	[POWER_SUPPLY_PROP_CYCLE_COUNT] = BATT_CYCLE_COUNT,
	[POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN] =  BATT_CHG_FULL_DESIGN,
	[POWER_SUPPLY_PROP_CHARGE_FULL] = BATT_CHG_FULL,
	[POWER_SUPPLY_PROP_MODEL_NAME] = BATT_MODEL_NAME,
	[POWER_SUPPLY_PROP_TIME_TO_FULL_AVG] = BATT_TTF_AVG,
	[POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG] = BATT_TTE_AVG,
	[POWER_SUPPLY_PROP_INTERNAL_RESISTANCE] = BATT_RESISTANCE,
	[POWER_SUPPLY_PROP_STATE_OF_HEALTH] = BATT_SOH,
	[POWER_SUPPLY_PROP_POWER_NOW] = BATT_POWER_NOW,
	[POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD] = BATT_CHG_CTRL_START_THR,
	[POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD] = BATT_CHG_CTRL_END_THR,
};

static const u8 sm8650_bat_prop_map[] = {
	[POWER_SUPPLY_PROP_STATUS] = BATT_STATUS,
	[POWER_SUPPLY_PROP_HEALTH] = BATT_HEALTH,
	[POWER_SUPPLY_PROP_PRESENT] = BATT_PRESENT,
	[POWER_SUPPLY_PROP_CHARGE_TYPE] = BATT_CHG_TYPE,
	[POWER_SUPPLY_PROP_CAPACITY] = BATT_CAPACITY,
	[POWER_SUPPLY_PROP_VOLTAGE_OCV] = BATT_VOLT_OCV,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = BATT_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = BATT_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = BATT_CURR_NOW,
	[POWER_SUPPLY_PROP_TEMP] = SM8650_BATT_TEMP,
	[POWER_SUPPLY_PROP_TECHNOLOGY] = SM8650_BATT_TECHNOLOGY,
	[POWER_SUPPLY_PROP_CYCLE_COUNT] = SM8650_BATT_CYCLE_COUNT,
	[POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN] = SM8650_BATT_CHG_FULL_DESIGN,
	[POWER_SUPPLY_PROP_CHARGE_FULL] = SM8650_BATT_CHG_FULL,
	[POWER_SUPPLY_PROP_MODEL_NAME] = SM8650_BATT_MODEL_NAME,
};

static int qcom_battmgr_bat_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	const u8 *prop_map = sm8350_bat_prop_map;
	size_t prop_map_size = ARRAY_SIZE(sm8350_bat_prop_map);
	unsigned int prop;
	int ret;

	if (battmgr->variant == QCOM_BATTMGR_SM8650) {
		prop_map = sm8650_bat_prop_map;
		prop_map_size = ARRAY_SIZE(sm8650_bat_prop_map);
	}

	if (psp >= prop_map_size)
		return -EINVAL;

	prop = prop_map[psp];

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_bat_sc8280xp_update(struct qcom_battmgr *battmgr,
					    enum power_supply_property psp)
{
	int ret;

	mutex_lock(&battmgr->lock);

	if (!battmgr->info.valid) {
		ret = qcom_battmgr_update_info(battmgr);
		if (ret < 0)
			goto out_unlock;
		battmgr->info.valid = true;
	}

	ret = qcom_battmgr_update_status(battmgr);
	if (ret < 0)
		goto out_unlock;

	if (psp == POWER_SUPPLY_PROP_TIME_TO_FULL_AVG) {
		ret = qcom_battmgr_update_charge_time(battmgr);
		if (ret < 0) {
			ret = -ENODATA;
			goto out_unlock;
		}
	}

	if (psp == POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG) {
		ret = qcom_battmgr_update_discharge_time(battmgr);
		if (ret < 0) {
			ret = -ENODATA;
			goto out_unlock;
		}
	}

out_unlock:
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_bat_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	enum qcom_battmgr_unit unit = battmgr->unit;
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
	    battmgr->variant == QCOM_BATTMGR_X1E80100)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_bat_sm8350_update(battmgr, psp);
	if (ret < 0)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = battmgr->status.status;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = battmgr->info.charge_type;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = battmgr->status.health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = battmgr->info.present;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = battmgr->info.technology;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = battmgr->info.cycle_count;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = battmgr->info.voltage_max_design;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->info.voltage_max;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->status.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = battmgr->status.voltage_ocv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->status.current_now;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		val->intval = battmgr->status.power_now;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		if (battmgr->variant != QCOM_BATTMGR_SM8650 &&
		    unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.design_capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (battmgr->variant != QCOM_BATTMGR_SM8650 &&
		    unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.last_full_capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_EMPTY:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.capacity_low;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->status.capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = battmgr->info.charge_count;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.design_capacity;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.last_full_capacity;
		break;
	case POWER_SUPPLY_PROP_ENERGY_EMPTY:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.capacity_low;
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->status.capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (battmgr->status.percent == (unsigned int)-1)
			return -ENODATA;
		val->intval = battmgr->status.percent;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = battmgr->status.temperature;
		break;
	case POWER_SUPPLY_PROP_INTERNAL_RESISTANCE:
		val->intval = battmgr->status.resistance;
		break;
	case POWER_SUPPLY_PROP_STATE_OF_HEALTH:
		val->intval = battmgr->status.soh_percent;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		val->intval = battmgr->status.discharge_time;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		val->intval = battmgr->status.charge_time;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		val->intval = battmgr->info.charge_ctrl_start;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = battmgr->info.charge_ctrl_end;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_YEAR:
		val->intval = battmgr->info.year;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_MONTH:
		val->intval = battmgr->info.month;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_DAY:
		val->intval = battmgr->info.day;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = battmgr->info.model_number;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = battmgr->info.oem_info;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = battmgr->info.serial_number;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int qcom_battmgr_set_charge_control(struct qcom_battmgr *battmgr,
					   u32 target_soc, u32 delta_soc)
{
	struct qcom_battmgr_charge_ctrl_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_CHG_CTRL_LIMIT_EN),
		.enable = cpu_to_le32(1),
		.target_soc = cpu_to_le32(target_soc),
		.delta_soc = cpu_to_le32(delta_soc),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_set_charge_start_threshold(struct qcom_battmgr *battmgr, int start_soc)
{
	u32 target_soc, delta_soc;
	int ret;

	start_soc = clamp(start_soc, CHARGE_CTRL_START_THR_MIN, CHARGE_CTRL_START_THR_MAX);

	/*
	 * If the new start threshold is larger than the old end threshold,
	 * move the end threshold one step (DELTA_SOC) after the new start
	 * threshold.
	 */
	if (start_soc > battmgr->info.charge_ctrl_end) {
		target_soc = start_soc + CHARGE_CTRL_DELTA_SOC;
		target_soc = min_t(u32, target_soc, CHARGE_CTRL_END_THR_MAX);
		delta_soc = target_soc - start_soc;
		delta_soc = min_t(u32, delta_soc, CHARGE_CTRL_DELTA_SOC);
	} else {
		target_soc =  battmgr->info.charge_ctrl_end;
		delta_soc = battmgr->info.charge_ctrl_end - start_soc;
	}

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_set_charge_control(battmgr, target_soc, delta_soc);
	mutex_unlock(&battmgr->lock);
	if (!ret) {
		battmgr->info.charge_ctrl_start = start_soc;
		battmgr->info.charge_ctrl_end = target_soc;
	}

	return 0;
}

static int qcom_battmgr_set_charge_end_threshold(struct qcom_battmgr *battmgr, int end_soc)
{
	u32 delta_soc = CHARGE_CTRL_DELTA_SOC;
	int ret;

	end_soc = clamp(end_soc, CHARGE_CTRL_END_THR_MIN, CHARGE_CTRL_END_THR_MAX);

	if (battmgr->info.charge_ctrl_start && end_soc > battmgr->info.charge_ctrl_start)
		delta_soc = end_soc - battmgr->info.charge_ctrl_start;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_set_charge_control(battmgr, end_soc, delta_soc);
	mutex_unlock(&battmgr->lock);
	if (!ret) {
		battmgr->info.charge_ctrl_start = end_soc - delta_soc;
		battmgr->info.charge_ctrl_end = end_soc;
	}

	return 0;
}

static int qcom_battmgr_charge_control_thresholds_init(struct qcom_battmgr *battmgr)
{
	int ret;
	u8 en, end_soc, start_soc, delta_soc;

	ret = nvmem_cell_read_u8(battmgr->dev->parent, "charge_limit_en", &en);
	if (!ret && en != 0) {
		ret = nvmem_cell_read_u8(battmgr->dev->parent, "charge_limit_end", &end_soc);
		if (ret < 0)
			return ret;

		ret = nvmem_cell_read_u8(battmgr->dev->parent, "charge_limit_delta", &delta_soc);
		if (ret < 0)
			return ret;

		if (delta_soc >= end_soc)
			return -EINVAL;

		start_soc = end_soc - delta_soc;
		end_soc = clamp(end_soc, CHARGE_CTRL_END_THR_MIN, CHARGE_CTRL_END_THR_MAX);
		start_soc = clamp(start_soc, CHARGE_CTRL_START_THR_MIN, CHARGE_CTRL_START_THR_MAX);

		battmgr->info.charge_ctrl_start = start_soc;
		battmgr->info.charge_ctrl_end = end_soc;
	}

	return 0;
}

static int qcom_battmgr_bat_is_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		return 1;
	default:
		return 0;
	}

	return 0;
}

static int qcom_battmgr_bat_set_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 const union power_supply_propval *pval)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);

	if (!battmgr->service_up)
		return -EAGAIN;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		return qcom_battmgr_set_charge_start_threshold(battmgr, pval->intval);
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		return qcom_battmgr_set_charge_end_threshold(battmgr, pval->intval);
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_EMPTY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_EMPTY,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MANUFACTURE_YEAR,
	POWER_SUPPLY_PROP_MANUFACTURE_MONTH,
	POWER_SUPPLY_PROP_MANUFACTURE_DAY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static const struct power_supply_desc sc8280xp_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sc8280xp_bat_props,
	.num_properties = ARRAY_SIZE(sc8280xp_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
};

static const enum power_supply_property x1e80100_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_EMPTY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_EMPTY,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MANUFACTURE_YEAR,
	POWER_SUPPLY_PROP_MANUFACTURE_MONTH,
	POWER_SUPPLY_PROP_MANUFACTURE_DAY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_desc x1e80100_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = x1e80100_bat_props,
	.num_properties = ARRAY_SIZE(x1e80100_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
	.set_property = qcom_battmgr_bat_set_property,
	.property_is_writeable = qcom_battmgr_bat_is_writeable,
};

static const enum power_supply_property sm8350_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_INTERNAL_RESISTANCE,
	POWER_SUPPLY_PROP_STATE_OF_HEALTH,
	POWER_SUPPLY_PROP_POWER_NOW,
};

static const struct power_supply_desc sm8350_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm8350_bat_props,
	.num_properties = ARRAY_SIZE(sm8350_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
};

static const enum power_supply_property sm8550_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_INTERNAL_RESISTANCE,
	POWER_SUPPLY_PROP_STATE_OF_HEALTH,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_desc sm8550_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm8550_bat_props,
	.num_properties = ARRAY_SIZE(sm8550_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
	.set_property = qcom_battmgr_bat_set_property,
	.property_is_writeable = qcom_battmgr_bat_is_writeable,
};

static const enum power_supply_property sm8650_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static const struct power_supply_desc sm8650_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm8650_bat_props,
	.num_properties = ARRAY_SIZE(sm8650_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
};

static int qcom_battmgr_ac_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->ac.online;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_ac_psy_desc = {
	.name = "qcom-battmgr-ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = sc8280xp_ac_props,
	.num_properties = ARRAY_SIZE(sc8280xp_ac_props),
	.get_property = qcom_battmgr_ac_get_property,
};

static const u8 sm8350_usb_prop_map[] = {
	[POWER_SUPPLY_PROP_ONLINE] = USB_ONLINE,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = USB_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = USB_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = USB_CURR_NOW,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = USB_CURR_MAX,
	[POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT] = USB_INPUT_CURR_LIMIT,
	[POWER_SUPPLY_PROP_USB_TYPE] = USB_TYPE,
};

static const u8 sm8650_usb_prop_map[] = {
	[POWER_SUPPLY_PROP_ONLINE] = USB_ONLINE,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = USB_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = USB_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = USB_CURR_NOW,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = USB_CURR_MAX,
	[POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT] = USB_INPUT_CURR_LIMIT,
	[POWER_SUPPLY_PROP_USB_TYPE] = USB_ADAP_TYPE,
	[POWER_SUPPLY_PROP_TEMP] = USB_TEMP,
};

static int qcom_battmgr_usb_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	const u8 *prop_map = sm8350_usb_prop_map;
	size_t prop_map_size = ARRAY_SIZE(sm8350_usb_prop_map);
	unsigned int prop;
	int ret;

	if (battmgr->variant == QCOM_BATTMGR_SM8650) {
		prop_map = sm8650_usb_prop_map;
		prop_map_size = ARRAY_SIZE(sm8650_usb_prop_map);
	}

	if (psp >= prop_map_size)
		return -EINVAL;

	prop = prop_map[psp];

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_usb_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
	    battmgr->variant == QCOM_BATTMGR_X1E80100)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_usb_sm8350_update(battmgr, psp);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->usb.online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->usb.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->usb.voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->usb.current_now;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = battmgr->usb.current_max;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = battmgr->usb.current_limit;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = battmgr->usb.usb_type;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = battmgr->usb.temperature;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_usb_psy_desc = {
	.name = "qcom-battmgr-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sc8280xp_usb_props,
	.num_properties = ARRAY_SIZE(sc8280xp_usb_props),
	.get_property = qcom_battmgr_usb_get_property,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |
		     BIT(POWER_SUPPLY_USB_TYPE_C)       |
		     BIT(POWER_SUPPLY_USB_TYPE_PD)      |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
};

static const enum power_supply_property sm8350_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static const struct power_supply_desc sm8350_usb_psy_desc = {
	.name = "qcom-battmgr-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sm8350_usb_props,
	.num_properties = ARRAY_SIZE(sm8350_usb_props),
	.get_property = qcom_battmgr_usb_get_property,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |
		     BIT(POWER_SUPPLY_USB_TYPE_C)       |
		     BIT(POWER_SUPPLY_USB_TYPE_PD)      |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
};

static const enum power_supply_property sm8650_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
	POWER_SUPPLY_PROP_TEMP,
};

static const struct power_supply_desc sm8650_usb_psy_desc = {
	.name = "qcom-battmgr-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sm8650_usb_props,
	.num_properties = ARRAY_SIZE(sm8650_usb_props),
	.get_property = qcom_battmgr_usb_get_property,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |
		     BIT(POWER_SUPPLY_USB_TYPE_C)       |
		     BIT(POWER_SUPPLY_USB_TYPE_PD)      |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
};

static const u8 sm8350_wls_prop_map[] = {
	[POWER_SUPPLY_PROP_ONLINE] = WLS_ONLINE,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = WLS_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = WLS_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = WLS_CURR_NOW,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = WLS_CURR_MAX,
};

static int qcom_battmgr_wls_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	unsigned int prop;
	int ret;

	if (psp >= ARRAY_SIZE(sm8350_wls_prop_map))
		return -EINVAL;

	prop = sm8350_wls_prop_map[psp];

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_WLS_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_wls_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
	    battmgr->variant == QCOM_BATTMGR_X1E80100)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_wls_sm8350_update(battmgr, psp);
	if (ret < 0)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->wireless.online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->wireless.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->wireless.voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->wireless.current_now;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = battmgr->wireless.current_max;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_wls_psy_desc = {
	.name = "qcom-battmgr-wls",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = sc8280xp_wls_props,
	.num_properties = ARRAY_SIZE(sc8280xp_wls_props),
	.get_property = qcom_battmgr_wls_get_property,
};

static const enum power_supply_property sm8350_wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static const struct power_supply_desc sm8350_wls_psy_desc = {
	.name = "qcom-battmgr-wls",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = sm8350_wls_props,
	.num_properties = ARRAY_SIZE(sm8350_wls_props),
	.get_property = qcom_battmgr_wls_get_property,
};

static const enum power_supply_property sm8650_wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sm8650_wls_psy_desc = {
	.name = "qcom-battmgr-wls",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = sm8650_wls_props,
	.num_properties = ARRAY_SIZE(sm8650_wls_props),
	.get_property = qcom_battmgr_wls_get_property,
};

static void qcom_battmgr_notification(struct qcom_battmgr *battmgr,
				      const struct qcom_battmgr_message *msg,
				      int len)
{
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);
	unsigned int notification;

	if (payload_len != sizeof(msg->notification)) {
		dev_warn(battmgr->dev, "ignoring notification with invalid length\n");
		return;
	}

	notification = le32_to_cpu(msg->notification) & 0xff;
	switch (notification) {
	case NOTIF_BAT_INFO:
		battmgr->info.valid = false;
		fallthrough;
	case NOTIF_BAT_STATUS:
	case NOTIF_BAT_PROPERTY:
	case NOTIF_BAT_CHARGING_STATE:
		power_supply_changed(battmgr->bat_psy);
		break;
	case NOTIF_USB_PROPERTY:
		power_supply_changed(battmgr->usb_psy);
		break;
	case NOTIF_WLS_PROPERTY:
		power_supply_changed(battmgr->wls_psy);
		break;
	case NOTIF_XM_PROPERTY:
		if (battmgr->variant == QCOM_BATTMGR_SM8650)
			power_supply_changed(battmgr->bat_psy);
		else
			dev_err(battmgr->dev, "unknown notification: %#x\n", notification);
		break;
	default:
		dev_err(battmgr->dev, "unknown notification: %#x\n", notification);
		break;
	}
}

static void qcom_battmgr_sc8280xp_strcpy(char *dest, const char *src)
{
	size_t len = src[0];

	/* Some firmware versions return Pascal-style strings */
	if (len < BATTMGR_STRING_LEN && len == strnlen(src + 1, BATTMGR_STRING_LEN - 1)) {
		memcpy(dest, src + 1, len);
		dest[len] = '\0';
	} else {
		strscpy(dest, src, BATTMGR_STRING_LEN);
	}
}

static unsigned int qcom_battmgr_sc8280xp_parse_technology(const char *chemistry)
{
	if ((!strncmp(chemistry, "LIO", BATTMGR_CHEMISTRY_LEN)) ||
	    (!strncmp(chemistry, "OOI", BATTMGR_CHEMISTRY_LEN)))
		return POWER_SUPPLY_TECHNOLOGY_LION;
	if (!strncmp(chemistry, "LIP", BATTMGR_CHEMISTRY_LEN) ||
	    !strncmp(chemistry, "LiP", BATTMGR_CHEMISTRY_LEN))
		return POWER_SUPPLY_TECHNOLOGY_LIPO;

	pr_err("Unknown battery technology '%s'\n", chemistry);
	return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
}

static unsigned int qcom_battmgr_sc8280xp_convert_temp(unsigned int temperature)
{
	return DIV_ROUND_CLOSEST(temperature, 10);
}

static void qcom_battmgr_sc8280xp_callback(struct qcom_battmgr *battmgr,
					   const struct qcom_battmgr_message *resp,
					   size_t len)
{
	unsigned int opcode = le32_to_cpu(resp->hdr.opcode);
	unsigned int source;
	unsigned int state;
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);

	if (payload_len < sizeof(__le32)) {
		dev_warn(battmgr->dev, "invalid payload length for %#x: %zd\n",
			 opcode, len);
		return;
	}

	switch (opcode) {
	case BATTMGR_REQUEST_NOTIFICATION:
		battmgr->error = 0;
		break;
	case BATTMGR_BAT_INFO:
		/* some firmware versions report an extra __le32 at the end of the payload */
		if (payload_len != sizeof(resp->info) &&
		    payload_len != (sizeof(resp->info) + sizeof(__le32))) {
			dev_warn(battmgr->dev,
				 "invalid payload length for battery information request: %zd\n",
				 payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->unit = le32_to_cpu(resp->info.power_unit);

		battmgr->info.present = true;
		battmgr->info.design_capacity = le32_to_cpu(resp->info.design_capacity) * 1000;
		battmgr->info.last_full_capacity = le32_to_cpu(resp->info.last_full_capacity) * 1000;
		battmgr->info.voltage_max_design = le32_to_cpu(resp->info.design_voltage) * 1000;
		battmgr->info.capacity_low = le32_to_cpu(resp->info.capacity_low) * 1000;
		battmgr->info.cycle_count = le32_to_cpu(resp->info.cycle_count);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.model_number, resp->info.model_number);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.serial_number, resp->info.serial_number);
		battmgr->info.technology = qcom_battmgr_sc8280xp_parse_technology(resp->info.battery_chemistry);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.oem_info, resp->info.oem_info);
		battmgr->info.day = resp->info.day;
		battmgr->info.month = resp->info.month;
		battmgr->info.year = le16_to_cpu(resp->info.year);
		break;
	case BATTMGR_BAT_STATUS:
		if (payload_len != sizeof(resp->status)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for battery status request: %zd\n",
				 payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		state = le32_to_cpu(resp->status.battery_state);
		if (state & BIT(0))
			battmgr->status.status = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (state & BIT(1))
			battmgr->status.status = POWER_SUPPLY_STATUS_CHARGING;
		else
			battmgr->status.status = POWER_SUPPLY_STATUS_NOT_CHARGING;

		battmgr->status.capacity = le32_to_cpu(resp->status.capacity) * 1000;
		battmgr->status.power_now = le32_to_cpu(resp->status.rate) * 1000;
		battmgr->status.voltage_now = le32_to_cpu(resp->status.battery_voltage) * 1000;
		battmgr->status.temperature = qcom_battmgr_sc8280xp_convert_temp(le32_to_cpu(resp->status.temperature));

		source = le32_to_cpu(resp->status.charging_source);
		battmgr->ac.online = source == BATTMGR_CHARGING_SOURCE_AC;
		battmgr->usb.online = source == BATTMGR_CHARGING_SOURCE_USB;
		battmgr->wireless.online = source == BATTMGR_CHARGING_SOURCE_WIRELESS;
		if (battmgr->info.last_full_capacity != 0) {
			/*
			 * 100 * battmgr->status.capacity can overflow a 32bit
			 * unsigned integer. FW readings are in m{W/A}h, which
			 * are multiplied by 1000 converting them to u{W/A}h,
			 * the format the power_supply API expects.
			 * To avoid overflow use the original value for dividend
			 * and convert the divider back to m{W/A}h, which can be
			 * done without any loss of precision.
			 */
			battmgr->status.percent =
				(100 * le32_to_cpu(resp->status.capacity)) /
				(battmgr->info.last_full_capacity / 1000);
		} else {
			/*
			 * Let the sysfs handler know no data is available at
			 * this time.
			 */
			battmgr->status.percent = (unsigned int)-1;
		}
		break;
	case BATTMGR_BAT_DISCHARGE_TIME:
		battmgr->status.discharge_time = le32_to_cpu(resp->time);
		break;
	case BATTMGR_BAT_CHARGE_TIME:
		battmgr->status.charge_time = le32_to_cpu(resp->time);
		break;
	case BATTMGR_CHG_CTRL_LIMIT_EN:
		battmgr->error = 0;
		break;
	default:
		dev_warn(battmgr->dev, "unknown message %#x\n", opcode);
		break;
	}

	complete(&battmgr->ack);
}

static void qcom_battmgr_sm8350_callback(struct qcom_battmgr *battmgr,
					 const struct qcom_battmgr_message *resp,
					 size_t len)
{
	unsigned int property;
	unsigned int opcode = le32_to_cpu(resp->hdr.opcode);
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);
	unsigned int val;

	if (payload_len < sizeof(__le32)) {
		dev_warn(battmgr->dev, "invalid payload length for %#x: %zd\n",
			 opcode, len);
		return;
	}

	switch (opcode) {
	case BATTMGR_BAT_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (property == BATT_MODEL_NAME) {
			if (payload_len != sizeof(resp->strval)) {
				dev_warn(battmgr->dev,
					 "invalid payload length for BATT_MODEL_NAME request: %zd\n",
					 payload_len);
				battmgr->error = -ENODATA;
				return;
			}
		} else {
			if (payload_len != sizeof(resp->intval)) {
				dev_warn(battmgr->dev,
					 "invalid payload length for %#x request: %zd\n",
					 property, payload_len);
				battmgr->error = -ENODATA;
				return;
			}

			battmgr->error = le32_to_cpu(resp->intval.result);
			if (battmgr->error)
				goto out_complete;
		}

		switch (property) {
		case BATT_STATUS:
			battmgr->status.status = le32_to_cpu(resp->intval.value);
			break;
		case BATT_HEALTH:
			battmgr->status.health = le32_to_cpu(resp->intval.value);
			break;
		case BATT_PRESENT:
			battmgr->info.present = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_TYPE:
			battmgr->info.charge_type = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CAPACITY:
			battmgr->status.percent = le32_to_cpu(resp->intval.value) / 100;
			break;
		case BATT_SOH:
			battmgr->status.soh_percent = le32_to_cpu(resp->intval.value);
			break;
		case BATT_VOLT_OCV:
			battmgr->status.voltage_ocv = le32_to_cpu(resp->intval.value);
			break;
		case BATT_VOLT_NOW:
			battmgr->status.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case BATT_VOLT_MAX:
			battmgr->info.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CURR_NOW:
			battmgr->status.current_now = le32_to_cpu(resp->intval.value);
			break;
		case BATT_TEMP:
			val = le32_to_cpu(resp->intval.value);
			battmgr->status.temperature = DIV_ROUND_CLOSEST(val, 10);
			break;
		case BATT_TECHNOLOGY:
			battmgr->info.technology = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_COUNTER:
			battmgr->info.charge_count = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CYCLE_COUNT:
			battmgr->info.cycle_count = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_FULL_DESIGN:
			battmgr->info.design_capacity = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_FULL:
			battmgr->info.last_full_capacity = le32_to_cpu(resp->intval.value);
			break;
		case BATT_MODEL_NAME:
			strscpy(battmgr->info.model_number, resp->strval.model, BATTMGR_STRING_LEN);
			break;
		case BATT_TTF_AVG:
			battmgr->status.charge_time = le32_to_cpu(resp->intval.value);
			break;
		case BATT_TTE_AVG:
			battmgr->status.discharge_time = le32_to_cpu(resp->intval.value);
			break;
		case BATT_RESISTANCE:
			battmgr->status.resistance = le32_to_cpu(resp->intval.value);
			break;
		case BATT_POWER_NOW:
			battmgr->status.power_now = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_CTRL_START_THR:
			battmgr->info.charge_ctrl_start = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_CTRL_END_THR:
			battmgr->info.charge_ctrl_end = le32_to_cpu(resp->intval.value);
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_USB_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (payload_len != sizeof(resp->intval)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for %#x request: %zd\n",
				 property, payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->error = le32_to_cpu(resp->intval.result);
		if (battmgr->error)
			goto out_complete;

		switch (property) {
		case USB_ONLINE:
			battmgr->usb.online = le32_to_cpu(resp->intval.value);
			if (battmgr->variant == QCOM_BATTMGR_SM8650 &&
			    !battmgr->usb.online)
				qcom_battmgr_sm8650_auth_invalidate(battmgr);
			break;
		case USB_VOLT_NOW:
			battmgr->usb.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case USB_VOLT_MAX:
			battmgr->usb.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case USB_CURR_NOW:
			battmgr->usb.current_now = le32_to_cpu(resp->intval.value);
			break;
		case USB_CURR_MAX:
			battmgr->usb.current_max = le32_to_cpu(resp->intval.value);
			break;
		case USB_INPUT_CURR_LIMIT:
			battmgr->usb.current_limit = le32_to_cpu(resp->intval.value);
			break;
		case USB_TYPE:
			battmgr->usb.usb_type = le32_to_cpu(resp->intval.value);
			break;
		case USB_ADAP_TYPE:
			val = le32_to_cpu(resp->intval.value);
			if (battmgr->variant == QCOM_BATTMGR_SM8650) {
				battmgr->usb.adapter_type_raw = val;
				switch (val) {
				case QTI_USB_TYPE_HVDCP:
				case QTI_USB_TYPE_HVDCP_3:
				case QTI_USB_TYPE_HVDCP_3P5:
				case QTI_USB_TYPE_HVDCP_3_CLASSB:
					battmgr->usb.usb_type = POWER_SUPPLY_USB_TYPE_DCP;
					break;
				case QTI_USB_TYPE_USB_FLOAT:
					battmgr->usb.usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
					break;
				default:
					if (val <= POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID)
						battmgr->usb.usb_type = val;
					else
						battmgr->usb.usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
					break;
				}
			} else {
				battmgr->usb.usb_type = val;
			}
			break;
		case USB_TEMP:
			if (battmgr->variant == QCOM_BATTMGR_SM8650)
				battmgr->usb.temperature = DIV_ROUND_CLOSEST(
					(s32)le32_to_cpu(resp->intval.value), 10);
			break;
		case USB_REAL_TYPE:
			if (battmgr->variant == QCOM_BATTMGR_SM8650)
				battmgr->usb.real_type_raw = le32_to_cpu(resp->intval.value);
			break;
		case USB_TYPEC_COMPLIANT:
			if (battmgr->variant == QCOM_BATTMGR_SM8650)
				battmgr->usb.typec_compliant = le32_to_cpu(resp->intval.value);
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_WLS_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (payload_len != sizeof(resp->intval)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for %#x request: %zd\n",
				 property, payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->error = le32_to_cpu(resp->intval.result);
		if (battmgr->error)
			goto out_complete;

		switch (property) {
		case WLS_ONLINE:
			battmgr->wireless.online = le32_to_cpu(resp->intval.value);
			break;
		case WLS_VOLT_NOW:
			battmgr->wireless.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case WLS_VOLT_MAX:
			battmgr->wireless.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case WLS_CURR_NOW:
			battmgr->wireless.current_now = le32_to_cpu(resp->intval.value);
			break;
		case WLS_CURR_MAX:
			battmgr->wireless.current_max = le32_to_cpu(resp->intval.value);
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_REQUEST_NOTIFICATION:
	case BATTMGR_CHG_CTRL_LIMIT_EN:
		battmgr->error = 0;
		break;
	default:
		dev_warn(battmgr->dev, "unknown message %#x\n", opcode);
		break;
	}

out_complete:
	complete(&battmgr->ack);
}

static void qcom_battmgr_sm8650_callback(struct qcom_battmgr *battmgr,
					 const struct qcom_battmgr_message *resp,
					 size_t len)
{
	unsigned int opcode = le32_to_cpu(resp->hdr.opcode);
	unsigned int property;
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);
	u32 old;
	s32 val;

	if (opcode == BATTMGR_XM_PROPERTY_GET ||
	    opcode == BATTMGR_XM_PROPERTY_SET) {
		enum qcom_battmgr_response_kind kind =
			READ_ONCE(battmgr->request_kind);
		const struct qcom_battmgr_xm_words_request *words =
			(const void *)resp;
		int i;

		property = le32_to_cpu(resp->intval.property);
		if (kind == QCOM_BATTMGR_RESPONSE_XM_WORDS) {
			if (opcode == BATTMGR_XM_PROPERTY_GET) {
				if (payload_len != sizeof(words->property) +
						   sizeof(words->data)) {
					battmgr->error = -ENODATA;
					goto out_complete;
				}

				for (i = 0; i < USBPD_UVDM_WORDS; i++)
					battmgr->response_words[i] =
						le32_to_cpu(words->data[i]);
			} else if (payload_len == sizeof(resp->intval) &&
				   le32_to_cpu(resp->intval.result)) {
				battmgr->error = -EREMOTEIO;
			}
			goto out_complete;
		}

		if (kind != QCOM_BATTMGR_RESPONSE_SCALAR ||
		    payload_len != sizeof(resp->intval)) {
			battmgr->error = -ENODATA;
			goto out_complete;
		}

		if (le32_to_cpu(resp->intval.result)) {
			battmgr->error = -EREMOTEIO;
			goto out_complete;
		}

		if (opcode == BATTMGR_XM_PROPERTY_GET)
			battmgr->response_value = le32_to_cpu(resp->intval.value);
		goto out_complete;
	}

	if (opcode != BATTMGR_BAT_PROPERTY_GET) {
		qcom_battmgr_sm8350_callback(battmgr, resp, len);
		return;
	}

	property = le32_to_cpu(resp->intval.property);
	if (property == SM8650_BATT_MODEL_NAME) {
		if (payload_len != sizeof(resp->strval)) {
			dev_warn(battmgr->dev,
				 "invalid SM8650 model response length: %zd\n",
				 payload_len);
			battmgr->error = -ENODATA;
			goto out_complete;
		}
	} else {
		if (payload_len != sizeof(resp->intval)) {
			dev_warn(battmgr->dev,
				 "invalid SM8650 property %#x response length: %zd\n",
				 property, payload_len);
			battmgr->error = -ENODATA;
			goto out_complete;
		}

		battmgr->error = le32_to_cpu(resp->intval.result);
		if (battmgr->error)
			goto out_complete;
	}

	switch (property) {
	case BATT_STATUS:
		val = le32_to_cpu(resp->intval.value);
		if (val < POWER_SUPPLY_STATUS_UNKNOWN ||
		    val > POWER_SUPPLY_STATUS_FULL) {
			dev_warn_ratelimited(battmgr->dev,
					     "rejecting SM8650 battery status %d\n",
					     val);
			battmgr->error = -ENODATA;
			break;
		}
		old = battmgr->status.status;
		battmgr->status.status = val;
		if (old != val)
			dev_info(battmgr->dev,
				 "SM8650 battery status %u -> %d\n", old, val);
		break;
	case BATT_HEALTH:
		battmgr->status.health = le32_to_cpu(resp->intval.value);
		break;
	case BATT_PRESENT:
		val = le32_to_cpu(resp->intval.value);
		if (val < 0 || val > 1) {
			dev_warn_ratelimited(battmgr->dev,
					     "rejecting SM8650 battery present %d\n",
					     val);
			battmgr->error = -ENODATA;
			break;
		}
		old = battmgr->info.present;
		battmgr->info.present = val;
		if (old != val)
			dev_info(battmgr->dev,
				 "SM8650 battery present %u -> %d\n", old, val);
		break;
	case BATT_CHG_TYPE:
		battmgr->info.charge_type = le32_to_cpu(resp->intval.value);
		break;
	case BATT_CAPACITY:
		old = battmgr->status.percent;
		val = le32_to_cpu(resp->intval.value);
		if (val < 0 || val > 10000) {
			dev_warn_ratelimited(battmgr->dev,
					     "rejecting SM8650 battery capacity %d\n",
					     val);
			battmgr->error = -ENODATA;
			break;
		}

		/*
		 * charger_pd can transiently return a near-zero value during cable
		 * events.  The stock zorn driver keeps its previous SOC when zero follows a
		 * capacity transaction timeout and VBAT is safely above shutdown.
		 * The upstream transport cannot identify that firmware-side stale
		 * response, so apply the same voltage sanity check before exposing a
		 * value which still rounds down to zero percent.
		 */
		if (val < 100 && old != (unsigned int)-1 && old > 0 &&
		    battmgr->status.voltage_now >
			SM8650_SHUTDOWN_VOLTAGE_UV + SM8650_SOC_ZERO_MARGIN_UV) {
			dev_warn_ratelimited(battmgr->dev,
					     "ignoring SM8650 near-zero capacity %d at %u uV; keeping %u%%\n",
					     val, battmgr->status.voltage_now, old);
			break;
		}

		battmgr->status.percent = val / 100;
		if (old != battmgr->status.percent)
			dev_info(battmgr->dev,
				 "SM8650 battery capacity %u -> %u%% (raw %d)\n",
				 old, battmgr->status.percent, val);
		break;
	case BATT_VOLT_OCV:
		battmgr->status.voltage_ocv = le32_to_cpu(resp->intval.value);
		break;
	case BATT_VOLT_NOW:
		battmgr->status.voltage_now = le32_to_cpu(resp->intval.value);
		break;
	case BATT_VOLT_MAX:
		battmgr->info.voltage_max = le32_to_cpu(resp->intval.value);
		break;
	case BATT_CURR_NOW:
		/* Xiaomi reports positive discharge current, opposite to Linux. */
		battmgr->status.current_now =
			(s32)(0U - le32_to_cpu(resp->intval.value));
		break;
	case SM8650_BATT_TEMP:
		val = (s32)le32_to_cpu(resp->intval.value);
		battmgr->status.temperature = DIV_ROUND_CLOSEST(val, 10);
		break;
	case SM8650_BATT_TECHNOLOGY:
		battmgr->info.technology = le32_to_cpu(resp->intval.value);
		break;
	case SM8650_BATT_CYCLE_COUNT:
		battmgr->info.cycle_count = le32_to_cpu(resp->intval.value);
		break;
	case SM8650_BATT_CHG_FULL_DESIGN:
		battmgr->info.design_capacity = le32_to_cpu(resp->intval.value);
		break;
	case SM8650_BATT_CHG_FULL:
		battmgr->info.last_full_capacity = le32_to_cpu(resp->intval.value);
		break;
	case SM8650_BATT_MODEL_NAME:
		strscpy(battmgr->info.model_number, resp->strval.model,
			BATTMGR_STRING_LEN);
		break;
	default:
		battmgr->error = -ENODATA;
		break;
	}

out_complete:
	complete(&battmgr->ack);
}

static void qcom_battmgr_callback(const void *data, size_t len, void *priv)
{
	const struct qcom_battmgr_message *resp = data;
	const struct pmic_glink_hdr *hdr = data;
	struct qcom_battmgr *battmgr = priv;
	unsigned int opcode = le32_to_cpu(hdr->opcode);
	int property;

	if (opcode == BATTMGR_NOTIFICATION) {
		qcom_battmgr_notification(battmgr, data, len);
		return;
	}

	if (battmgr->variant == QCOM_BATTMGR_SM8650) {
		enum qcom_battmgr_response_kind kind =
			READ_ONCE(battmgr->request_kind);
		size_t expected_len = 0;

		/* Known unsolicited Xiaomi status uploads are not transactions. */
		if (opcode == BATTMGR_XM_PROPERTY_GET && len == 220)
			return;

		if (opcode != READ_ONCE(battmgr->request_opcode)) {
			dev_warn_ratelimited(battmgr->dev,
					     "SM8650 drop opcode=%#x len=%zu waiting=%#x/%d\n",
					     opcode, len,
					     READ_ONCE(battmgr->request_opcode),
					     READ_ONCE(battmgr->request_property));
			return;
		}

		if (opcode == BATTMGR_XM_PROPERTY_GET ||
		    opcode == BATTMGR_XM_PROPERTY_SET) {
			if (kind == QCOM_BATTMGR_RESPONSE_SCALAR) {
				expected_len = sizeof(struct pmic_glink_hdr) +
					       sizeof(resp->intval);
				if (len != expected_len)
					return;
			} else if (kind == QCOM_BATTMGR_RESPONSE_XM_WORDS &&
				   opcode == BATTMGR_XM_PROPERTY_GET) {
				expected_len =
					sizeof(struct qcom_battmgr_xm_words_request);
				if (len != expected_len)
					return;
			} else if (kind == QCOM_BATTMGR_RESPONSE_XM_WORDS &&
				   opcode == BATTMGR_XM_PROPERTY_SET) {
				/* Stock accepts scalar, word, and digest-sized ACKs. */
				if (len != sizeof(struct pmic_glink_hdr) +
					   sizeof(resp->intval) &&
				    len != sizeof(struct qcom_battmgr_xm_words_request) &&
				    len != sizeof(struct pmic_glink_hdr) + 10 * sizeof(__le32))
					return;
			} else {
				return;
			}
		}

		if (opcode == BATTMGR_BAT_PROPERTY_GET ||
		    opcode == BATTMGR_USB_PROPERTY_GET ||
		    opcode == BATTMGR_WLS_PROPERTY_GET ||
		    opcode == BATTMGR_XM_PROPERTY_GET ||
		    opcode == BATTMGR_XM_PROPERTY_SET) {
			if (len < sizeof(struct pmic_glink_hdr) + sizeof(__le32))
				return;
			property = le32_to_cpu(resp->intval.property);
			if (property != READ_ONCE(battmgr->request_property)) {
				dev_warn_ratelimited(battmgr->dev,
						     "SM8650 drop opcode=%#x property=%d len=%zu waiting=%d\n",
						     opcode, property, len,
						     READ_ONCE(battmgr->request_property));
				return;
			}
		}

		qcom_battmgr_sm8650_callback(battmgr, data, len);
	} else if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
		   battmgr->variant == QCOM_BATTMGR_X1E80100) {
		qcom_battmgr_sc8280xp_callback(battmgr, data, len);
	} else {
		qcom_battmgr_sm8350_callback(battmgr, data, len);
	}
}

static void qcom_battmgr_enable_worker(struct work_struct *work)
{
	struct qcom_battmgr *battmgr = container_of(work, struct qcom_battmgr, enable_work);
	struct qcom_battmgr_enable_request req = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_NOTIFY),
		.hdr.opcode = cpu_to_le32(BATTMGR_REQUEST_NOTIFICATION),
	};
	int ret;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request(battmgr, &req, sizeof(req));
	mutex_unlock(&battmgr->lock);
	if (ret)
		dev_err(battmgr->dev, "failed to request power notifications\n");
}

static void qcom_battmgr_pdr_notify(void *priv, int state)
{
	struct qcom_battmgr *battmgr = priv;

	if (state == SERVREG_SERVICE_STATE_UP) {
		battmgr->service_up = true;
		schedule_work(&battmgr->enable_work);
	} else {
		battmgr->service_up = false;
		qcom_battmgr_sm8650_auth_invalidate(battmgr);
	}
}

static bool qcom_battmgr_sm8650_xm_value_valid(unsigned int property, u32 value)
{
	switch (property) {
	case XM_PROP_CONNECTOR_TEMP:
		return (s32)value >= -400 && (s32)value <= 1500;
	case XM_PROP_AUTHENTIC:
	case XM_PROP_BATTERY_ADAPT_POWER_MATCH:
	case XM_PROP_PD_VERIFIED:
		return value <= 1;
	case XM_PROP_UVDM_STATE:
		return value <= 9;
	case XM_PROP_APDO_MAX:
	case XM_PROP_POWER_MAX:
		return value <= 1000;
	default:
		return true;
	}
}

static int qcom_battmgr_sm8650_read_xm(struct qcom_battmgr *battmgr,
					    unsigned int property, u32 *value)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;
	if (property > XM_PROP_POWER_MAX)
		return -EINVAL;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_XM_PROPERTY_GET,
					   property, 0);
	if (!ret && !qcom_battmgr_sm8650_xm_value_valid(property,
						       battmgr->response_value)) {
		dev_warn_ratelimited(battmgr->dev,
				     "rejecting SM8650 XM property %u value %#x\n",
				     property, battmgr->response_value);
		ret = -ENODATA;
	}
	if (!ret) {
		battmgr->xm_cache[property] = battmgr->response_value;
		battmgr->xm_valid[property] = true;
		*value = battmgr->response_value;
	}
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_sm8650_read_usb(struct qcom_battmgr *battmgr,
					     unsigned int property)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					   property, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static ssize_t usb_adapter_type_raw_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	int ret;

	ret = qcom_battmgr_sm8650_read_usb(battmgr, USB_ADAP_TYPE);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", battmgr->usb.adapter_type_raw);
}
static DEVICE_ATTR_RO(usb_adapter_type_raw);

static ssize_t usb_real_type_raw_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	int ret;

	ret = qcom_battmgr_sm8650_read_usb(battmgr, USB_REAL_TYPE);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", battmgr->usb.real_type_raw);
}
static DEVICE_ATTR_RO(usb_real_type_raw);

static ssize_t typec_compliant_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	int ret;

	ret = qcom_battmgr_sm8650_read_usb(battmgr, USB_TYPEC_COMPLIANT);
	if (ret)
		return ret;
	if (battmgr->usb.typec_compliant > 1)
		return -ENODATA;

	return sysfs_emit(buf, "%u\n", battmgr->usb.typec_compliant);
}
static DEVICE_ATTR_RO(typec_compliant);

#define SM8650_XM_ATTR_DEC(_name, _property) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev); \
	u32 value; \
	int ret; \
	\
	ret = qcom_battmgr_sm8650_read_xm(battmgr, _property, &value); \
	if (ret) \
		return ret; \
	\
	return sysfs_emit(buf, "%u\n", value); \
} \
static DEVICE_ATTR_RO(_name)

static ssize_t connector_temp_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_CONNECTOR_TEMP,
					 &value);
	if (ret)
		return ret;

	/* Stock firmware reports connector temperature in tenths of Celsius. */
	return sysfs_emit(buf, "%d\n", (s32)value);
}
static DEVICE_ATTR_RO(connector_temp);

SM8650_XM_ATTR_DEC(authentic, XM_PROP_AUTHENTIC);
SM8650_XM_ATTR_DEC(battery_adapter_power_match,
		   XM_PROP_BATTERY_ADAPT_POWER_MATCH);
SM8650_XM_ATTR_DEC(pd_verified, XM_PROP_PD_VERIFIED);
SM8650_XM_ATTR_DEC(uvdm_state, XM_PROP_UVDM_STATE);
SM8650_XM_ATTR_DEC(apdo_max, XM_PROP_APDO_MAX);
SM8650_XM_ATTR_DEC(power_max, XM_PROP_POWER_MAX);

static ssize_t adapter_id_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_ADAPTER_ID, &value);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%08x\n", value);
}
static DEVICE_ATTR_RO(adapter_id);

static ssize_t adapter_svid_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_ADAPTER_SVID, &value);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%04x\n", value);
}
static DEVICE_ATTR_RO(adapter_svid);

static int qcom_battmgr_sm8650_write_xm(struct qcom_battmgr *battmgr,
					     unsigned int property, u32 value)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_XM_PROPERTY_SET,
					   property, value);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_sm8650_write_xm_words(struct qcom_battmgr *battmgr,
						   unsigned int property,
						   const u32 *words)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_xm_request_words(battmgr, BATTMGR_XM_PROPERTY_SET,
					   property, words);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_sm8650_read_xm_words(struct qcom_battmgr *battmgr,
						  unsigned int property,
						  u32 *words)
{
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_xm_request_words(battmgr, BATTMGR_XM_PROPERTY_GET,
					   property, NULL);
	if (!ret)
		memcpy(words, battmgr->response_words, sizeof(battmgr->response_words));
	memzero_explicit(battmgr->response_words,
			 sizeof(battmgr->response_words));
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_sm8650_parse_uvdm_words(const char *hex, u32 *words)
{
	int i, j;

	if (strlen(hex) != USBPD_UVDM_HEX_LEN)
		return -EINVAL;

	for (i = 0; i < USBPD_UVDM_WORDS; i++) {
		u32 word = 0;

		for (j = 0; j < 8; j++) {
			int nibble = hex_to_bin(hex[i * 8 + j]);

			if (nibble < 0)
				return -EINVAL;
			word = (word << 4) | nibble;
		}
		words[i] = word;
	}

	return 0;
}

static int qcom_battmgr_sm8650_auth_connected(struct qcom_battmgr *battmgr)
{
	int ret;

	ret = qcom_battmgr_sm8650_read_usb(battmgr, USB_ONLINE);
	if (ret)
		return ret;

	return battmgr->usb.online ? 0 : -ENOTCONN;
}

static int qcom_battmgr_sm8650_auth_prerequisites(struct qcom_battmgr *battmgr)
{
	u32 svid;
	int ret;

	ret = qcom_battmgr_sm8650_auth_connected(battmgr);
	if (ret)
		return ret;

	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_ADAPTER_SVID,
					 &svid);
	if (ret)
		return ret;
	if (svid != USBPD_XIAOMI_SVID)
		return -EACCES;

	return 0;
}

static bool qcom_battmgr_sm8650_auth_command_allowed(
	struct qcom_battmgr *battmgr, unsigned int cmd)
{
	enum qcom_battmgr_auth_stage stage = battmgr->auth_stage;

	if (!qcom_battmgr_sm8650_auth_live(battmgr))
		return false;

	switch (cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		return stage == QCOM_BATTMGR_AUTH_STARTED ||
		       stage == QCOM_BATTMGR_AUTH_VERSION;
	case USBPD_UVDM_CHARGER_VOLTAGE:
		return stage == QCOM_BATTMGR_AUTH_VERSION ||
		       stage == QCOM_BATTMGR_AUTH_VOLTAGE;
	case USBPD_UVDM_CHARGER_TEMP:
		return stage == QCOM_BATTMGR_AUTH_VOLTAGE ||
		       stage == QCOM_BATTMGR_AUTH_TEMPERATURE;
	case USBPD_UVDM_SESSION_SEED:
		return stage == QCOM_BATTMGR_AUTH_TEMPERATURE ||
		       stage == QCOM_BATTMGR_AUTH_SEED;
	case USBPD_UVDM_AUTHENTICATION:
		return stage == QCOM_BATTMGR_AUTH_SEED ||
		       stage == QCOM_BATTMGR_AUTH_CHALLENGE;
	case USBPD_UVDM_VERIFIED:
		return stage >= QCOM_BATTMGR_AUTH_RESPONSE &&
		       stage <= QCOM_BATTMGR_AUTH_REVERSE;
	case USBPD_UVDM_REVERSE_AUTHENTICATION:
		return stage == QCOM_BATTMGR_AUTH_VERIFIED ||
		       stage == QCOM_BATTMGR_AUTH_REVERSE;
	default:
		return false;
	}
}

static ssize_t uvdm_command_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	char input[USBPD_UVDM_HEX_LEN + 16];
	char *payload, *command;
	u32 words[USBPD_UVDM_WORDS] = {};
	unsigned int cmd, value = 0, property;
	enum qcom_battmgr_auth_stage next_stage;
	int ret;

	if (!count || count >= sizeof(input))
		return -E2BIG;
	memcpy(input, buf, count);
	input[count] = '\0';
	command = strim(input);
	payload = strchr(command, ' ');
	if (payload) {
		*payload++ = '\0';
		payload = skip_spaces(payload);
		if (!*payload || strchr(payload, ' '))
			return -EINVAL;
	}

	ret = kstrtouint(command, 10, &cmd);
	if (ret)
		return ret;

	switch (cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		property = XM_PROP_VDM_CHARGER_VERSION;
		next_stage = QCOM_BATTMGR_AUTH_VERSION;
		if (payload)
			return -EINVAL;
		break;
	case USBPD_UVDM_CHARGER_VOLTAGE:
		property = XM_PROP_VDM_CHARGER_VOLTAGE;
		next_stage = QCOM_BATTMGR_AUTH_VOLTAGE;
		if (payload)
			return -EINVAL;
		break;
	case USBPD_UVDM_CHARGER_TEMP:
		property = XM_PROP_VDM_CHARGER_TEMP;
		next_stage = QCOM_BATTMGR_AUTH_TEMPERATURE;
		if (payload)
			return -EINVAL;
		break;
	case USBPD_UVDM_SESSION_SEED:
		property = XM_PROP_VDM_SESSION_SEED;
		next_stage = QCOM_BATTMGR_AUTH_SEED;
		goto parse_words;
	case USBPD_UVDM_AUTHENTICATION:
		property = XM_PROP_VDM_AUTHENTICATION;
		next_stage = QCOM_BATTMGR_AUTH_CHALLENGE;
		goto parse_words;
	case USBPD_UVDM_REVERSE_AUTHENTICATION:
		property = XM_PROP_VDM_REVERSE_AUTHENTICATION;
		next_stage = QCOM_BATTMGR_AUTH_REVERSE;
parse_words:
		if (!payload)
			return -EINVAL;
		ret = qcom_battmgr_sm8650_parse_uvdm_words(payload, words);
		if (ret)
			return ret;
		break;
	case USBPD_UVDM_VERIFIED:
		property = XM_PROP_VDM_VERIFIED;
		next_stage = QCOM_BATTMGR_AUTH_VERIFIED;
		if (!payload)
			return -EINVAL;
		ret = kstrtouint(payload, 0, &value);
		if (ret)
			return ret;
		if (value > 1)
			return -ERANGE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&battmgr->auth_lock);
	if (!qcom_battmgr_sm8650_auth_command_allowed(battmgr, cmd)) {
		ret = -EPERM;
		goto out_unlock;
	}

	if (cmd == USBPD_UVDM_SESSION_SEED ||
	    cmd == USBPD_UVDM_AUTHENTICATION ||
	    cmd == USBPD_UVDM_REVERSE_AUTHENTICATION)
		ret = qcom_battmgr_sm8650_write_xm_words(battmgr, property,
							 words);
	else
		ret = qcom_battmgr_sm8650_write_xm(battmgr, property, value);

	if (ret) {
		battmgr->auth_stage = QCOM_BATTMGR_AUTH_IDLE;
		battmgr->auth_deadline = 0;
	} else if (cmd == USBPD_UVDM_VERIFIED && !value) {
		battmgr->auth_stage = QCOM_BATTMGR_AUTH_IDLE;
		battmgr->auth_deadline = 0;
	} else {
		qcom_battmgr_sm8650_auth_advance(battmgr, next_stage);
	}

out_unlock:
	mutex_unlock(&battmgr->auth_lock);
	memzero_explicit(words, sizeof(words));
	memzero_explicit(input, sizeof(input));
	return ret ? ret : count;
}

static ssize_t uvdm_command_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	u32 words[USBPD_UVDM_WORDS] = {}, value, state;
	unsigned int property;
	int i, ret, len;

	mutex_lock(&battmgr->auth_lock);
	if (!qcom_battmgr_sm8650_auth_live(battmgr)) {
		ret = -EPERM;
		goto out_unlock;
	}

	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_UVDM_STATE, &state);
	if (ret)
		goto out_invalidate;
	if (!state) {
		ret = -ENOTCONN;
		goto out_invalidate;
	}

	switch (state) {
	case USBPD_UVDM_CHARGER_VERSION:
		property = XM_PROP_VDM_CHARGER_VERSION;
		break;
	case USBPD_UVDM_CHARGER_VOLTAGE:
		property = XM_PROP_VDM_CHARGER_VOLTAGE;
		break;
	case USBPD_UVDM_CHARGER_TEMP:
		property = XM_PROP_VDM_CHARGER_TEMP;
		break;
	case USBPD_UVDM_AUTHENTICATION:
		if (battmgr->auth_stage != QCOM_BATTMGR_AUTH_CHALLENGE &&
		    battmgr->auth_stage != QCOM_BATTMGR_AUTH_RESPONSE) {
			ret = -EPERM;
			goto out_unlock;
		}
		ret = qcom_battmgr_sm8650_read_xm_words(battmgr,
						 XM_PROP_VDM_AUTHENTICATION,
						 words);
		if (ret)
			goto out_invalidate;
		qcom_battmgr_sm8650_auth_advance(battmgr,
						 QCOM_BATTMGR_AUTH_RESPONSE);
		len = sysfs_emit(buf, "%u,", state);
		for (i = 0; i < USBPD_UVDM_WORDS; i++)
			len += sysfs_emit_at(buf, len, "%08x", words[i]);
		len += sysfs_emit_at(buf, len, "\n");
		ret = len;
		goto out_unlock;
	default:
		ret = sysfs_emit(buf, "%u,Null\n", state);
		goto out_unlock;
	}

	ret = qcom_battmgr_sm8650_read_xm(battmgr, property, &value);
	if (ret)
		goto out_invalidate;
	ret = sysfs_emit(buf, "%u,%u\n", state, value);
	goto out_unlock;

out_invalidate:
	battmgr->auth_stage = QCOM_BATTMGR_AUTH_IDLE;
	battmgr->auth_deadline = 0;
out_unlock:
	mutex_unlock(&battmgr->auth_lock);
	memzero_explicit(words, sizeof(words));
	return ret;
}
static struct device_attribute dev_attr_uvdm_command =
	__ATTR(uvdm_command, 0600, uvdm_command_show, uvdm_command_store);

static ssize_t verify_process_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	bool value;
	int clear_ret = 0, ret;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;

	/* verify_process starts Xiaomi identity discovery; SVID appears after it. */
	if (value) {
		ret = qcom_battmgr_sm8650_auth_connected(battmgr);
		if (ret)
			return ret;
	}

	mutex_lock(&battmgr->auth_lock);
	if (value && battmgr->auth_stage != QCOM_BATTMGR_AUTH_IDLE) {
		ret = -EBUSY;
		goto out_unlock;
	}

	if (value || battmgr->auth_stage != QCOM_BATTMGR_AUTH_COMMITTED)
		clear_ret = qcom_battmgr_sm8650_write_xm(battmgr,
							 XM_PROP_PD_VERIFIED, 0);
	ret = qcom_battmgr_sm8650_write_xm(battmgr, XM_PROP_VERIFY_PROCESS,
						 value);
	if (value && !clear_ret && !ret) {
		battmgr->auth_stage = QCOM_BATTMGR_AUTH_STARTED;
		battmgr->auth_deadline = jiffies + USBPD_AUTH_SESSION_TIMEOUT;
	} else {
		battmgr->auth_stage = QCOM_BATTMGR_AUTH_IDLE;
		battmgr->auth_deadline = 0;
	}
	if (!ret)
		ret = clear_ret;

out_unlock:
	mutex_unlock(&battmgr->auth_lock);
	return ret ? ret : count;
}
static struct device_attribute dev_attr_verify_process =
	__ATTR(verify_process, 0200, NULL, verify_process_store);

static ssize_t pd_auth_result_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	bool value;
	int ret;

	ret = kstrtobool(buf, &value);
	if (ret)
		return ret;

	mutex_lock(&battmgr->auth_lock);
	if (!value) {
		ret = qcom_battmgr_sm8650_write_xm(battmgr,
						    XM_PROP_PD_VERIFIED, 0);
		battmgr->auth_stage = QCOM_BATTMGR_AUTH_IDLE;
		battmgr->auth_deadline = 0;
		goto out_unlock;
	}

	if (!qcom_battmgr_sm8650_auth_live(battmgr) ||
	    (battmgr->auth_stage != QCOM_BATTMGR_AUTH_VERIFIED &&
	     battmgr->auth_stage != QCOM_BATTMGR_AUTH_REVERSE)) {
		ret = -EPERM;
		goto out_unlock;
	}

	ret = qcom_battmgr_sm8650_auth_prerequisites(battmgr);
	if (ret)
		goto out_invalidate;
	if (!qcom_battmgr_sm8650_auth_live(battmgr)) {
		ret = -ENOTCONN;
		goto out_unlock;
	}
	ret = qcom_battmgr_sm8650_write_xm(battmgr, XM_PROP_PD_VERIFIED, 1);
	if (ret)
		goto out_invalidate;

	/* Serialize transport calls; no callback can invalidate after this SET. */
	battmgr->auth_stage = QCOM_BATTMGR_AUTH_COMMITTED;
	battmgr->auth_deadline = 0;
	goto out_unlock;

out_invalidate:
	battmgr->auth_stage = QCOM_BATTMGR_AUTH_IDLE;
	battmgr->auth_deadline = 0;
out_unlock:
	mutex_unlock(&battmgr->auth_lock);
	return ret ? ret : count;
}
static struct device_attribute dev_attr_pd_auth_result =
	__ATTR(pd_auth_result, 0200, NULL, pd_auth_result_store);

static int qcom_battmgr_sm8650_quick_charge_type(struct qcom_battmgr *battmgr,
						  u32 *result)
{
	u32 real_type, pd_verified, power_max, authentic, power_match;
	u32 connector_temp;
	int ret;

	ret = qcom_battmgr_bat_sm8350_update(battmgr, POWER_SUPPLY_PROP_HEALTH);
	if (ret)
		return ret;
	ret = qcom_battmgr_bat_sm8350_update(battmgr, POWER_SUPPLY_PROP_STATUS);
	if (ret)
		return ret;
	ret = qcom_battmgr_sm8650_read_usb(battmgr, USB_REAL_TYPE);
	if (ret)
		return ret;
	real_type = battmgr->usb.real_type_raw;

	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_PD_VERIFIED,
						 &pd_verified);
	if (ret)
		return ret;
	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_POWER_MAX, &power_max);
	if (ret)
		return ret;
	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_AUTHENTIC, &authentic);
	if (ret)
		return ret;
	ret = qcom_battmgr_sm8650_read_xm(battmgr,
					 XM_PROP_BATTERY_ADAPT_POWER_MATCH,
					 &power_match);
	if (ret)
		return ret;
	ret = qcom_battmgr_sm8650_read_xm(battmgr, XM_PROP_CONNECTOR_TEMP,
					 &connector_temp);
	if (ret)
		return ret;

	*result = 0;
	if (battmgr->status.health == POWER_SUPPLY_HEALTH_COLD ||
	    battmgr->status.health == POWER_SUPPLY_HEALTH_HOT || !authentic ||
	    !power_match || ((s32)connector_temp > 600 &&
	    battmgr->status.status == POWER_SUPPLY_STATUS_DISCHARGING))
		return 0;

	if (real_type == POWER_SUPPLY_USB_TYPE_PD_PPS) {
		if (!pd_verified)
			*result = 1;
		else if (power_max >= 50)
			*result = 4;
		else
			*result = 3;
	} else if (real_type == POWER_SUPPLY_USB_TYPE_PD ||
		   real_type == QTI_USB_TYPE_HVDCP ||
		   real_type == QTI_USB_TYPE_HVDCP_3) {
		*result = 1;
	} else if (real_type == QTI_USB_TYPE_HVDCP_3P5 ||
		   real_type == QTI_USB_TYPE_HVDCP_3_CLASSB) {
		*result = 2;
	}

	return 0;
}

static ssize_t quick_charge_type_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(dev);
	u32 value;
	int ret;

	ret = qcom_battmgr_sm8650_quick_charge_type(battmgr, &value);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", value);
}
static DEVICE_ATTR_RO(quick_charge_type);

static struct attribute *qcom_battmgr_sm8650_attrs[] = {
	&dev_attr_usb_adapter_type_raw.attr,
	&dev_attr_usb_real_type_raw.attr,
	&dev_attr_typec_compliant.attr,
	&dev_attr_connector_temp.attr,
	&dev_attr_authentic.attr,
	&dev_attr_battery_adapter_power_match.attr,
	&dev_attr_adapter_id.attr,
	&dev_attr_adapter_svid.attr,
	&dev_attr_pd_verified.attr,
	&dev_attr_uvdm_state.attr,
	&dev_attr_apdo_max.attr,
	&dev_attr_power_max.attr,
	&dev_attr_quick_charge_type.attr,
	&dev_attr_uvdm_command.attr,
	&dev_attr_verify_process.attr,
	&dev_attr_pd_auth_result.attr,
	NULL,
};

static const struct attribute_group qcom_battmgr_sm8650_group = {
	.name = "charger_diagnostics",
	.attrs = qcom_battmgr_sm8650_attrs,
};

static const struct of_device_id qcom_battmgr_of_variants[] = {
	{ .compatible = "qcom,glymur-pmic-glink", .data = (void *)QCOM_BATTMGR_X1E80100 },
	{ .compatible = "qcom,kaanapali-pmic-glink", .data = (void *)QCOM_BATTMGR_SM8550 },
	{ .compatible = "qcom,sc8180x-pmic-glink", .data = (void *)QCOM_BATTMGR_SC8280XP },
	{ .compatible = "qcom,sc8280xp-pmic-glink", .data = (void *)QCOM_BATTMGR_SC8280XP },
	{ .compatible = "qcom,sm8650-pmic-glink", .data = (void *)QCOM_BATTMGR_SM8650 },
	{ .compatible = "qcom,sm8550-pmic-glink", .data = (void *)QCOM_BATTMGR_SM8550 },
	{ .compatible = "qcom,x1e80100-pmic-glink", .data = (void *)QCOM_BATTMGR_X1E80100 },
	/* Unmatched devices falls back to QCOM_BATTMGR_SM8350 */
	{}
};

static char *qcom_battmgr_battery[] = { "battery" };

static int qcom_battmgr_probe(struct auxiliary_device *adev,
			      const struct auxiliary_device_id *id)
{
	const struct power_supply_desc *psy_desc;
	struct power_supply_config psy_cfg_supply = {};
	struct power_supply_config psy_cfg = {};
	const struct of_device_id *match;
	struct qcom_battmgr *battmgr;
	struct device *dev = &adev->dev;
	int ret;

	battmgr = devm_kzalloc(dev, sizeof(*battmgr), GFP_KERNEL);
	if (!battmgr)
		return -ENOMEM;

	battmgr->dev = dev;
	dev_set_drvdata(dev, battmgr);

	psy_cfg.drv_data = battmgr;
	psy_cfg.fwnode = dev_fwnode(&adev->dev);

	psy_cfg_supply.drv_data = battmgr;
	psy_cfg_supply.fwnode = dev_fwnode(&adev->dev);
	psy_cfg_supply.supplied_to = qcom_battmgr_battery;
	psy_cfg_supply.num_supplicants = 1;

	mutex_init(&battmgr->lock);
	mutex_init(&battmgr->auth_lock);
	init_completion(&battmgr->ack);
	battmgr->request_opcode = -1;
	battmgr->request_property = -1;
	battmgr->request_kind = QCOM_BATTMGR_RESPONSE_NONE;

	match = of_match_device(qcom_battmgr_of_variants, dev->parent);
	if (match)
		battmgr->variant = (unsigned long)match->data;
	else
		battmgr->variant = QCOM_BATTMGR_SM8350;

	if (battmgr->variant == QCOM_BATTMGR_SM8650)
		battmgr->status.percent = (unsigned int)-1;

	ret = qcom_battmgr_charge_control_thresholds_init(battmgr);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to init battery charge control thresholds\n");

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
	    battmgr->variant == QCOM_BATTMGR_X1E80100) {
		if (battmgr->variant == QCOM_BATTMGR_X1E80100)
			psy_desc = &x1e80100_bat_psy_desc;
		else
			psy_desc = &sc8280xp_bat_psy_desc;

		battmgr->bat_psy = devm_power_supply_register(dev, psy_desc, &psy_cfg);
		if (IS_ERR(battmgr->bat_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->bat_psy),
					     "failed to register battery power supply\n");

		battmgr->ac_psy = devm_power_supply_register(dev, &sc8280xp_ac_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->ac_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->ac_psy),
					     "failed to register AC power supply\n");

		battmgr->usb_psy = devm_power_supply_register(dev, &sc8280xp_usb_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->usb_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->usb_psy),
					     "failed to register USB power supply\n");

		battmgr->wls_psy = devm_power_supply_register(dev, &sc8280xp_wls_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->wls_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->wls_psy),
					     "failed to register wireless charing power supply\n");
	} else {
		if (battmgr->variant == QCOM_BATTMGR_SM8650)
			psy_desc = &sm8650_bat_psy_desc;
		else if (battmgr->variant == QCOM_BATTMGR_SM8550)
			psy_desc = &sm8550_bat_psy_desc;
		else
			psy_desc = &sm8350_bat_psy_desc;

		battmgr->bat_psy = devm_power_supply_register(dev, psy_desc, &psy_cfg);
		if (IS_ERR(battmgr->bat_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->bat_psy),
					     "failed to register battery power supply\n");

		if (battmgr->variant == QCOM_BATTMGR_SM8650)
			psy_desc = &sm8650_usb_psy_desc;
		else
			psy_desc = &sm8350_usb_psy_desc;

		battmgr->usb_psy = devm_power_supply_register(dev, psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->usb_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->usb_psy),
					     "failed to register USB power supply\n");

		if (battmgr->variant == QCOM_BATTMGR_SM8650)
			psy_desc = &sm8650_wls_psy_desc;
		else
			psy_desc = &sm8350_wls_psy_desc;

		battmgr->wls_psy = devm_power_supply_register(dev, psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->wls_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->wls_psy),
					     "failed to register wireless charing power supply\n");
	}

	ret = devm_work_autocancel(dev, &battmgr->enable_work,
				   qcom_battmgr_enable_worker);
	if (ret)
		return ret;

	battmgr->client = devm_pmic_glink_client_alloc(dev, PMIC_GLINK_OWNER_BATTMGR,
						       qcom_battmgr_callback,
						       qcom_battmgr_pdr_notify,
						       battmgr);
	if (IS_ERR(battmgr->client))
		return PTR_ERR(battmgr->client);

	pmic_glink_client_register(battmgr->client);

	if (battmgr->variant == QCOM_BATTMGR_SM8650) {
		ret = devm_device_add_group(dev, &qcom_battmgr_sm8650_group);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to add charger diagnostics\n");
	}

	return 0;
}

static const struct auxiliary_device_id qcom_battmgr_id_table[] = {
	{ .name = "pmic_glink.power-supply", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, qcom_battmgr_id_table);

static struct auxiliary_driver qcom_battmgr_driver = {
	.name = "pmic_glink_power_supply",
	.probe = qcom_battmgr_probe,
	.id_table = qcom_battmgr_id_table,
};

module_auxiliary_driver(qcom_battmgr_driver);

MODULE_DESCRIPTION("Qualcomm PMIC GLINK battery manager driver");
MODULE_LICENSE("GPL");
