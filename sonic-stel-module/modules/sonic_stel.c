// SPDX-License-Identifier: GPL-2.0
/*
 * sonic_stel - Minimal genetlink kernel module for SONiC Stream Telemetry
 *
 * Registers genetlink family "sonic_stel" with multicast group "ipfix",
 * enabling countersyncd to subscribe and receive IPFIX data in software-switch
 * environments.
 *
 * Userspace (SAIVPP) writes IPFIX data via the netlink command interface,
 * and this module multicasts it to all subscribers.
 *
 * Usage from userspace:
 *   1. Open NETLINK_GENERIC socket
 *   2. Resolve family "sonic_stel" via CTRL_CMD_GETFAMILY
 *   3. Send GENL_CMD_SEND_IPFIX command with NLA attr containing IPFIX data
 *   4. Module multicasts the data to "ipfix" group subscribers (countersyncd)
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <net/genetlink.h>

#define SONIC_STEL_FAMILY_NAME  "sonic_stel"
#define SONIC_STEL_VERSION      1
#define SONIC_STEL_MCGRP_NAME   "ipfix"
#define SONIC_STEL_MAX_IPFIX_LEN 65535
#define SONIC_STEL_BUFFER_BYTES  (4 * 1024 * 1024)
#define SONIC_STEL_BUFFER_RECORDS 1024

/* Genetlink commands */
enum {
	SONIC_STEL_CMD_UNSPEC,
	SONIC_STEL_CMD_SEND_IPFIX,  /* Userspace -> kernel -> multicast */
	__SONIC_STEL_CMD_MAX,
};
#define SONIC_STEL_CMD_MAX (__SONIC_STEL_CMD_MAX - 1)

/* Netlink attributes */
enum {
	SONIC_STEL_ATTR_UNSPEC,
	SONIC_STEL_ATTR_IPFIX_DATA,  /* Binary IPFIX payload */
	__SONIC_STEL_ATTR_MAX,
};
#define SONIC_STEL_ATTR_MAX (__SONIC_STEL_ATTR_MAX - 1)

/* Multicast group */
static const struct genl_multicast_group sonic_stel_mcgrps[] = {
	{
		.name = SONIC_STEL_MCGRP_NAME,
		.flags = GENL_MCAST_CAP_NET_ADMIN,
	},
};

/* Attribute policy */
static const struct nla_policy sonic_stel_policy[SONIC_STEL_ATTR_MAX + 1] = {
	[SONIC_STEL_ATTR_IPFIX_DATA] = {
		.type = NLA_BINARY,
		.len = SONIC_STEL_MAX_IPFIX_LEN,
	},
};

struct sonic_stel_record {
	struct list_head node;
	size_t len;
	u8 data[];
};

static LIST_HEAD(sonic_stel_buffer);
static DEFINE_MUTEX(sonic_stel_buffer_lock);
static size_t sonic_stel_buffer_bytes;
static size_t sonic_stel_buffer_records;

/* Forward declaration */
static int sonic_stel_cmd_send_ipfix(struct sk_buff *skb, struct genl_info *info);

/* Family definition - forward declared for ops */
static struct genl_family sonic_stel_family;

/* Operations */
static const struct genl_ops sonic_stel_ops[] = {
	{
		.cmd    = SONIC_STEL_CMD_SEND_IPFIX,
		.flags  = GENL_ADMIN_PERM,
		.doit   = sonic_stel_cmd_send_ipfix,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 2, 0)
		.policy = sonic_stel_policy,
#endif
	},
};

static int sonic_stel_multicast(const void *data, size_t len)
{
	struct sk_buff *skb_out;
	void *hdr;

	skb_out = genlmsg_new(len, GFP_KERNEL);
	if (!skb_out)
		return -ENOMEM;

	hdr = genlmsg_put(skb_out, 0, 0, &sonic_stel_family,
			  0, SONIC_STEL_CMD_SEND_IPFIX);
	if (!hdr) {
		nlmsg_free(skb_out);
		return -EMSGSIZE;
	}

	/* countersyncd expects raw IPFIX immediately after the genetlink header. */
	if (nla_append(skb_out, len, data) < 0) {
		nlmsg_free(skb_out);
		return -EMSGSIZE;
	}

	genlmsg_end(skb_out, hdr);
	return genlmsg_multicast(&sonic_stel_family, skb_out, 0, 0, GFP_KERNEL);
}

static void sonic_stel_drop_oldest(void)
{
	struct sonic_stel_record *record;

	if (list_empty(&sonic_stel_buffer))
		return;

	record = list_first_entry(&sonic_stel_buffer,
				  struct sonic_stel_record, node);
	list_del(&record->node);
	sonic_stel_buffer_bytes -= record->len;
	--sonic_stel_buffer_records;
	kfree(record);
}

static int sonic_stel_buffer_record(const void *data, size_t len)
{
	struct sonic_stel_record *record;

	if (len > SONIC_STEL_BUFFER_BYTES)
		return -EMSGSIZE;

	while (sonic_stel_buffer_records >= SONIC_STEL_BUFFER_RECORDS ||
	       sonic_stel_buffer_bytes + len > SONIC_STEL_BUFFER_BYTES) {
		sonic_stel_drop_oldest();
		pr_warn_ratelimited("sonic_stel: telemetry buffer full, dropping oldest record\n");
	}

	record = kmalloc(struct_size(record, data, len), GFP_KERNEL);
	if (!record)
		return -ENOMEM;

	record->len = len;
	memcpy(record->data, data, len);
	list_add_tail(&record->node, &sonic_stel_buffer);
	sonic_stel_buffer_bytes += len;
	++sonic_stel_buffer_records;
	return 0;
}

static int sonic_stel_flush_buffer(void)
{
	struct sonic_stel_record *record;
	struct sonic_stel_record *next;
	int ret = 0;

	list_for_each_entry_safe(record, next, &sonic_stel_buffer, node) {
		ret = sonic_stel_multicast(record->data, record->len);
		if (ret)
			break;

		list_del(&record->node);
		sonic_stel_buffer_bytes -= record->len;
		--sonic_stel_buffer_records;
		kfree(record);
	}

	return ret;
}

/*
 * Relay one IPFIX message to countersyncd. If no listener is present, retain a
 * bounded FIFO and retry it before the next live record. The caller and
 * subscriber both require CAP_NET_ADMIN through the family definitions above.
 */
static int sonic_stel_cmd_send_ipfix(struct sk_buff *skb, struct genl_info *info)
{
	const void *ipfix_data;
	size_t ipfix_len;
	int ret;

	if (!info->attrs[SONIC_STEL_ATTR_IPFIX_DATA])
		return -EINVAL;

	ipfix_data = nla_data(info->attrs[SONIC_STEL_ATTR_IPFIX_DATA]);
	ipfix_len = nla_len(info->attrs[SONIC_STEL_ATTR_IPFIX_DATA]);
	if (ipfix_len < 16 || ipfix_len > SONIC_STEL_MAX_IPFIX_LEN)
		return -EMSGSIZE;

	mutex_lock(&sonic_stel_buffer_lock);
	sonic_stel_flush_buffer();

	if (list_empty(&sonic_stel_buffer)) {
		ret = sonic_stel_multicast(ipfix_data, ipfix_len);
		if (!ret)
			goto unlock;
	}

	ret = sonic_stel_buffer_record(ipfix_data, ipfix_len);

unlock:
	mutex_unlock(&sonic_stel_buffer_lock);
	return ret;
}

/* Family definition */
static struct genl_family sonic_stel_family = {
	.name       = SONIC_STEL_FAMILY_NAME,
	.version    = SONIC_STEL_VERSION,
	.maxattr    = SONIC_STEL_ATTR_MAX,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
	.policy     = sonic_stel_policy,
#endif
	.module     = THIS_MODULE,
	.ops        = sonic_stel_ops,
	.n_ops      = ARRAY_SIZE(sonic_stel_ops),
	.mcgrps     = sonic_stel_mcgrps,
	.n_mcgrps   = ARRAY_SIZE(sonic_stel_mcgrps),
};

static int __init sonic_stel_init(void)
{
	int ret;

	ret = genl_register_family(&sonic_stel_family);
	if (ret) {
		pr_err("sonic_stel: failed to register genetlink family: %d\n", ret);
		return ret;
	}

	pr_info("sonic_stel: registered genetlink family '%s' with multicast group '%s'\n",
		SONIC_STEL_FAMILY_NAME, SONIC_STEL_MCGRP_NAME);
	return 0;
}

static void __exit sonic_stel_exit(void)
{
	genl_unregister_family(&sonic_stel_family);

	mutex_lock(&sonic_stel_buffer_lock);
	while (!list_empty(&sonic_stel_buffer))
		sonic_stel_drop_oldest();
	mutex_unlock(&sonic_stel_buffer_lock);

	pr_info("sonic_stel: unregistered genetlink family\n");
}

module_init(sonic_stel_init);
module_exit(sonic_stel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SONiC");
MODULE_DESCRIPTION("Minimal genetlink module for SONiC Stream Telemetry (SAIVPP)");
