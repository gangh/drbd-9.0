/*
   drbd_nl.c

   This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

   Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
   Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
   Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

   drbd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   drbd is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with drbd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/drbd.h>
#include <linux/in.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/blkpg.h>
#include <linux/cpumask.h>
#include "drbd_int.h"
#include "drbd_protocol.h"
#include "drbd_req.h"
#include <asm/unaligned.h>
#include <linux/drbd_limits.h>
#include <linux/kthread.h>

#include <net/genetlink.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,31)
/*
 * copied from more recent kernel source
 */
int genl_register_family_with_ops(struct genl_family *family,
	struct genl_ops *ops, size_t n_ops)
{
	int err, i;

	err = genl_register_family(family);
	if (err)
		return err;

	for (i = 0; i < n_ops; ++i, ++ops) {
		err = genl_register_ops(family, ops);
		if (err)
			goto err_out;
	}
	return 0;
err_out:
	genl_unregister_family(family);
	return err;
}
#endif

/* .doit */
// int drbd_adm_create_resource(struct sk_buff *skb, struct genl_info *info);
// int drbd_adm_delete_resource(struct sk_buff *skb, struct genl_info *info);

int drbd_adm_new_minor(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_del_minor(struct sk_buff *skb, struct genl_info *info);

int drbd_adm_new_resource(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_del_resource(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_down(struct sk_buff *skb, struct genl_info *info);

int drbd_adm_set_role(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_attach(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_disk_opts(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_detach(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_connect(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_net_opts(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_resize(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_start_ov(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_new_c_uuid(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_disconnect(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_invalidate(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_invalidate_peer(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_pause_sync(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_resume_sync(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_suspend_io(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_resume_io(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_outdate(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_resource_opts(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_get_status(struct sk_buff *skb, struct genl_info *info);
int drbd_adm_get_timeout_type(struct sk_buff *skb, struct genl_info *info);
/* .dumpit */
int drbd_adm_get_status_all(struct sk_buff *skb, struct netlink_callback *cb);

#include <linux/drbd_genl_api.h>
#include "drbd_nla.h"
#include <linux/genl_magic_func.h>

/* used blkdev_get_by_path, to claim our meta data device(s) */
static char *drbd_m_holder = "Hands off! this is DRBD's meta data device.";

/* Configuration is strictly serialized, because generic netlink message
 * processing is strictly serialized by the genl_lock().
 * Which means we can use one static global drbd_config_context struct.
 */
static struct drbd_config_context {
	/* assigned from drbd_genlmsghdr */
	unsigned int minor;
	/* assigned from request attributes, if present */
	unsigned int volume;
#define VOLUME_UNSPECIFIED		(-1U)
	/* pointer into the request skb,
	 * limited lifetime! */
	char *resource_name;
	struct nlattr *my_addr;
	struct nlattr *peer_addr;

	/* reply buffer */
	struct sk_buff *reply_skb;
	/* pointer into reply buffer */
	struct drbd_genlmsghdr *reply_dh;
	/* resolved from attributes, if possible */
	struct drbd_device *device;
	struct drbd_connection *connection;
} adm_ctx;

static void drbd_adm_send_reply(struct sk_buff *skb, struct genl_info *info)
{
	genlmsg_end(skb, genlmsg_data(nlmsg_data(nlmsg_hdr(skb))));
	if (genlmsg_reply(skb, info))
		printk(KERN_ERR "drbd: error sending genl reply\n");
}

/* Used on a fresh "drbd_adm_prepare"d reply_skb, this cannot fail: The only
 * reason it could fail was no space in skb, and there are 4k available. */
int drbd_msg_put_info(const char *info)
{
	struct sk_buff *skb = adm_ctx.reply_skb;
	struct nlattr *nla;
	int err = -EMSGSIZE;

	if (!info || !info[0])
		return 0;

	nla = nla_nest_start(skb, DRBD_NLA_CFG_REPLY);
	if (!nla)
		return err;

	err = nla_put_string(skb, T_info_text, info);
	if (err) {
		nla_nest_cancel(skb, nla);
		return err;
	} else
		nla_nest_end(skb, nla);
	return 0;
}

/* This would be a good candidate for a "pre_doit" hook,
 * and per-family private info->pointers.
 * But we need to stay compatible with older kernels.
 * If it returns successfully, adm_ctx members are valid.
 */
#define DRBD_ADM_NEED_MINOR	1
#define DRBD_ADM_NEED_RESOURCE	2
#define DRBD_ADM_NEED_CONNECTION 4
static int drbd_adm_prepare(struct sk_buff *skb, struct genl_info *info,
		unsigned flags)
{
	struct drbd_genlmsghdr *d_in = info->userhdr;
	const u8 cmd = info->genlhdr->cmd;
	int err;

	memset(&adm_ctx, 0, sizeof(adm_ctx));

	/* genl_rcv_msg only checks for CAP_NET_ADMIN on "GENL_ADMIN_PERM" :( */
	if (cmd != DRBD_ADM_GET_STATUS
	&& security_netlink_recv(skb, CAP_SYS_ADMIN))
	       return -EPERM;

	adm_ctx.reply_skb = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!adm_ctx.reply_skb) {
		err = -ENOMEM;
		goto fail;
	}

	adm_ctx.reply_dh = genlmsg_put_reply(adm_ctx.reply_skb,
					info, &drbd_genl_family, 0, cmd);
	/* put of a few bytes into a fresh skb of >= 4k will always succeed.
	 * but anyways */
	if (!adm_ctx.reply_dh) {
		err = -ENOMEM;
		goto fail;
	}

	adm_ctx.reply_dh->minor = d_in->minor;
	adm_ctx.reply_dh->ret_code = NO_ERROR;

	adm_ctx.volume = VOLUME_UNSPECIFIED;
	if (info->attrs[DRBD_NLA_CFG_CONTEXT]) {
		struct nlattr *nla;
		/* parse and validate only */
		err = drbd_cfg_context_from_attrs(NULL, info);
		if (err)
			goto fail;

		/* It was present, and valid,
		 * copy it over to the reply skb. */
		err = nla_put_nohdr(adm_ctx.reply_skb,
				info->attrs[DRBD_NLA_CFG_CONTEXT]->nla_len,
				info->attrs[DRBD_NLA_CFG_CONTEXT]);
		if (err)
			goto fail;

		/* and assign stuff to the global adm_ctx */
		nla = nested_attr_tb[__nla_type(T_ctx_volume)];
		if (nla)
			adm_ctx.volume = nla_get_u32(nla);
		nla = nested_attr_tb[__nla_type(T_ctx_resource_name)];
		if (nla)
			adm_ctx.resource_name = nla_data(nla);
		adm_ctx.my_addr = nested_attr_tb[__nla_type(T_ctx_my_addr)];
		adm_ctx.peer_addr = nested_attr_tb[__nla_type(T_ctx_peer_addr)];
		if ((adm_ctx.my_addr &&
		     nla_len(adm_ctx.my_addr) > sizeof(adm_ctx.connection->my_addr)) ||
		    (adm_ctx.peer_addr &&
		     nla_len(adm_ctx.peer_addr) > sizeof(adm_ctx.connection->peer_addr))) {
			err = -EINVAL;
			goto fail;
		}
	}

	adm_ctx.minor = d_in->minor;
	adm_ctx.device = minor_to_mdev(d_in->minor);
	adm_ctx.connection = conn_get_by_name(adm_ctx.resource_name);

	if (!adm_ctx.device && (flags & DRBD_ADM_NEED_MINOR)) {
		drbd_msg_put_info("unknown minor");
		return ERR_MINOR_INVALID;
	}
	if (!adm_ctx.connection && (flags & DRBD_ADM_NEED_RESOURCE)) {
		drbd_msg_put_info("unknown resource");
		if (adm_ctx.resource_name)
			return ERR_RES_NOT_KNOWN;
		return ERR_INVALID_REQUEST;
	}

	if (flags & DRBD_ADM_NEED_CONNECTION) {
		if (adm_ctx.connection && !(flags & DRBD_ADM_NEED_RESOURCE)) {
			drbd_msg_put_info("no resource name expected");
			return ERR_INVALID_REQUEST;
		}
		if (adm_ctx.device) {
			drbd_msg_put_info("no minor number expected");
			return ERR_INVALID_REQUEST;
		}
		if (adm_ctx.my_addr && adm_ctx.peer_addr)
			adm_ctx.connection = conn_get_by_addrs(
				nla_data(adm_ctx.my_addr), nla_len(adm_ctx.my_addr),
				nla_data(adm_ctx.peer_addr), nla_len(adm_ctx.peer_addr));
		if (!adm_ctx.connection) {
			drbd_msg_put_info("unknown connection");
			return ERR_INVALID_REQUEST;
		}
	}

	/* some more paranoia, if the request was over-determined */
	if (adm_ctx.device && adm_ctx.connection &&
	    first_peer_device(adm_ctx.device)->connection != adm_ctx.connection) {
		pr_warning("request: minor=%u, resource=%s; but that minor belongs to connection %s\n",
				adm_ctx.minor, adm_ctx.resource_name,
				adm_ctx.device->resource->name);
		drbd_msg_put_info("minor exists in different resource");
		return ERR_INVALID_REQUEST;
	}
	if (adm_ctx.device &&
	    adm_ctx.volume != VOLUME_UNSPECIFIED &&
	    adm_ctx.volume != adm_ctx.device->vnr) {
		pr_warning("request: minor=%u, volume=%u; but that minor is volume %u in %s\n",
				adm_ctx.minor, adm_ctx.volume,
				adm_ctx.device->vnr,
				adm_ctx.device->resource->name);
		drbd_msg_put_info("minor exists as different volume");
		return ERR_INVALID_REQUEST;
	}

	return NO_ERROR;

fail:
	nlmsg_free(adm_ctx.reply_skb);
	adm_ctx.reply_skb = NULL;
	return err;
}

static int drbd_adm_finish(struct genl_info *info, int retcode)
{
	if (adm_ctx.connection) {
		kref_put(&adm_ctx.connection->kref, drbd_destroy_connection);
		adm_ctx.connection = NULL;
	}

	if (!adm_ctx.reply_skb)
		return -ENOMEM;

	adm_ctx.reply_dh->ret_code = retcode;
	drbd_adm_send_reply(adm_ctx.reply_skb, info);
	return 0;
}

static void setup_khelper_env(struct drbd_connection *connection, char **envp)
{
	char *afs;

	/* FIXME: A future version will not allow this case. */
	if (connection->my_addr_len == 0 || connection->peer_addr_len == 0)
		return;

	switch (((struct sockaddr *)&connection->peer_addr)->sa_family) {
	case AF_INET6:
		afs = "ipv6";
		snprintf(envp[4], 60, "DRBD_PEER_ADDRESS=%pI6",
			 &((struct sockaddr_in6 *)&connection->peer_addr)->sin6_addr);
		break;
	case AF_INET:
		afs = "ipv4";
		snprintf(envp[4], 60, "DRBD_PEER_ADDRESS=%pI4",
			 &((struct sockaddr_in *)&connection->peer_addr)->sin_addr);
		break;
	default:
		afs = "ssocks";
		snprintf(envp[4], 60, "DRBD_PEER_ADDRESS=%pI4",
			 &((struct sockaddr_in *)&connection->peer_addr)->sin_addr);
	}
	snprintf(envp[3], 20, "DRBD_PEER_AF=%s", afs);
}

int drbd_khelper(struct drbd_device *device, char *cmd)
{
	char *envp[] = { "HOME=/",
			"TERM=linux",
			"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
			 (char[20]) { }, /* address family */
			 (char[60]) { }, /* address */
			NULL };
	char mb[12];
	char *argv[] = {usermode_helper, cmd, mb, NULL };
	struct sib_info sib;
	int ret;

	snprintf(mb, 12, "minor-%d", mdev_to_minor(device));
	setup_khelper_env(first_peer_device(device)->connection, envp);

	/* The helper may take some time.
	 * write out any unsynced meta data changes now */
	drbd_md_sync(device);

	dev_info(DEV, "helper command: %s %s %s\n", usermode_helper, cmd, mb);
	sib.sib_reason = SIB_HELPER_PRE;
	sib.helper_name = cmd;
	drbd_bcast_event(device, &sib);
	ret = call_usermodehelper(usermode_helper, argv, envp, 1);
	if (ret)
		dev_warn(DEV, "helper command: %s %s %s exit code %u (0x%x)\n",
				usermode_helper, cmd, mb,
				(ret >> 8) & 0xff, ret);
	else
		dev_info(DEV, "helper command: %s %s %s exit code %u (0x%x)\n",
				usermode_helper, cmd, mb,
				(ret >> 8) & 0xff, ret);
	sib.sib_reason = SIB_HELPER_POST;
	sib.helper_exit_code = ret;
	drbd_bcast_event(device, &sib);

	if (ret < 0) /* Ignore any ERRNOs we got. */
		ret = 0;

	return ret;
}

static void conn_md_sync(struct drbd_connection *connection)
{
	struct drbd_device *device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->volumes, device, vnr) {
		kref_get(&device->kref);
		rcu_read_unlock();
		drbd_md_sync(device);
		kref_put(&device->kref, drbd_destroy_device);
		rcu_read_lock();
	}
	rcu_read_unlock();
}

int conn_khelper(struct drbd_connection *connection, char *cmd)
{
	char *envp[] = { "HOME=/",
			"TERM=linux",
			"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
			 (char[20]) { }, /* address family */
			 (char[60]) { }, /* address */
			NULL };
	char *resource_name = connection->resource->name;
	char *argv[] = {usermode_helper, cmd, resource_name, NULL };
	int ret;

	setup_khelper_env(connection, envp);
	conn_md_sync(connection);

	conn_info(connection, "helper command: %s %s %s\n", usermode_helper, cmd, resource_name);
	/* TODO: conn_bcast_event() ?? */

	ret = call_usermodehelper(usermode_helper, argv, envp, 1);
	if (ret)
		conn_warn(connection, "helper command: %s %s %s exit code %u (0x%x)\n",
			  usermode_helper, cmd, resource_name,
			  (ret >> 8) & 0xff, ret);
	else
		conn_info(connection, "helper command: %s %s %s exit code %u (0x%x)\n",
			  usermode_helper, cmd, resource_name,
			  (ret >> 8) & 0xff, ret);
	/* TODO: conn_bcast_event() ?? */

	if (ret < 0) /* Ignore any ERRNOs we got. */
		ret = 0;

	return ret;
}

static enum drbd_fencing_p highest_fencing_policy(struct drbd_connection *connection)
{
	enum drbd_fencing_p fp = FP_NOT_AVAIL;
	struct drbd_device *device;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->volumes, device, vnr) {
		if (get_ldev_if_state(device, D_CONSISTENT)) {
			fp = max_t(enum drbd_fencing_p, fp,
				   rcu_dereference(device->ldev->disk_conf)->fencing);
			put_ldev(device);
		}
	}
	rcu_read_unlock();

	return fp;
}

bool conn_try_outdate_peer(struct drbd_connection *connection)
{
	union drbd_state mask = { };
	union drbd_state val = { };
	enum drbd_fencing_p fp;
	char *ex_to_string;
	int r;

	if (connection->cstate >= C_WF_REPORT_PARAMS) {
		conn_err(connection, "Expected cstate < C_WF_REPORT_PARAMS\n");
		return false;
	}

	fp = highest_fencing_policy(connection);
	switch (fp) {
	case FP_NOT_AVAIL:
		conn_warn(connection, "Not fencing peer, I'm not even Consistent myself.\n");
		goto out;
	case FP_DONT_CARE:
		return true;
	default: ;
	}

	r = conn_khelper(connection, "fence-peer");

	switch ((r>>8) & 0xff) {
	case 3: /* peer is inconsistent */
		ex_to_string = "peer is inconsistent or worse";
		mask.pdsk = D_MASK;
		val.pdsk = D_INCONSISTENT;
		break;
	case 4: /* peer got outdated, or was already outdated */
		ex_to_string = "peer was fenced";
		mask.pdsk = D_MASK;
		val.pdsk = D_OUTDATED;
		break;
	case 5: /* peer was down */
		if (conn_highest_disk(connection) == D_UP_TO_DATE) {
			/* we will(have) create(d) a new UUID anyways... */
			ex_to_string = "peer is unreachable, assumed to be dead";
			mask.pdsk = D_MASK;
			val.pdsk = D_OUTDATED;
		} else {
			ex_to_string = "peer unreachable, doing nothing since disk != UpToDate";
		}
		break;
	case 6: /* Peer is primary, voluntarily outdate myself.
		 * This is useful when an unconnected R_SECONDARY is asked to
		 * become R_PRIMARY, but finds the other peer being active. */
		ex_to_string = "peer is active";
		conn_warn(connection, "Peer is primary, outdating myself.\n");
		mask.disk = D_MASK;
		val.disk = D_OUTDATED;
		break;
	case 7:
		/* THINK: do we need to handle this
		 * like case 4, or more like case 5? */
		if (fp != FP_STONITH)
			conn_err(connection, "fence-peer() = 7 && fencing != Stonith !!!\n");
		ex_to_string = "peer was stonithed";
		mask.pdsk = D_MASK;
		val.pdsk = D_OUTDATED;
		break;
	default:
		/* The script is broken ... */
		conn_err(connection, "fence-peer helper broken, returned %d\n", (r>>8)&0xff);
		return false; /* Eventually leave IO frozen */
	}

	conn_info(connection, "fence-peer helper returned %d (%s)\n",
		  (r>>8) & 0xff, ex_to_string);

 out:

	/* Not using
	   conn_request_state(connection, mask, val, CS_VERBOSE);
	   here, because we might were able to re-establish the connection in the
	   meantime. */
	spin_lock_irq(&connection->req_lock);
	if (connection->cstate < C_WF_REPORT_PARAMS)
		_conn_request_state(connection, mask, val, CS_VERBOSE);
	spin_unlock_irq(&connection->req_lock);

	return conn_highest_pdsk(connection) <= D_OUTDATED;
}

static int _try_outdate_peer_async(void *data)
{
	struct drbd_connection *connection = (struct drbd_connection *)data;

	conn_try_outdate_peer(connection);

	kref_put(&connection->kref, drbd_destroy_connection);
	return 0;
}

void conn_try_outdate_peer_async(struct drbd_connection *connection)
{
	struct task_struct *opa;

	kref_get(&connection->kref);
	opa = kthread_run(_try_outdate_peer_async, connection, "drbd_async_h");
	if (IS_ERR(opa)) {
		conn_err(connection, "out of mem, failed to invoke fence-peer helper\n");
		kref_put(&connection->kref, drbd_destroy_connection);
	}
}

enum drbd_state_rv
drbd_set_role(struct drbd_device *device, enum drbd_role new_role, int force)
{
	const int max_tries = 4;
	enum drbd_state_rv rv = SS_UNKNOWN_ERROR;
	struct net_conf *nc;
	int try = 0;
	int forced = 0;
	union drbd_state mask, val;

	if (new_role == R_PRIMARY)
		request_ping(first_peer_device(device)->connection); /* Detect a dead peer ASAP */

	mutex_lock(device->state_mutex);

	mask.i = 0; mask.role = R_MASK;
	val.i  = 0; val.role  = new_role;

	while (try++ < max_tries) {
		rv = _drbd_request_state(device, mask, val, CS_WAIT_COMPLETE);

		/* in case we first succeeded to outdate,
		 * but now suddenly could establish a connection */
		if (rv == SS_CW_FAILED_BY_PEER && mask.pdsk != 0) {
			val.pdsk = 0;
			mask.pdsk = 0;
			continue;
		}

		if (rv == SS_NO_UP_TO_DATE_DISK && force &&
		    (device->state.disk < D_UP_TO_DATE &&
		     device->state.disk >= D_INCONSISTENT)) {
			mask.disk = D_MASK;
			val.disk  = D_UP_TO_DATE;
			forced = 1;
			continue;
		}

		if (rv == SS_NO_UP_TO_DATE_DISK &&
		    device->state.disk == D_CONSISTENT && mask.pdsk == 0) {
			D_ASSERT(device->state.pdsk == D_UNKNOWN);

			if (conn_try_outdate_peer(first_peer_device(device)->connection)) {
				val.disk = D_UP_TO_DATE;
				mask.disk = D_MASK;
			}
			continue;
		}

		if (rv == SS_NOTHING_TO_DO)
			goto out;
		if (rv == SS_PRIMARY_NOP && mask.pdsk == 0) {
			if (!conn_try_outdate_peer(first_peer_device(device)->connection) && force) {
				dev_warn(DEV, "Forced into split brain situation!\n");
				mask.pdsk = D_MASK;
				val.pdsk  = D_OUTDATED;

			}
			continue;
		}
		if (rv == SS_TWO_PRIMARIES) {
			/* Maybe the peer is detected as dead very soon...
			   retry at most once more in this case. */
			int timeo;
			rcu_read_lock();
			nc = rcu_dereference(first_peer_device(device)->connection->net_conf);
			timeo = nc ? (nc->ping_timeo + 1) * HZ / 10 : 1;
			rcu_read_unlock();
			schedule_timeout_interruptible(timeo);
			if (try < max_tries)
				try = max_tries - 1;
			continue;
		}
		if (rv < SS_SUCCESS) {
			rv = _drbd_request_state(device, mask, val,
						CS_VERBOSE + CS_WAIT_COMPLETE);
			if (rv < SS_SUCCESS)
				goto out;
		}
		break;
	}

	if (rv < SS_SUCCESS)
		goto out;

	if (forced)
		dev_warn(DEV, "Forced to consider local data as UpToDate!\n");

	/* Wait until nothing is on the fly :) */
	wait_event(device->misc_wait, atomic_read(&device->ap_pending_cnt) == 0);

	if (new_role == R_SECONDARY) {
		set_disk_ro(device->vdisk, true);
		if (get_ldev(device)) {
			device->ldev->md.uuid[UI_CURRENT] &= ~(u64)1;
			put_ldev(device);
		}
	} else {
		mutex_lock(&first_peer_device(device)->connection->conf_update);
		nc = first_peer_device(device)->connection->net_conf;
		if (nc)
			nc->discard_my_data = 0; /* without copy; single bit op is atomic */
		mutex_unlock(&first_peer_device(device)->connection->conf_update);

		set_disk_ro(device->vdisk, false);
		if (get_ldev(device)) {
			if (((device->state.conn < C_CONNECTED ||
			       device->state.pdsk <= D_FAILED)
			      && device->ldev->md.uuid[UI_BITMAP] == 0) || forced)
				drbd_uuid_new_current(device);

			device->ldev->md.uuid[UI_CURRENT] |=  (u64)1;
			put_ldev(device);
		}
	}

	/* writeout of activity log covered areas of the bitmap
	 * to stable storage done in after state change already */

	if (device->state.conn >= C_WF_REPORT_PARAMS) {
		/* if this was forced, we should consider sync */
		if (forced)
			drbd_send_uuids(device);
		drbd_send_state(device);
	}

	drbd_md_sync(device);

	drbd_kobject_uevent(device);
out:
	mutex_unlock(device->state_mutex);
	return rv;
}

static const char *from_attrs_err_to_txt(int err)
{
	return	err == -ENOMSG ? "required attribute missing" :
		err == -EOPNOTSUPP ? "unknown mandatory attribute" :
		err == -EEXIST ? "can not change invariant setting" :
		"invalid attribute value";
}

int drbd_adm_set_role(struct sk_buff *skb, struct genl_info *info)
{
	struct set_role_parms parms;
	int err;
	enum drbd_ret_code retcode;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	memset(&parms, 0, sizeof(parms));
	if (info->attrs[DRBD_NLA_SET_ROLE_PARMS]) {
		err = set_role_parms_from_attrs(&parms, info);
		if (err) {
			retcode = ERR_MANDATORY_TAG;
			drbd_msg_put_info(from_attrs_err_to_txt(err));
			goto out;
		}
	}

	if (info->genlhdr->cmd == DRBD_ADM_PRIMARY)
		retcode = drbd_set_role(adm_ctx.device, R_PRIMARY, parms.assume_uptodate);
	else
		retcode = drbd_set_role(adm_ctx.device, R_SECONDARY, 0);
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

/* initializes the md.*_offset members, so we are able to find
 * the on disk meta data */
STATIC void drbd_md_set_sector_offsets(struct drbd_device *device,
				       struct drbd_backing_dev *bdev)
{
	sector_t md_size_sect = 0;
	int meta_dev_idx;

	rcu_read_lock();
	meta_dev_idx = rcu_dereference(bdev->disk_conf)->meta_dev_idx;

	switch (meta_dev_idx) {
	default:
		/* v07 style fixed size indexed meta data */
		bdev->md.md_size_sect = MD_RESERVED_SECT;
		bdev->md.md_offset = drbd_md_ss__(device, bdev);
		bdev->md.al_offset = MD_AL_OFFSET;
		bdev->md.bm_offset = MD_BM_OFFSET;
		break;
	case DRBD_MD_INDEX_FLEX_EXT:
		/* just occupy the full device; unit: sectors */
		bdev->md.md_size_sect = drbd_get_capacity(bdev->md_bdev);
		bdev->md.md_offset = 0;
		bdev->md.al_offset = MD_AL_OFFSET;
		bdev->md.bm_offset = MD_BM_OFFSET;
		break;
	case DRBD_MD_INDEX_INTERNAL:
	case DRBD_MD_INDEX_FLEX_INT:
		bdev->md.md_offset = drbd_md_ss__(device, bdev);
		/* al size is still fixed */
		bdev->md.al_offset = -MD_AL_SECTORS;
		/* we need (slightly less than) ~ this much bitmap sectors: */
		md_size_sect = drbd_get_capacity(bdev->backing_bdev);
		md_size_sect = ALIGN(md_size_sect, BM_SECT_PER_EXT);
		md_size_sect = BM_SECT_TO_EXT(md_size_sect);
		md_size_sect = ALIGN(md_size_sect, 8);

		/* plus the "drbd meta data super block",
		 * and the activity log; */
		md_size_sect += MD_BM_OFFSET;

		bdev->md.md_size_sect = md_size_sect;
		/* bitmap offset is adjusted by 'super' block size */
		bdev->md.bm_offset   = -md_size_sect + MD_AL_OFFSET;
		break;
	}
	rcu_read_unlock();
}

/* input size is expected to be in KB */
char *ppsize(char *buf, unsigned long long size)
{
	/* Needs 9 bytes at max including trailing NUL:
	 * -1ULL ==> "16384 EB" */
	static char units[] = { 'K', 'M', 'G', 'T', 'P', 'E' };
	int base = 0;
	while (size >= 10000 && base < sizeof(units)-1) {
		/* shift + round */
		size = (size >> 10) + !!(size & (1<<9));
		base++;
	}
	sprintf(buf, "%u %cB", (unsigned)size, units[base]);

	return buf;
}

/* there is still a theoretical deadlock when called from receiver
 * on an D_INCONSISTENT R_PRIMARY:
 *  remote READ does inc_ap_bio, receiver would need to receive answer
 *  packet from remote to dec_ap_bio again.
 *  receiver receive_sizes(), comes here,
 *  waits for ap_bio_cnt == 0. -> deadlock.
 * but this cannot happen, actually, because:
 *  R_PRIMARY D_INCONSISTENT, and peer's disk is unreachable
 *  (not connected, or bad/no disk on peer):
 *  see drbd_fail_request_early, ap_bio_cnt is zero.
 *  R_PRIMARY D_INCONSISTENT, and C_SYNC_TARGET:
 *  peer may not initiate a resize.
 */
/* Note these are not to be confused with
 * drbd_adm_suspend_io/drbd_adm_resume_io,
 * which are (sub) state changes triggered by admin (drbdsetup),
 * and can be long lived.
 * This changes an device->flag, is triggered by drbd internals,
 * and should be short-lived. */
void drbd_suspend_io(struct drbd_device *device)
{
	set_bit(SUSPEND_IO, &device->flags);
	if (drbd_suspended(device))
		return;
	wait_event(device->misc_wait, !atomic_read(&device->ap_bio_cnt));
}

void drbd_resume_io(struct drbd_device *device)
{
	clear_bit(SUSPEND_IO, &device->flags);
	wake_up(&device->misc_wait);
}

/**
 * drbd_determine_dev_size() -  Sets the right device size obeying all constraints
 * @device:	DRBD device.
 *
 * Returns 0 on success, negative return values indicate errors.
 * You should call drbd_md_sync() after calling this function.
 */
enum determine_dev_size drbd_determine_dev_size(struct drbd_device *device, enum dds_flags flags) __must_hold(local)
{
	sector_t prev_first_sect, prev_size; /* previous meta location */
	sector_t la_size, u_size;
	sector_t size;
	char ppb[10];

	int md_moved, la_size_changed;
	enum determine_dev_size rv = unchanged;

	/* race:
	 * application request passes inc_ap_bio,
	 * but then cannot get an AL-reference.
	 * this function later may wait on ap_bio_cnt == 0. -> deadlock.
	 *
	 * to avoid that:
	 * Suspend IO right here.
	 * still lock the act_log to not trigger ASSERTs there.
	 */
	drbd_suspend_io(device);

	/* no wait necessary anymore, actually we could assert that */
	wait_event(device->al_wait, lc_try_lock(device->act_log));

	prev_first_sect = drbd_md_first_sector(device->ldev);
	prev_size = device->ldev->md.md_size_sect;
	la_size = device->ldev->md.la_size_sect;

	/* TODO: should only be some assert here, not (re)init... */
	drbd_md_set_sector_offsets(device, device->ldev);

	rcu_read_lock();
	u_size = rcu_dereference(device->ldev->disk_conf)->disk_size;
	rcu_read_unlock();
	size = drbd_new_dev_size(device, device->ldev, u_size, flags & DDSF_FORCED);

	if (drbd_get_capacity(device->this_bdev) != size ||
	    drbd_bm_capacity(device) != size) {
		int err;
		err = drbd_bm_resize(device, size, !(flags & DDSF_NO_RESYNC));
		if (unlikely(err)) {
			/* currently there is only one error: ENOMEM! */
			size = drbd_bm_capacity(device)>>1;
			if (size == 0) {
				dev_err(DEV, "OUT OF MEMORY! "
				    "Could not allocate bitmap!\n");
			} else {
				dev_err(DEV, "BM resizing failed. "
				    "Leaving size unchanged at size = %lu KB\n",
				    (unsigned long)size);
			}
			rv = dev_size_error;
		}
		/* racy, see comments above. */
		drbd_set_my_capacity(device, size);
		device->ldev->md.la_size_sect = size;
		dev_info(DEV, "size = %s (%llu KB)\n", ppsize(ppb, size>>1),
		     (unsigned long long)size>>1);
	}
	if (rv == dev_size_error)
		goto out;

	la_size_changed = (la_size != device->ldev->md.la_size_sect);

	md_moved = prev_first_sect != drbd_md_first_sector(device->ldev)
		|| prev_size	   != device->ldev->md.md_size_sect;

	if (la_size_changed || md_moved) {
		int err;

		drbd_al_shrink(device); /* All extents inactive. */
		dev_info(DEV, "Writing the whole bitmap, %s\n",
			 la_size_changed && md_moved ? "size changed and md moved" :
			 la_size_changed ? "size changed" : "md moved");
		/* next line implicitly does drbd_suspend_io()+drbd_resume_io() */
		err = drbd_bitmap_io(device, &drbd_bm_write,
				"size changed", BM_LOCKED_MASK);
		if (err) {
			rv = dev_size_error;
			goto out;
		}
		drbd_md_mark_dirty(device);
	}

	if (size > la_size)
		rv = grew;
	if (size < la_size)
		rv = shrunk;
out:
	lc_unlock(device->act_log);
	wake_up(&device->al_wait);
	drbd_resume_io(device);

	return rv;
}

sector_t
drbd_new_dev_size(struct drbd_device *device, struct drbd_backing_dev *bdev,
		  sector_t u_size, int assume_peer_has_space)
{
	sector_t p_size = device->p_size;   /* partner's disk size. */
	sector_t la_size = bdev->md.la_size_sect; /* last agreed size. */
	sector_t m_size; /* my size */
	sector_t size = 0;

	m_size = drbd_get_max_capacity(bdev);

	if (device->state.conn < C_CONNECTED && assume_peer_has_space) {
		dev_warn(DEV, "Resize while not connected was forced by the user!\n");
		p_size = m_size;
	}

	if (p_size && m_size) {
		size = min_t(sector_t, p_size, m_size);
	} else {
		if (la_size) {
			size = la_size;
			if (m_size && m_size < size)
				size = m_size;
			if (p_size && p_size < size)
				size = p_size;
		} else {
			if (m_size)
				size = m_size;
			if (p_size)
				size = p_size;
		}
	}

	if (size == 0)
		dev_err(DEV, "Both nodes diskless!\n");

	if (u_size) {
		if (u_size > size)
			dev_err(DEV, "Requested disk size is too big (%lu > %lu)\n",
			    (unsigned long)u_size>>1, (unsigned long)size>>1);
		else
			size = u_size;
	}

	return size;
}

/**
 * drbd_check_al_size() - Ensures that the AL is of the right size
 * @device:	DRBD device.
 *
 * Returns -EBUSY if current al lru is still used, -ENOMEM when allocation
 * failed, and 0 on success. You should call drbd_md_sync() after you called
 * this function.
 */
STATIC int drbd_check_al_size(struct drbd_device *device, struct disk_conf *dc)
{
	struct lru_cache *n, *t;
	struct lc_element *e;
	unsigned int in_use;
	int i;

	if (device->act_log &&
	    device->act_log->nr_elements == dc->al_extents)
		return 0;

	in_use = 0;
	t = device->act_log;
	n = lc_create("act_log", drbd_al_ext_cache, AL_UPDATES_PER_TRANSACTION,
		dc->al_extents, sizeof(struct lc_element), 0);

	if (n == NULL) {
		dev_err(DEV, "Cannot allocate act_log lru!\n");
		return -ENOMEM;
	}
	spin_lock_irq(&device->al_lock);
	if (t) {
		for (i = 0; i < t->nr_elements; i++) {
			e = lc_element_by_index(t, i);
			if (e->refcnt)
				dev_err(DEV, "refcnt(%d)==%d\n",
				    e->lc_number, e->refcnt);
			in_use += e->refcnt;
		}
	}
	if (!in_use)
		device->act_log = n;
	spin_unlock_irq(&device->al_lock);
	if (in_use) {
		dev_err(DEV, "Activity log still in use!\n");
		lc_destroy(n);
		return -EBUSY;
	} else {
		if (t)
			lc_destroy(t);
	}
	drbd_md_mark_dirty(device); /* we changed device->act_log->nr_elemens */
	return 0;
}

static void drbd_setup_queue_param(struct drbd_device *device, unsigned int max_bio_size)
{
	struct request_queue * const q = device->rq_queue;
	int max_hw_sectors = max_bio_size >> 9;
	int max_segments = 0;

	if (get_ldev_if_state(device, D_ATTACHING)) {
		struct request_queue * const b = device->ldev->backing_bdev->bd_disk->queue;

		max_hw_sectors = min(queue_max_hw_sectors(b), max_bio_size >> 9);
		rcu_read_lock();
		max_segments = rcu_dereference(device->ldev->disk_conf)->max_bio_bvecs;
		rcu_read_unlock();
		put_ldev(device);
	}

	blk_queue_logical_block_size(q, 512);
	blk_queue_max_hw_sectors(q, max_hw_sectors);
	/* This is the workaround for "bio would need to, but cannot, be split" */
	blk_queue_max_segments(q, max_segments ? max_segments : BLK_MAX_SEGMENTS);
	blk_queue_segment_boundary(q, PAGE_CACHE_SIZE-1);

	if (get_ldev_if_state(device, D_ATTACHING)) {
		struct request_queue * const b = device->ldev->backing_bdev->bd_disk->queue;

		blk_queue_stack_limits(q, b);

		if (q->backing_dev_info.ra_pages != b->backing_dev_info.ra_pages) {
			dev_info(DEV, "Adjusting my ra_pages to backing device's (%lu -> %lu)\n",
				 q->backing_dev_info.ra_pages,
				 b->backing_dev_info.ra_pages);
			q->backing_dev_info.ra_pages = b->backing_dev_info.ra_pages;
		}
		put_ldev(device);
	}
}

void drbd_reconsider_max_bio_size(struct drbd_device *device)
{
	int now, new, local, peer;

	now = queue_max_hw_sectors(device->rq_queue) << 9;
	local = device->local_max_bio_size; /* Eventually last known value, from volatile memory */
	peer = device->peer_max_bio_size; /* Eventually last known value, from meta data */

	if (get_ldev_if_state(device, D_ATTACHING)) {
		local = queue_max_hw_sectors(device->ldev->backing_bdev->bd_disk->queue) << 9;
		device->local_max_bio_size = local;
		put_ldev(device);
	}

	/* We may ignore peer limits if the peer is modern enough.
	   Because new from 8.3.8 onwards the peer can use multiple
	   BIOs for a single peer_request */
	if (device->state.conn >= C_CONNECTED) {
		if (first_peer_device(device)->connection->agreed_pro_version < 94)
			peer = min_t(int, device->peer_max_bio_size, DRBD_MAX_SIZE_H80_PACKET);
			/* Correct old drbd (up to 8.3.7) if it believes it can do more than 32KiB */
		else if (first_peer_device(device)->connection->agreed_pro_version == 94)
			peer = DRBD_MAX_SIZE_H80_PACKET;
		else if (first_peer_device(device)->connection->agreed_pro_version < 100)
			peer = DRBD_MAX_BIO_SIZE_P95;  /* drbd 8.3.8 onwards, before 8.4.0 */
		else
			peer = DRBD_MAX_BIO_SIZE;
	}

	new = min_t(int, local, peer);

	if (device->state.role == R_PRIMARY && new < now)
		dev_err(DEV, "ASSERT FAILED new < now; (%d < %d)\n", new, now);

	if (new != now)
		dev_info(DEV, "max BIO size = %u\n", new);

	drbd_setup_queue_param(device, new);
}

/* Starts the worker thread */
static void conn_reconfig_start(struct drbd_connection *connection)
{
	drbd_thread_start(&connection->worker);
	conn_flush_workqueue(connection);
}

/* if still unconfigured, stops worker again. */
static void conn_reconfig_done(struct drbd_connection *connection)
{
	bool stop_threads;
	spin_lock_irq(&connection->req_lock);
	stop_threads = conn_all_vols_unconf(connection) &&
		connection->cstate == C_STANDALONE;
	spin_unlock_irq(&connection->req_lock);
	if (stop_threads) {
		/* asender is implicitly stopped by receiver
		 * in conn_disconnect() */
		drbd_thread_stop(&connection->receiver);
		drbd_thread_stop(&connection->worker);
	}
}

/* Make sure IO is suspended before calling this function(). */
static void drbd_suspend_al(struct drbd_device *device)
{
	int s = 0;

	if (!lc_try_lock(device->act_log)) {
		dev_warn(DEV, "Failed to lock al in drbd_suspend_al()\n");
		return;
	}

	drbd_al_shrink(device);
	spin_lock_irq(&first_peer_device(device)->connection->req_lock);
	if (device->state.conn < C_CONNECTED)
		s = !test_and_set_bit(AL_SUSPENDED, &device->flags);
	spin_unlock_irq(&first_peer_device(device)->connection->req_lock);
	lc_unlock(device->act_log);

	if (s)
		dev_info(DEV, "Suspended AL updates\n");
}


static bool should_set_defaults(struct genl_info *info)
{
	unsigned flags = ((struct drbd_genlmsghdr*)info->userhdr)->flags;
	return 0 != (flags & DRBD_GENL_F_SET_DEFAULTS);
}

static void enforce_disk_conf_limits(struct disk_conf *dc)
{
	if (dc->al_extents < DRBD_AL_EXTENTS_MIN)
		dc->al_extents = DRBD_AL_EXTENTS_MIN;
	if (dc->al_extents > DRBD_AL_EXTENTS_MAX)
		dc->al_extents = DRBD_AL_EXTENTS_MAX;

	if (dc->c_plan_ahead > DRBD_C_PLAN_AHEAD_MAX)
		dc->c_plan_ahead = DRBD_C_PLAN_AHEAD_MAX;
}

int drbd_adm_disk_opts(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;
	struct drbd_device *device;
	struct disk_conf *new_disk_conf, *old_disk_conf;
	struct fifo_buffer *old_plan = NULL, *new_plan = NULL;
	int err, fifo_size;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	device = adm_ctx.device;

	/* we also need a disk
	 * to change the options on */
	if (!get_ldev(device)) {
		retcode = ERR_NO_DISK;
		goto out;
	}

	new_disk_conf = kmalloc(sizeof(struct disk_conf), GFP_KERNEL);
	if (!new_disk_conf) {
		retcode = ERR_NOMEM;
		goto fail;
	}

	mutex_lock(&first_peer_device(device)->connection->conf_update);
	old_disk_conf = device->ldev->disk_conf;
	*new_disk_conf = *old_disk_conf;
	if (should_set_defaults(info))
		set_disk_conf_defaults(new_disk_conf);

	err = disk_conf_from_attrs_for_change(new_disk_conf, info);
	if (err && err != -ENOMSG) {
		retcode = ERR_MANDATORY_TAG;
		drbd_msg_put_info(from_attrs_err_to_txt(err));
	}

	if (!expect(new_disk_conf->resync_rate >= 1))
		new_disk_conf->resync_rate = 1;

	enforce_disk_conf_limits(new_disk_conf);

	fifo_size = (new_disk_conf->c_plan_ahead * 10 * SLEEP_TIME) / HZ;
	if (fifo_size != device->rs_plan_s->size) {
		new_plan = fifo_alloc(fifo_size);
		if (!new_plan) {
			dev_err(DEV, "kmalloc of fifo_buffer failed");
			retcode = ERR_NOMEM;
			goto fail_unlock;
		}
	}

	wait_event(device->al_wait, lc_try_lock(device->act_log));
	drbd_al_shrink(device);
	err = drbd_check_al_size(device, new_disk_conf);
	lc_unlock(device->act_log);
	wake_up(&device->al_wait);

	if (err) {
		retcode = ERR_NOMEM;
		goto fail_unlock;
	}

	write_lock_irq(&global_state_lock);
	retcode = drbd_resync_after_valid(device, new_disk_conf->resync_after);
	if (retcode == NO_ERROR) {
		rcu_assign_pointer(device->ldev->disk_conf, new_disk_conf);
		drbd_resync_after_changed(device);
	}
	write_unlock_irq(&global_state_lock);

	if (retcode != NO_ERROR)
		goto fail_unlock;

	if (new_plan) {
		old_plan = device->rs_plan_s;
		rcu_assign_pointer(device->rs_plan_s, new_plan);
	}

	mutex_unlock(&first_peer_device(device)->connection->conf_update);
	drbd_md_sync(device);

	if (device->state.conn >= C_CONNECTED)
		drbd_send_sync_param(device);

	synchronize_rcu();
	kfree(old_disk_conf);
	kfree(old_plan);
	mod_timer(&device->request_timer, jiffies + HZ);
	goto success;

fail_unlock:
	mutex_unlock(&first_peer_device(device)->connection->conf_update);
 fail:
	kfree(new_disk_conf);
	kfree(new_plan);
success:
	put_ldev(device);
 out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_attach(struct sk_buff *skb, struct genl_info *info)
{
	struct drbd_device *device;
	int err;
	enum drbd_ret_code retcode;
	enum determine_dev_size dd;
	sector_t max_possible_sectors;
	sector_t min_md_device_sectors;
	struct drbd_backing_dev *nbc = NULL; /* new_backing_conf */
	struct disk_conf *new_disk_conf = NULL;
	struct block_device *bdev;
	struct lru_cache *resync_lru = NULL;
	struct fifo_buffer *new_plan = NULL;
	union drbd_state ns, os;
	enum drbd_state_rv rv;
	struct net_conf *nc;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto finish;

	device = adm_ctx.device;
	conn_reconfig_start(first_peer_device(device)->connection);

	/* if you want to reconfigure, please tear down first */
	if (device->state.disk > D_DISKLESS) {
		retcode = ERR_DISK_CONFIGURED;
		goto fail;
	}
	/* It may just now have detached because of IO error.  Make sure
	 * drbd_ldev_destroy is done already, we may end up here very fast,
	 * e.g. if someone calls attach from the on-io-error handler,
	 * to realize a "hot spare" feature (not that I'd recommend that) */
	wait_event(device->misc_wait, !atomic_read(&device->local_cnt));

	/* allocation not in the IO path, drbdsetup context */
	nbc = kzalloc(sizeof(struct drbd_backing_dev), GFP_KERNEL);
	if (!nbc) {
		retcode = ERR_NOMEM;
		goto fail;
	}
	new_disk_conf = kzalloc(sizeof(struct disk_conf), GFP_KERNEL);
	if (!new_disk_conf) {
		retcode = ERR_NOMEM;
		goto fail;
	}
	nbc->disk_conf = new_disk_conf;

	set_disk_conf_defaults(new_disk_conf);
	err = disk_conf_from_attrs(new_disk_conf, info);
	if (err) {
		retcode = ERR_MANDATORY_TAG;
		drbd_msg_put_info(from_attrs_err_to_txt(err));
		goto fail;
	}

	enforce_disk_conf_limits(new_disk_conf);

	new_plan = fifo_alloc((new_disk_conf->c_plan_ahead * 10 * SLEEP_TIME) / HZ);
	if (!new_plan) {
		retcode = ERR_NOMEM;
		goto fail;
	}

	if (new_disk_conf->meta_dev_idx < DRBD_MD_INDEX_FLEX_INT) {
		retcode = ERR_MD_IDX_INVALID;
		goto fail;
	}

	rcu_read_lock();
	nc = rcu_dereference(first_peer_device(device)->connection->net_conf);
	if (nc) {
		if (new_disk_conf->fencing == FP_STONITH && nc->wire_protocol == DRBD_PROT_A) {
			rcu_read_unlock();
			retcode = ERR_STONITH_AND_PROT_A;
			goto fail;
		}
	}
	rcu_read_unlock();

	bdev = blkdev_get_by_path(new_disk_conf->backing_dev,
				  FMODE_READ | FMODE_WRITE | FMODE_EXCL, device);
	if (IS_ERR(bdev)) {
		dev_err(DEV, "open(\"%s\") failed with %ld\n", new_disk_conf->backing_dev,
			PTR_ERR(bdev));
		retcode = ERR_OPEN_DISK;
		goto fail;
	}
	nbc->backing_bdev = bdev;

	/*
	 * meta_dev_idx >= 0: external fixed size, possibly multiple
	 * drbd sharing one meta device.  TODO in that case, paranoia
	 * check that [md_bdev, meta_dev_idx] is not yet used by some
	 * other drbd minor!  (if you use drbd.conf + drbdadm, that
	 * should check it for you already; but if you don't, or
	 * someone fooled it, we need to double check here)
	 */
	bdev = blkdev_get_by_path(new_disk_conf->meta_dev,
				  FMODE_READ | FMODE_WRITE | FMODE_EXCL,
				  (new_disk_conf->meta_dev_idx < 0) ?
				  (void *)device : (void *)drbd_m_holder);
	if (IS_ERR(bdev)) {
		dev_err(DEV, "open(\"%s\") failed with %ld\n", new_disk_conf->meta_dev,
			PTR_ERR(bdev));
		retcode = ERR_OPEN_MD_DISK;
		goto fail;
	}
	nbc->md_bdev = bdev;

	if ((nbc->backing_bdev == nbc->md_bdev) !=
	    (new_disk_conf->meta_dev_idx == DRBD_MD_INDEX_INTERNAL ||
	     new_disk_conf->meta_dev_idx == DRBD_MD_INDEX_FLEX_INT)) {
		retcode = ERR_MD_IDX_INVALID;
		goto fail;
	}

	resync_lru = lc_create("resync", drbd_bm_ext_cache,
			1, 61, sizeof(struct bm_extent),
			offsetof(struct bm_extent, lce));
	if (!resync_lru) {
		retcode = ERR_NOMEM;
		goto fail;
	}

	/* RT - for drbd_get_max_capacity() DRBD_MD_INDEX_FLEX_INT */
	drbd_md_set_sector_offsets(device, nbc);

	if (drbd_get_max_capacity(nbc) < new_disk_conf->disk_size) {
		dev_err(DEV, "max capacity %llu smaller than disk size %llu\n",
			(unsigned long long) drbd_get_max_capacity(nbc),
			(unsigned long long) new_disk_conf->disk_size);
		retcode = ERR_DISK_TOO_SMALL;
		goto fail;
	}

	if (new_disk_conf->meta_dev_idx < 0) {
		max_possible_sectors = DRBD_MAX_SECTORS_FLEX;
		/* at least one MB, otherwise it does not make sense */
		min_md_device_sectors = (2<<10);
	} else {
		max_possible_sectors = DRBD_MAX_SECTORS;
		min_md_device_sectors = MD_RESERVED_SECT * (new_disk_conf->meta_dev_idx + 1);
	}

	if (drbd_get_capacity(nbc->md_bdev) < min_md_device_sectors) {
		retcode = ERR_MD_DISK_TOO_SMALL;
		dev_warn(DEV, "refusing attach: md-device too small, "
		     "at least %llu sectors needed for this meta-disk type\n",
		     (unsigned long long) min_md_device_sectors);
		goto fail;
	}

	/* Make sure the new disk is big enough
	 * (we may currently be R_PRIMARY with no local disk...) */
	if (drbd_get_max_capacity(nbc) <
	    drbd_get_capacity(device->this_bdev)) {
		retcode = ERR_DISK_TOO_SMALL;
		goto fail;
	}

	nbc->known_size = drbd_get_capacity(nbc->backing_bdev);

	if (nbc->known_size > max_possible_sectors) {
		dev_warn(DEV, "==> truncating very big lower level device "
			"to currently maximum possible %llu sectors <==\n",
			(unsigned long long) max_possible_sectors);
		if (new_disk_conf->meta_dev_idx >= 0)
			dev_warn(DEV, "==>> using internal or flexible "
				      "meta data may help <<==\n");
	}

	drbd_suspend_io(device);
	/* also wait for the last barrier ack. */
	wait_event(device->misc_wait, !atomic_read(&device->ap_pending_cnt) || drbd_suspended(device));
	/* and for any other previously queued work */
	drbd_flush_workqueue(device);

	rv = _drbd_request_state(device, NS(disk, D_ATTACHING), CS_VERBOSE);
	retcode = rv;  /* FIXME: Type mismatch. */
	drbd_resume_io(device);
	if (rv < SS_SUCCESS)
		goto fail;

	if (!get_ldev_if_state(device, D_ATTACHING))
		goto force_diskless;

	drbd_md_set_sector_offsets(device, nbc);

	if (!device->bitmap) {
		if (drbd_bm_init(device)) {
			retcode = ERR_NOMEM;
			goto force_diskless_dec;
		}
	}

	retcode = drbd_md_read(device, nbc);
	if (retcode != NO_ERROR)
		goto force_diskless_dec;

	if (device->state.conn < C_CONNECTED &&
	    device->state.role == R_PRIMARY &&
	    (device->ed_uuid & ~((u64)1)) != (nbc->md.uuid[UI_CURRENT] & ~((u64)1))) {
		dev_err(DEV, "Can only attach to data with current UUID=%016llX\n",
		    (unsigned long long)device->ed_uuid);
		retcode = ERR_DATA_NOT_CURRENT;
		goto force_diskless_dec;
	}

	/* Since we are diskless, fix the activity log first... */
	if (drbd_check_al_size(device, new_disk_conf)) {
		retcode = ERR_NOMEM;
		goto force_diskless_dec;
	}

	/* Prevent shrinking of consistent devices ! */
	if (drbd_md_test_flag(nbc, MDF_CONSISTENT) &&
	    drbd_new_dev_size(device, nbc, nbc->disk_conf->disk_size, 0) < nbc->md.la_size_sect) {
		dev_warn(DEV, "refusing to truncate a consistent device\n");
		retcode = ERR_DISK_TOO_SMALL;
		goto force_diskless_dec;
	}

	/* Reset the "barriers don't work" bits here, then force meta data to
	 * be written, to ensure we determine if barriers are supported. */
	if (new_disk_conf->md_flushes)
		clear_bit(MD_NO_BARRIER, &device->flags);
	else
		set_bit(MD_NO_BARRIER, &device->flags);

	/* Point of no return reached.
	 * Devices and memory are no longer released by error cleanup below.
	 * now device takes over responsibility, and the state engine should
	 * clean it up somewhere.  */
	D_ASSERT(device->ldev == NULL);
	device->ldev = nbc;
	device->resync = resync_lru;
	device->rs_plan_s = new_plan;
	nbc = NULL;
	resync_lru = NULL;
	new_disk_conf = NULL;
	new_plan = NULL;

	device->write_ordering = WO_bio_barrier;
	drbd_bump_write_ordering(device, WO_bio_barrier);

	if (drbd_md_test_flag(device->ldev, MDF_CRASHED_PRIMARY))
		set_bit(CRASHED_PRIMARY, &device->flags);
	else
		clear_bit(CRASHED_PRIMARY, &device->flags);

	if (drbd_md_test_flag(device->ldev, MDF_PRIMARY_IND) &&
	    !(device->state.role == R_PRIMARY &&
	      first_peer_device(device)->connection->susp_nod))
		set_bit(CRASHED_PRIMARY, &device->flags);

	device->send_cnt = 0;
	device->recv_cnt = 0;
	device->read_cnt = 0;
	device->writ_cnt = 0;

	drbd_reconsider_max_bio_size(device);

	/* If I am currently not R_PRIMARY,
	 * but meta data primary indicator is set,
	 * I just now recover from a hard crash,
	 * and have been R_PRIMARY before that crash.
	 *
	 * Now, if I had no connection before that crash
	 * (have been degraded R_PRIMARY), chances are that
	 * I won't find my peer now either.
	 *
	 * In that case, and _only_ in that case,
	 * we use the degr-wfc-timeout instead of the default,
	 * so we can automatically recover from a crash of a
	 * degraded but active "cluster" after a certain timeout.
	 */
	clear_bit(USE_DEGR_WFC_T, &device->flags);
	if (device->state.role != R_PRIMARY &&
	     drbd_md_test_flag(device->ldev, MDF_PRIMARY_IND) &&
	    !drbd_md_test_flag(device->ldev, MDF_CONNECTED_IND))
		set_bit(USE_DEGR_WFC_T, &device->flags);

	dd = drbd_determine_dev_size(device, 0);
	if (dd == dev_size_error) {
		retcode = ERR_NOMEM_BITMAP;
		goto force_diskless_dec;
	} else if (dd == grew)
		set_bit(RESYNC_AFTER_NEG, &device->flags);

	if (drbd_md_test_flag(device->ldev, MDF_FULL_SYNC)) {
		dev_info(DEV, "Assuming that all blocks are out of sync "
		     "(aka FullSync)\n");
		if (drbd_bitmap_io(device, &drbd_bmio_set_n_write,
			"set_n_write from attaching", BM_LOCKED_MASK)) {
			retcode = ERR_IO_MD_DISK;
			goto force_diskless_dec;
		}
	} else {
		if (drbd_bitmap_io(device, &drbd_bm_read,
			"read from attaching", BM_LOCKED_MASK)) {
			retcode = ERR_IO_MD_DISK;
			goto force_diskless_dec;
		}
	}

	if (_drbd_bm_total_weight(device) == drbd_bm_bits(device))
		drbd_suspend_al(device); /* IO is still suspended here... */

	spin_lock_irq(&first_peer_device(device)->connection->req_lock);
	os = drbd_read_state(device);
	ns = os;
	/* If MDF_CONSISTENT is not set go into inconsistent state,
	   otherwise investigate MDF_WasUpToDate...
	   If MDF_WAS_UP_TO_DATE is not set go into D_OUTDATED disk state,
	   otherwise into D_CONSISTENT state.
	*/
	if (drbd_md_test_flag(device->ldev, MDF_CONSISTENT)) {
		if (drbd_md_test_flag(device->ldev, MDF_WAS_UP_TO_DATE))
			ns.disk = D_CONSISTENT;
		else
			ns.disk = D_OUTDATED;
	} else {
		ns.disk = D_INCONSISTENT;
	}

	if (drbd_md_test_flag(device->ldev, MDF_PEER_OUT_DATED))
		ns.pdsk = D_OUTDATED;

	rcu_read_lock();
	if (ns.disk == D_CONSISTENT &&
	    (ns.pdsk == D_OUTDATED || rcu_dereference(device->ldev->disk_conf)->fencing == FP_DONT_CARE))
		ns.disk = D_UP_TO_DATE;
	rcu_read_unlock();

	/* All tests on MDF_PRIMARY_IND, MDF_CONNECTED_IND,
	   MDF_CONSISTENT and MDF_WAS_UP_TO_DATE must happen before
	   this point, because drbd_request_state() modifies these
	   flags. */

	/* In case we are C_CONNECTED postpone any decision on the new disk
	   state after the negotiation phase. */
	if (device->state.conn == C_CONNECTED) {
		device->new_state_tmp.i = ns.i;
		ns.i = os.i;
		ns.disk = D_NEGOTIATING;

		/* We expect to receive up-to-date UUIDs soon.
		   To avoid a race in receive_state, free p_uuid while
		   holding req_lock. I.e. atomic with the state change */
		kfree(device->p_uuid);
		device->p_uuid = NULL;
	}

	rv = _drbd_set_state(device, ns, CS_VERBOSE, NULL);
	spin_unlock_irq(&first_peer_device(device)->connection->req_lock);

	if (rv < SS_SUCCESS)
		goto force_diskless_dec;

	mod_timer(&device->request_timer, jiffies + HZ);

	if (device->state.role == R_PRIMARY)
		device->ldev->md.uuid[UI_CURRENT] |=  (u64)1;
	else
		device->ldev->md.uuid[UI_CURRENT] &= ~(u64)1;

	drbd_md_mark_dirty(device);
	drbd_md_sync(device);

	drbd_kobject_uevent(device);
	put_ldev(device);
	conn_reconfig_done(first_peer_device(device)->connection);
	drbd_adm_finish(info, retcode);
	return 0;

 force_diskless_dec:
	put_ldev(device);
 force_diskless:
	drbd_force_state(device, NS(disk, D_DISKLESS));
	drbd_md_sync(device);
 fail:
	conn_reconfig_done(first_peer_device(device)->connection);
	if (nbc) {
		if (nbc->backing_bdev)
			blkdev_put(nbc->backing_bdev,
				   FMODE_READ | FMODE_WRITE | FMODE_EXCL);
		if (nbc->md_bdev)
			blkdev_put(nbc->md_bdev,
				   FMODE_READ | FMODE_WRITE | FMODE_EXCL);
		kfree(nbc);
	}
	kfree(new_disk_conf);
	lc_destroy(resync_lru);
	kfree(new_plan);

 finish:
	drbd_adm_finish(info, retcode);
	return 0;
}

static int adm_detach(struct drbd_device *device, int force)
{
	enum drbd_state_rv retcode;
	int ret;

	if (force) {
		drbd_force_state(device, NS(disk, D_FAILED));
		retcode = SS_SUCCESS;
		goto out;
	}

	drbd_suspend_io(device); /* so no-one is stuck in drbd_al_begin_io */
	retcode = drbd_request_state(device, NS(disk, D_FAILED));
	/* D_FAILED will transition to DISKLESS. */
	ret = wait_event_interruptible(device->misc_wait,
			device->state.disk != D_FAILED);
	drbd_resume_io(device);
	if (retcode == SS_IS_DISKLESS)
		retcode = SS_NOTHING_TO_DO;
	if (ret)
		retcode = ERR_INTR;
out:
	return retcode;
}

/* Detaching the disk is a process in multiple stages.  First we need to lock
 * out application IO, in-flight IO, IO stuck in drbd_al_begin_io.
 * Then we transition to D_DISKLESS, and wait for put_ldev() to return all
 * internal references as well.
 * Only then we have finally detached. */
int drbd_adm_detach(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;
	struct detach_parms parms = { };
	int err;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	if (info->attrs[DRBD_NLA_DETACH_PARMS]) {
		err = detach_parms_from_attrs(&parms, info);
		if (err) {
			retcode = ERR_MANDATORY_TAG;
			drbd_msg_put_info(from_attrs_err_to_txt(err));
			goto out;
		}
	}

	retcode = adm_detach(adm_ctx.device, parms.force_detach);
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

static bool conn_resync_running(struct drbd_connection *connection)
{
	struct drbd_device *device;
	bool rv = false;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->volumes, device, vnr) {
		if (device->state.conn == C_SYNC_SOURCE ||
		    device->state.conn == C_SYNC_TARGET ||
		    device->state.conn == C_PAUSED_SYNC_S ||
		    device->state.conn == C_PAUSED_SYNC_T) {
			rv = true;
			break;
		}
	}
	rcu_read_unlock();

	return rv;
}

static bool conn_ov_running(struct drbd_connection *connection)
{
	struct drbd_device *device;
	bool rv = false;
	int vnr;

	rcu_read_lock();
	idr_for_each_entry(&connection->volumes, device, vnr) {
		if (device->state.conn == C_VERIFY_S ||
		    device->state.conn == C_VERIFY_T) {
			rv = true;
			break;
		}
	}
	rcu_read_unlock();

	return rv;
}

static enum drbd_ret_code
_check_net_options(struct drbd_connection *connection, struct net_conf *old_conf, struct net_conf *new_conf)
{
	struct drbd_device *device;
	int i;

	if (old_conf && connection->cstate == C_WF_REPORT_PARAMS && connection->agreed_pro_version < 100) {
		if (new_conf->wire_protocol != old_conf->wire_protocol)
			return ERR_NEED_APV_100;

		if (new_conf->two_primaries != old_conf->two_primaries)
			return ERR_NEED_APV_100;

		if (!new_conf->integrity_alg != !old_conf->integrity_alg)
			return ERR_NEED_APV_100;

		if (strcmp(new_conf->integrity_alg, old_conf->integrity_alg))
			return ERR_NEED_APV_100;
	}

	if (!new_conf->two_primaries &&
	    conn_highest_role(connection) == R_PRIMARY &&
	    conn_highest_peer(connection) == R_PRIMARY)
		return ERR_NEED_ALLOW_TWO_PRI;

	if (new_conf->two_primaries &&
	    (new_conf->wire_protocol != DRBD_PROT_C))
		return ERR_NOT_PROTO_C;

	idr_for_each_entry(&connection->volumes, device, i) {
		if (get_ldev(device)) {
			enum drbd_fencing_p fp = rcu_dereference(device->ldev->disk_conf)->fencing;
			put_ldev(device);
			if (new_conf->wire_protocol == DRBD_PROT_A && fp == FP_STONITH)
				return ERR_STONITH_AND_PROT_A;
		}
		if (device->state.role == R_PRIMARY && new_conf->discard_my_data)
			return ERR_DISCARD;
	}

	if (new_conf->on_congestion != OC_BLOCK && new_conf->wire_protocol != DRBD_PROT_A)
		return ERR_CONG_NOT_PROTO_A;

	return NO_ERROR;
}

static enum drbd_ret_code
check_net_options(struct drbd_connection *connection, struct net_conf *new_conf)
{
	static enum drbd_ret_code rv;
	struct drbd_device *device;
	int i;

	rcu_read_lock();
	rv = _check_net_options(connection, rcu_dereference(connection->net_conf), new_conf);
	rcu_read_unlock();

	/* connection->volumes protected by genl_lock() here */
	idr_for_each_entry(&connection->volumes, device, i) {
		if (!device->bitmap) {
			if(drbd_bm_init(device))
				return ERR_NOMEM;
		}
	}

	return rv;
}

struct crypto {
	struct crypto_hash *verify_tfm;
	struct crypto_hash *csums_tfm;
	struct crypto_hash *cram_hmac_tfm;
	struct crypto_hash *integrity_tfm;
};

static int
alloc_hash(struct crypto_hash **tfm, char *tfm_name, int err_alg)
{
	if (!tfm_name[0])
		return NO_ERROR;

	*tfm = crypto_alloc_hash(tfm_name, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(*tfm)) {
		*tfm = NULL;
		return err_alg;
	}

	return NO_ERROR;
}

static enum drbd_ret_code
alloc_crypto(struct crypto *crypto, struct net_conf *new_conf)
{
	char hmac_name[CRYPTO_MAX_ALG_NAME];
	enum drbd_ret_code rv;

	rv = alloc_hash(&crypto->csums_tfm, new_conf->csums_alg,
		       ERR_CSUMS_ALG);
	if (rv != NO_ERROR)
		return rv;
	rv = alloc_hash(&crypto->verify_tfm, new_conf->verify_alg,
		       ERR_VERIFY_ALG);
	if (rv != NO_ERROR)
		return rv;
	rv = alloc_hash(&crypto->integrity_tfm, new_conf->integrity_alg,
		       ERR_INTEGRITY_ALG);
	if (rv != NO_ERROR)
		return rv;
	if (new_conf->cram_hmac_alg[0] != 0) {
		snprintf(hmac_name, CRYPTO_MAX_ALG_NAME, "hmac(%s)",
			 new_conf->cram_hmac_alg);

		rv = alloc_hash(&crypto->cram_hmac_tfm, hmac_name,
			       ERR_AUTH_ALG);
	}

	return rv;
}

static void free_crypto(struct crypto *crypto)
{
	crypto_free_hash(crypto->cram_hmac_tfm);
	crypto_free_hash(crypto->integrity_tfm);
	crypto_free_hash(crypto->csums_tfm);
	crypto_free_hash(crypto->verify_tfm);
}

int drbd_adm_net_opts(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;
	struct drbd_connection *connection;
	struct net_conf *old_conf, *new_conf = NULL;
	int err;
	int ovr; /* online verify running */
	int rsr; /* re-sync running */
	struct crypto crypto = { };

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_CONNECTION);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	connection = adm_ctx.connection;

	new_conf = kzalloc(sizeof(struct net_conf), GFP_KERNEL);
	if (!new_conf) {
		retcode = ERR_NOMEM;
		goto out;
	}

	conn_reconfig_start(connection);

	mutex_lock(&connection->data.mutex);
	mutex_lock(&connection->conf_update);
	old_conf = connection->net_conf;

	if (!old_conf) {
		drbd_msg_put_info("net conf missing, try connect");
		retcode = ERR_INVALID_REQUEST;
		goto fail;
	}

	*new_conf = *old_conf;
	if (should_set_defaults(info))
		set_net_conf_defaults(new_conf);

	err = net_conf_from_attrs_for_change(new_conf, info);
	if (err && err != -ENOMSG) {
		retcode = ERR_MANDATORY_TAG;
		drbd_msg_put_info(from_attrs_err_to_txt(err));
		goto fail;
	}

	retcode = check_net_options(connection, new_conf);
	if (retcode != NO_ERROR)
		goto fail;

	/* re-sync running */
	rsr = conn_resync_running(connection);
	if (rsr && strcmp(new_conf->csums_alg, old_conf->csums_alg)) {
		retcode = ERR_CSUMS_RESYNC_RUNNING;
		goto fail;
	}

	/* online verify running */
	ovr = conn_ov_running(connection);
	if (ovr && strcmp(new_conf->verify_alg, old_conf->verify_alg)) {
		retcode = ERR_VERIFY_RUNNING;
		goto fail;
	}

	retcode = alloc_crypto(&crypto, new_conf);
	if (retcode != NO_ERROR)
		goto fail;

	rcu_assign_pointer(connection->net_conf, new_conf);

	if (!rsr) {
		crypto_free_hash(connection->csums_tfm);
		connection->csums_tfm = crypto.csums_tfm;
		crypto.csums_tfm = NULL;
	}
	if (!ovr) {
		crypto_free_hash(connection->verify_tfm);
		connection->verify_tfm = crypto.verify_tfm;
		crypto.verify_tfm = NULL;
	}

	crypto_free_hash(connection->integrity_tfm);
	connection->integrity_tfm = crypto.integrity_tfm;
	if (connection->cstate >= C_WF_REPORT_PARAMS && connection->agreed_pro_version >= 100)
		/* Do this without trying to take connection->data.mutex again.  */
		__drbd_send_protocol(connection, P_PROTOCOL_UPDATE);

	crypto_free_hash(connection->cram_hmac_tfm);
	connection->cram_hmac_tfm = crypto.cram_hmac_tfm;

	mutex_unlock(&connection->conf_update);
	mutex_unlock(&connection->data.mutex);
	synchronize_rcu();
	kfree(old_conf);

	if (connection->cstate >= C_WF_REPORT_PARAMS)
		drbd_send_sync_param(minor_to_mdev(conn_lowest_minor(connection)));

	goto done;

 fail:
	mutex_unlock(&connection->conf_update);
	mutex_unlock(&connection->data.mutex);
	free_crypto(&crypto);
	kfree(new_conf);
 done:
	conn_reconfig_done(connection);
 out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_connect(struct sk_buff *skb, struct genl_info *info)
{
	struct drbd_device *device;
	struct net_conf *old_conf, *new_conf = NULL;
	struct crypto crypto = { };
	struct drbd_resource *resource;
	struct drbd_connection *connection;
	enum drbd_ret_code retcode;
	int i;
	int err;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_RESOURCE);

	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;
	if (!(adm_ctx.my_addr && adm_ctx.peer_addr)) {
		drbd_msg_put_info("connection endpoint(s) missing");
		retcode = ERR_INVALID_REQUEST;
		goto out;
	}

	/* No need for _rcu here. All reconfiguration is
	 * strictly serialized on genl_lock(). We are protected against
	 * concurrent reconfiguration/addition/deletion */
	for_each_resource(resource, &drbd_resources) {
		for_each_connection(connection, resource) {
			if (nla_len(adm_ctx.my_addr) == connection->my_addr_len &&
			    !memcmp(nla_data(adm_ctx.my_addr), &connection->my_addr,
				    connection->my_addr_len)) {
				retcode = ERR_LOCAL_ADDR;
				goto out;
			}

			if (nla_len(adm_ctx.peer_addr) == connection->peer_addr_len &&
			    !memcmp(nla_data(adm_ctx.peer_addr), &connection->peer_addr,
				    connection->peer_addr_len)) {
				retcode = ERR_PEER_ADDR;
				goto out;
			}
		}
	}

	connection = adm_ctx.connection;
	conn_reconfig_start(connection);

	if (connection->cstate > C_STANDALONE) {
		retcode = ERR_NET_CONFIGURED;
		goto fail;
	}

	/* allocation not in the IO path, drbdsetup / netlink process context */
	new_conf = kzalloc(sizeof(*new_conf), GFP_KERNEL);
	if (!new_conf) {
		retcode = ERR_NOMEM;
		goto fail;
	}

	set_net_conf_defaults(new_conf);

	err = net_conf_from_attrs(new_conf, info);
	if (err && err != -ENOMSG) {
		retcode = ERR_MANDATORY_TAG;
		drbd_msg_put_info(from_attrs_err_to_txt(err));
		goto fail;
	}

	retcode = check_net_options(connection, new_conf);
	if (retcode != NO_ERROR)
		goto fail;

	retcode = alloc_crypto(&crypto, new_conf);
	if (retcode != NO_ERROR)
		goto fail;

	((char *)new_conf->shared_secret)[SHARED_SECRET_MAX-1] = 0;

	conn_flush_workqueue(connection);

	mutex_lock(&connection->conf_update);
	old_conf = connection->net_conf;
	if (old_conf) {
		retcode = ERR_NET_CONFIGURED;
		mutex_unlock(&connection->conf_update);
		goto fail;
	}
	rcu_assign_pointer(connection->net_conf, new_conf);

	conn_free_crypto(connection);
	connection->cram_hmac_tfm = crypto.cram_hmac_tfm;
	connection->integrity_tfm = crypto.integrity_tfm;
	connection->csums_tfm = crypto.csums_tfm;
	connection->verify_tfm = crypto.verify_tfm;

	connection->my_addr_len = nla_len(adm_ctx.my_addr);
	memcpy(&connection->my_addr, nla_data(adm_ctx.my_addr), connection->my_addr_len);
	connection->peer_addr_len = nla_len(adm_ctx.peer_addr);
	memcpy(&connection->peer_addr, nla_data(adm_ctx.peer_addr), connection->peer_addr_len);

	mutex_unlock(&connection->conf_update);

	rcu_read_lock();
	idr_for_each_entry(&connection->volumes, device, i) {
		device->send_cnt = 0;
		device->recv_cnt = 0;
	}
	rcu_read_unlock();

	retcode = conn_request_state(connection, NS(conn, C_UNCONNECTED), CS_VERBOSE);

	conn_reconfig_done(connection);
	drbd_adm_finish(info, retcode);
	return 0;

fail:
	free_crypto(&crypto);
	kfree(new_conf);

	conn_reconfig_done(connection);
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

static enum drbd_state_rv conn_try_disconnect(struct drbd_connection *connection, bool force)
{
	enum drbd_state_rv rv;

	rv = conn_request_state(connection, NS(conn, C_DISCONNECTING),
			force ? CS_HARD : 0);

	switch (rv) {
	case SS_NOTHING_TO_DO:
		break;
	case SS_ALREADY_STANDALONE:
		return SS_SUCCESS;
	case SS_PRIMARY_NOP:
		/* Our state checking code wants to see the peer outdated. */
		rv = conn_request_state(connection, NS2(conn, C_DISCONNECTING,
						pdsk, D_OUTDATED), CS_VERBOSE);
		break;
	case SS_CW_FAILED_BY_PEER:
		/* The peer probably wants to see us outdated. */
		rv = conn_request_state(connection, NS2(conn, C_DISCONNECTING,
							disk, D_OUTDATED), 0);
		if (rv == SS_IS_DISKLESS || rv == SS_LOWER_THAN_OUTDATED) {
			rv = conn_request_state(connection, NS(conn, C_DISCONNECTING),
					CS_HARD);
		}
		break;
	default:;
		/* no special handling necessary */
	}

	if (rv >= SS_SUCCESS) {
		enum drbd_state_rv rv2;
		/* No one else can reconfigure the network while I am here.
		 * The state handling only uses drbd_thread_stop_nowait(),
		 * we want to really wait here until the receiver is no more.
		 */
		drbd_thread_stop(&adm_ctx.connection->receiver);

		/* Race breaker.  This additional state change request may be
		 * necessary, if this was a forced disconnect during a receiver
		 * restart.  We may have "killed" the receiver thread just
		 * after drbdd_init() returned.  Typically, we should be
		 * C_STANDALONE already, now, and this becomes a no-op.
		 */
		rv2 = conn_request_state(connection, NS(conn, C_STANDALONE),
				CS_VERBOSE | CS_HARD);
		if (rv2 < SS_SUCCESS)
			conn_err(connection,
				"unexpected rv2=%d in conn_try_disconnect()\n",
				rv2);
	}
	return rv;
}

int drbd_adm_disconnect(struct sk_buff *skb, struct genl_info *info)
{
	struct disconnect_parms parms;
	struct drbd_connection *connection;
	enum drbd_state_rv rv;
	enum drbd_ret_code retcode;
	int err;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_CONNECTION);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto fail;

	connection = adm_ctx.connection;
	memset(&parms, 0, sizeof(parms));
	if (info->attrs[DRBD_NLA_DISCONNECT_PARMS]) {
		err = disconnect_parms_from_attrs(&parms, info);
		if (err) {
			retcode = ERR_MANDATORY_TAG;
			drbd_msg_put_info(from_attrs_err_to_txt(err));
			goto fail;
		}
	}

	rv = conn_try_disconnect(connection, parms.force_disconnect);
	if (rv < SS_SUCCESS)
		retcode = rv;  /* FIXME: Type mismatch. */
	else
		retcode = NO_ERROR;
 fail:
	drbd_adm_finish(info, retcode);
	return 0;
}

void resync_after_online_grow(struct drbd_device *device)
{
	int iass; /* I am sync source */

	dev_info(DEV, "Resync of new storage after online grow\n");
	if (device->state.role != device->state.peer)
		iass = (device->state.role == R_PRIMARY);
	else
		iass = test_bit(DISCARD_CONCURRENT, &first_peer_device(device)->connection->flags);

	if (iass)
		drbd_start_resync(device, C_SYNC_SOURCE);
	else
		_drbd_request_state(device, NS(conn, C_WF_SYNC_UUID), CS_VERBOSE + CS_SERIALIZE);
}

int drbd_adm_resize(struct sk_buff *skb, struct genl_info *info)
{
	struct disk_conf *old_disk_conf, *new_disk_conf = NULL;
	struct resize_parms rs;
	struct drbd_device *device;
	enum drbd_ret_code retcode;
	enum determine_dev_size dd;
	enum dds_flags ddsf;
	sector_t u_size;
	int err;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto fail;

	memset(&rs, 0, sizeof(struct resize_parms));
	if (info->attrs[DRBD_NLA_RESIZE_PARMS]) {
		err = resize_parms_from_attrs(&rs, info);
		if (err) {
			retcode = ERR_MANDATORY_TAG;
			drbd_msg_put_info(from_attrs_err_to_txt(err));
			goto fail;
		}
	}

	device = adm_ctx.device;
	if (device->state.conn > C_CONNECTED) {
		retcode = ERR_RESIZE_RESYNC;
		goto fail;
	}

	if (device->state.role == R_SECONDARY &&
	    device->state.peer == R_SECONDARY) {
		retcode = ERR_NO_PRIMARY;
		goto fail;
	}

	if (!get_ldev(device)) {
		retcode = ERR_NO_DISK;
		goto fail;
	}

	if (rs.no_resync && first_peer_device(device)->connection->agreed_pro_version < 93) {
		retcode = ERR_NEED_APV_93;
		goto fail;
	}

	rcu_read_lock();
	u_size = rcu_dereference(device->ldev->disk_conf)->disk_size;
	rcu_read_unlock();
	if (u_size != (sector_t)rs.resize_size) {
		new_disk_conf = kmalloc(sizeof(struct disk_conf), GFP_KERNEL);
		if (!new_disk_conf) {
			retcode = ERR_NOMEM;
			goto fail;
		}
	}

	if (device->ldev->known_size != drbd_get_capacity(device->ldev->backing_bdev))
		device->ldev->known_size = drbd_get_capacity(device->ldev->backing_bdev);

	if (new_disk_conf) {
		mutex_lock(&first_peer_device(device)->connection->conf_update);
		old_disk_conf = device->ldev->disk_conf;
		*new_disk_conf = *old_disk_conf;
		new_disk_conf->disk_size = (sector_t)rs.resize_size;
		rcu_assign_pointer(device->ldev->disk_conf, new_disk_conf);
		mutex_unlock(&first_peer_device(device)->connection->conf_update);
		synchronize_rcu();
		kfree(old_disk_conf);
	}

	ddsf = (rs.resize_force ? DDSF_FORCED : 0) | (rs.no_resync ? DDSF_NO_RESYNC : 0);
	dd = drbd_determine_dev_size(device, ddsf);
	drbd_md_sync(device);
	put_ldev(device);
	if (dd == dev_size_error) {
		retcode = ERR_NOMEM_BITMAP;
		goto fail;
	}

	if (device->state.conn == C_CONNECTED) {
		if (dd == grew)
			set_bit(RESIZE_PENDING, &device->flags);

		drbd_send_uuids(device);
		drbd_send_sizes(device, 1, ddsf);
	}

 fail:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_resource_opts(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;
	struct drbd_connection *connection;
	struct res_opts res_opts;
	int err;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_RESOURCE);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto fail;
	connection = adm_ctx.connection;

	res_opts = connection->res_opts;
	if (should_set_defaults(info))
		set_res_opts_defaults(&res_opts);

	err = res_opts_from_attrs(&res_opts, info);
	if (err && err != -ENOMSG) {
		retcode = ERR_MANDATORY_TAG;
		drbd_msg_put_info(from_attrs_err_to_txt(err));
		goto fail;
	}

	err = set_resource_options(connection, &res_opts);
	if (err) {
		retcode = ERR_INVALID_REQUEST;
		if (err == -ENOMEM)
			retcode = ERR_NOMEM;
	}

fail:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_invalidate(struct sk_buff *skb, struct genl_info *info)
{
	struct drbd_device *device;
	int retcode; /* enum drbd_ret_code rsp. enum drbd_state_rv */

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	device = adm_ctx.device;

	/* If there is still bitmap IO pending, probably because of a previous
	 * resync just being finished, wait for it before requesting a new resync. */
	wait_event(device->misc_wait, !test_bit(BITMAP_IO, &device->flags));

	retcode = _drbd_request_state(device, NS(conn, C_STARTING_SYNC_T), CS_ORDERED);

	if (retcode < SS_SUCCESS && retcode != SS_NEED_CONNECTION)
		retcode = drbd_request_state(device, NS(conn, C_STARTING_SYNC_T));

	while (retcode == SS_NEED_CONNECTION) {
		spin_lock_irq(&first_peer_device(device)->connection->req_lock);
		if (device->state.conn < C_CONNECTED)
			retcode = _drbd_set_state(_NS(device, disk, D_INCONSISTENT), CS_VERBOSE, NULL);
		spin_unlock_irq(&first_peer_device(device)->connection->req_lock);

		if (retcode != SS_NEED_CONNECTION)
			break;

		retcode = drbd_request_state(device, NS(conn, C_STARTING_SYNC_T));
	}

out:
	drbd_adm_finish(info, retcode);
	return 0;
}

static int drbd_adm_simple_request_state(struct sk_buff *skb, struct genl_info *info,
		union drbd_state mask, union drbd_state val)
{
	enum drbd_ret_code retcode;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	retcode = drbd_request_state(adm_ctx.device, mask, val);
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_invalidate_peer(struct sk_buff *skb, struct genl_info *info)
{
	return drbd_adm_simple_request_state(skb, info, NS(conn, C_STARTING_SYNC_S));
}

int drbd_adm_pause_sync(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	if (drbd_request_state(adm_ctx.device, NS(user_isp, 1)) == SS_NOTHING_TO_DO)
		retcode = ERR_PAUSE_IS_SET;
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_resume_sync(struct sk_buff *skb, struct genl_info *info)
{
	union drbd_dev_state s;
	enum drbd_ret_code retcode;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	if (drbd_request_state(adm_ctx.device, NS(user_isp, 0)) == SS_NOTHING_TO_DO) {
		s = adm_ctx.device->state;
		if (s.conn == C_PAUSED_SYNC_S || s.conn == C_PAUSED_SYNC_T) {
			retcode = s.aftr_isp ? ERR_PIC_AFTER_DEP :
				  s.peer_isp ? ERR_PIC_PEER_DEP : ERR_PAUSE_IS_CLEAR;
		} else {
			retcode = ERR_PAUSE_IS_CLEAR;
		}
	}

out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_suspend_io(struct sk_buff *skb, struct genl_info *info)
{
	return drbd_adm_simple_request_state(skb, info, NS(susp, 1));
}

int drbd_adm_resume_io(struct sk_buff *skb, struct genl_info *info)
{
	struct drbd_device *device;
	int retcode; /* enum drbd_ret_code rsp. enum drbd_state_rv */

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	device = adm_ctx.device;
	if (test_bit(NEW_CUR_UUID, &device->flags)) {
		drbd_uuid_new_current(device);
		clear_bit(NEW_CUR_UUID, &device->flags);
	}
	drbd_suspend_io(device);
	retcode = drbd_request_state(device, NS3(susp, 0, susp_nod, 0, susp_fen, 0));
	if (retcode == SS_SUCCESS) {
		if (device->state.conn < C_CONNECTED)
			tl_clear(first_peer_device(device)->connection);
		if (device->state.disk == D_DISKLESS || device->state.disk == D_FAILED)
			tl_restart(first_peer_device(device)->connection, FAIL_FROZEN_DISK_IO);
	}
	drbd_resume_io(device);

out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_outdate(struct sk_buff *skb, struct genl_info *info)
{
	return drbd_adm_simple_request_state(skb, info, NS(disk, D_OUTDATED));
}

int nla_put_drbd_cfg_context(struct sk_buff *skb, struct drbd_connection *connection, unsigned vnr)
{
	struct nlattr *nla;
	nla = nla_nest_start(skb, DRBD_NLA_CFG_CONTEXT);
	if (!nla)
		goto nla_put_failure;
	if (vnr != VOLUME_UNSPECIFIED)
		NLA_PUT_U32(skb, T_ctx_volume, vnr);
	NLA_PUT_STRING(skb, T_ctx_resource_name, connection->resource->name);
	if (connection->my_addr_len)
		NLA_PUT(skb, T_ctx_my_addr,
			connection->my_addr_len, &connection->my_addr);
	if (connection->peer_addr_len)
		NLA_PUT(skb, T_ctx_peer_addr,
			connection->peer_addr_len, &connection->peer_addr);
	nla_nest_end(skb, nla);
	return 0;

nla_put_failure:
	if (nla)
		nla_nest_cancel(skb, nla);
	return -EMSGSIZE;
}

int nla_put_status_info(struct sk_buff *skb, struct drbd_device *device,
		const struct sib_info *sib)
{
	struct state_info *si = NULL; /* for sizeof(si->member); */
	struct net_conf *nc;
	struct nlattr *nla;
	int got_ldev;
	int err = 0;
	int exclude_sensitive;

	/* If sib != NULL, this is drbd_bcast_event, which anyone can listen
	 * to.  So we better exclude_sensitive information.
	 *
	 * If sib == NULL, this is drbd_adm_get_status, executed synchronously
	 * in the context of the requesting user process. Exclude sensitive
	 * information, unless current has superuser.
	 *
	 * NOTE: for drbd_adm_get_status_all(), this is a netlink dump, and
	 * relies on the current implementation of netlink_dump(), which
	 * executes the dump callback successively from netlink_recvmsg(),
	 * always in the context of the receiving process */
	exclude_sensitive = sib || !capable(CAP_SYS_ADMIN);

	got_ldev = get_ldev(device);

	/* We need to add connection name and volume number information still.
	 * Minor number is in drbd_genlmsghdr. */
	if (nla_put_drbd_cfg_context(skb, first_peer_device(device)->connection, device->vnr))
		goto nla_put_failure;

	if (res_opts_to_skb(skb, &first_peer_device(device)->connection->res_opts, exclude_sensitive))
		goto nla_put_failure;

	rcu_read_lock();
	if (got_ldev)
		if (disk_conf_to_skb(skb, rcu_dereference(device->ldev->disk_conf), exclude_sensitive))
			goto nla_put_failure;

	nc = rcu_dereference(first_peer_device(device)->connection->net_conf);
	if (nc)
		err = net_conf_to_skb(skb, nc, exclude_sensitive);
	rcu_read_unlock();
	if (err)
		goto nla_put_failure;

	nla = nla_nest_start(skb, DRBD_NLA_STATE_INFO);
	if (!nla)
		goto nla_put_failure;
	NLA_PUT_U32(skb, T_sib_reason, sib ? sib->sib_reason : SIB_GET_STATUS_REPLY);
	NLA_PUT_U32(skb, T_current_state, device->state.i);
	NLA_PUT_U64(skb, T_ed_uuid, device->ed_uuid);
	NLA_PUT_U64(skb, T_capacity, drbd_get_capacity(device->this_bdev));

	if (got_ldev) {
		NLA_PUT_U32(skb, T_disk_flags, device->ldev->md.flags);
		NLA_PUT(skb, T_uuids, sizeof(si->uuids), device->ldev->md.uuid);
		NLA_PUT_U64(skb, T_bits_total, drbd_bm_bits(device));
		NLA_PUT_U64(skb, T_bits_oos, drbd_bm_total_weight(device));
		if (C_SYNC_SOURCE <= device->state.conn &&
		    C_PAUSED_SYNC_T >= device->state.conn) {
			NLA_PUT_U64(skb, T_bits_rs_total, device->rs_total);
			NLA_PUT_U64(skb, T_bits_rs_failed, device->rs_failed);
		}
	}

	if (sib) {
		switch(sib->sib_reason) {
		case SIB_SYNC_PROGRESS:
		case SIB_GET_STATUS_REPLY:
			break;
		case SIB_STATE_CHANGE:
			NLA_PUT_U32(skb, T_prev_state, sib->os.i);
			NLA_PUT_U32(skb, T_new_state, sib->ns.i);
			break;
		case SIB_HELPER_POST:
			NLA_PUT_U32(skb,
				T_helper_exit_code, sib->helper_exit_code);
			/* fall through */
		case SIB_HELPER_PRE:
			NLA_PUT_STRING(skb, T_helper, sib->helper_name);
			break;
		}
	}
	nla_nest_end(skb, nla);

	if (0)
nla_put_failure:
		err = -EMSGSIZE;
	if (got_ldev)
		put_ldev(device);
	return err;
}

int drbd_adm_get_status(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;
	int err;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	err = nla_put_status_info(adm_ctx.reply_skb, adm_ctx.device, NULL);
	if (err) {
		nlmsg_free(adm_ctx.reply_skb);
		return err;
	}
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

static int get_one_status(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct drbd_device *device;
	struct drbd_genlmsghdr *dh;
	struct drbd_resource *pos = (struct drbd_resource *)cb->args[0];
	struct drbd_resource *resource = NULL;
	struct drbd_connection *connection;
	struct drbd_resource *tmp;
	unsigned volume = cb->args[1];

	/* Open coded, deferred, iteration:
	 * for_each_resource_safe(resource, tmp, &drbd_resources) {
	 *      connection = "first connection of resource";
	 *	idr_for_each_entry(&connection->volumes, device, i) {
	 *	  ...
	 *	}
	 * }
	 * where resource is cb->args[0];
	 * and i is cb->args[1];
	 *
	 * cb->args[2] indicates if we shall loop over all resources,
	 * or just dump all volumes of a single resource.
	 *
	 * This may miss entries inserted after this dump started,
	 * or entries deleted before they are reached.
	 *
	 * We need to make sure the device won't disappear while
	 * we are looking at it, and revalidate our iterators
	 * on each iteration.
	 */

	/* synchronize with conn_create()/drbd_destroy_connection() */
	rcu_read_lock();
	/* revalidate iterator position */
	for_each_resource_rcu(tmp, &drbd_resources) {
		if (pos == NULL) {
			/* first iteration */
			pos = tmp;
			resource = pos;
			break;
		}
		if (tmp == pos) {
			resource = pos;
			break;
		}
	}
	if (resource) {
next_resource:
		connection = first_connection(resource);
		device = idr_get_next(&connection->volumes, &volume);
		if (!device) {
			/* No more volumes to dump on this resource.
			 * Advance resource iterator. */
			pos = list_entry_rcu(resource->resources.next,
					     struct drbd_resource, resources);
			/* Did we dump any volume of this resource yet? */
			if (volume != 0) {
				/* If we reached the end of the list,
				 * or only a single resource dump was requested,
				 * we are done. */
				if (&pos->resources == &drbd_resources || cb->args[2])
					goto out;
				volume = 0;
				resource = pos;
				goto next_resource;
			}
		}

		dh = genlmsg_put(skb, NETLINK_CB(cb->skb).pid,
				cb->nlh->nlmsg_seq, &drbd_genl_family,
				NLM_F_MULTI, DRBD_ADM_GET_STATUS);
		if (!dh)
			goto out;

		if (!device) {
			/* This is a connection without a single volume.
			 * Suprisingly enough, it may have a network
			 * configuration. */
			struct net_conf *nc;
			dh->minor = -1U;
			dh->ret_code = NO_ERROR;
			if (nla_put_drbd_cfg_context(skb, connection, VOLUME_UNSPECIFIED))
				goto cancel;
			nc = rcu_dereference(connection->net_conf);
			if (nc && net_conf_to_skb(skb, nc, 1) != 0)
				goto cancel;
			goto done;
		}

		D_ASSERT(device->vnr == volume);
		D_ASSERT(first_peer_device(device)->connection == connection);

		dh->minor = mdev_to_minor(device);
		dh->ret_code = NO_ERROR;

		if (nla_put_status_info(skb, device, NULL)) {
cancel:
			genlmsg_cancel(skb, dh);
			goto out;
		}
done:
		genlmsg_end(skb, dh);
        }

out:
	rcu_read_unlock();
	/* where to start the next iteration */
        cb->args[0] = (long)pos;
        cb->args[1] = (pos == resource) ? volume + 1 : 0;

	/* No more resources/volumes/minors found results in an empty skb.
	 * Which will terminate the dump. */
        return skb->len;
}

/*
 * Request status of all resources, or of all volumes within a single resource.
 *
 * This is a dump, as the answer may not fit in a single reply skb otherwise.
 * Which means we cannot use the family->attrbuf or other such members, because
 * dump is NOT protected by the genl_lock().  During dump, we only have access
 * to the incoming skb, and need to opencode "parsing" of the nlattr payload.
 *
 * Once things are setup properly, we call into get_one_status().
 */
int drbd_adm_get_status_all(struct sk_buff *skb, struct netlink_callback *cb)
{
	const unsigned hdrlen = GENL_HDRLEN + GENL_MAGIC_FAMILY_HDRSZ;
	struct nlattr *nla;
	const char *resource_name;
	struct drbd_connection *connection;
	int maxtype;

	/* Is this a followup call? */
	if (cb->args[0]) {
		/* ... of a single resource dump,
		 * and the resource iterator has been advanced already? */
		if (cb->args[2] && cb->args[2] != cb->args[0])
			return 0; /* DONE. */
		goto dump;
	}

	/* First call (from netlink_dump_start).  We need to figure out
	 * which resource(s) the user wants us to dump. */
	nla = nla_find(nlmsg_attrdata(cb->nlh, hdrlen),
			nlmsg_attrlen(cb->nlh, hdrlen),
			DRBD_NLA_CFG_CONTEXT);

	/* No explicit context given.  Dump all. */
	if (!nla)
		goto dump;
	maxtype = ARRAY_SIZE(drbd_cfg_context_nl_policy) - 1;
	nla = drbd_nla_find_nested(maxtype, nla, __nla_type(T_ctx_resource_name));
	if (IS_ERR(nla))
		return PTR_ERR(nla);
	/* context given, but no name present? */
	if (!nla)
		return -EINVAL;
	resource_name = nla_data(nla);
	connection = conn_get_by_name(resource_name);

	if (!connection)
		return -ENODEV;

	kref_put(&connection->kref, drbd_destroy_connection); /* get_one_status() (re)validates connection by itself */

	/* prime iterators, and set "filter" mode mark:
	 * only dump this connection. */
	cb->args[0] = (long)connection;
	/* cb->args[1] = 0; passed in this way. */
	cb->args[2] = (long)connection;

dump:
	return get_one_status(skb, cb);
}

int drbd_adm_get_timeout_type(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;
	struct timeout_parms tp;
	int err;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	tp.timeout_type =
		adm_ctx.device->state.pdsk == D_OUTDATED ? UT_PEER_OUTDATED :
		test_bit(USE_DEGR_WFC_T, &adm_ctx.device->flags) ? UT_DEGRADED :
		UT_DEFAULT;

	err = timeout_parms_to_priv_skb(adm_ctx.reply_skb, &tp);
	if (err) {
		nlmsg_free(adm_ctx.reply_skb);
		return err;
	}
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_start_ov(struct sk_buff *skb, struct genl_info *info)
{
	struct drbd_device *device;
	enum drbd_ret_code retcode;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	device = adm_ctx.device;
	if (info->attrs[DRBD_NLA_START_OV_PARMS]) {
		/* resume from last known position, if possible */
		struct start_ov_parms parms =
			{ .ov_start_sector = device->ov_start_sector };
		int err = start_ov_parms_from_attrs(&parms, info);
		if (err) {
			retcode = ERR_MANDATORY_TAG;
			drbd_msg_put_info(from_attrs_err_to_txt(err));
			goto out;
		}
		/* w_make_ov_request expects position to be aligned */
		device->ov_start_sector = parms.ov_start_sector & ~BM_SECT_PER_BIT;
	}
	/* If there is still bitmap IO pending, e.g. previous resync or verify
	 * just being finished, wait for it before requesting a new resync. */
	wait_event(device->misc_wait, !test_bit(BITMAP_IO, &device->flags));
	retcode = drbd_request_state(device,NS(conn,C_VERIFY_S));
out:
	drbd_adm_finish(info, retcode);
	return 0;
}


int drbd_adm_new_c_uuid(struct sk_buff *skb, struct genl_info *info)
{
	struct drbd_device *device;
	enum drbd_ret_code retcode;
	int skip_initial_sync = 0;
	int err;
	struct new_c_uuid_parms args;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out_nolock;

	device = adm_ctx.device;
	memset(&args, 0, sizeof(args));
	if (info->attrs[DRBD_NLA_NEW_C_UUID_PARMS]) {
		err = new_c_uuid_parms_from_attrs(&args, info);
		if (err) {
			retcode = ERR_MANDATORY_TAG;
			drbd_msg_put_info(from_attrs_err_to_txt(err));
			goto out_nolock;
		}
	}

	mutex_lock(device->state_mutex); /* Protects us against serialized state changes. */

	if (!get_ldev(device)) {
		retcode = ERR_NO_DISK;
		goto out;
	}

	/* this is "skip initial sync", assume to be clean */
	if (device->state.conn == C_CONNECTED &&
	    first_peer_device(device)->connection->agreed_pro_version >= 90 &&
	    device->ldev->md.uuid[UI_CURRENT] == UUID_JUST_CREATED && args.clear_bm) {
		dev_info(DEV, "Preparing to skip initial sync\n");
		skip_initial_sync = 1;
	} else if (device->state.conn != C_STANDALONE) {
		retcode = ERR_CONNECTED;
		goto out_dec;
	}

	drbd_uuid_set(device, UI_BITMAP, 0); /* Rotate UI_BITMAP to History 1, etc... */
	drbd_uuid_new_current(device); /* New current, previous to UI_BITMAP */

	if (args.clear_bm) {
		err = drbd_bitmap_io(device, &drbd_bmio_clear_n_write,
			"clear_n_write from new_c_uuid", BM_LOCKED_MASK);
		if (err) {
			dev_err(DEV, "Writing bitmap failed with %d\n",err);
			retcode = ERR_IO_MD_DISK;
		}
		if (skip_initial_sync) {
			drbd_send_uuids_skip_initial_sync(device);
			_drbd_uuid_set(device, UI_BITMAP, 0);
			drbd_print_uuids(device, "cleared bitmap UUID");
			spin_lock_irq(&first_peer_device(device)->connection->req_lock);
			_drbd_set_state(_NS2(device, disk, D_UP_TO_DATE, pdsk, D_UP_TO_DATE),
					CS_VERBOSE, NULL);
			spin_unlock_irq(&first_peer_device(device)->connection->req_lock);
		}
	}

	drbd_md_sync(device);
out_dec:
	put_ldev(device);
out:
	mutex_unlock(device->state_mutex);
out_nolock:
	drbd_adm_finish(info, retcode);
	return 0;
}

static enum drbd_ret_code
drbd_check_resource_name(const char *name)
{
	if (!name || !name[0]) {
		drbd_msg_put_info("resource name missing");
		return ERR_MANDATORY_TAG;
	}
	/* if we want to use these in sysfs/configfs/debugfs some day,
	 * we must not allow slashes */
	if (strchr(name, '/')) {
		drbd_msg_put_info("invalid resource name");
		return ERR_INVALID_REQUEST;
	}
	return NO_ERROR;
}

int drbd_adm_new_resource(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;
	struct res_opts res_opts;
	int err;

	retcode = drbd_adm_prepare(skb, info, 0);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	set_res_opts_defaults(&res_opts);
	err = res_opts_from_attrs(&res_opts, info);
	if (err && err != -ENOMSG) {
		retcode = ERR_MANDATORY_TAG;
		drbd_msg_put_info(from_attrs_err_to_txt(err));
		goto out;
	}

	retcode = drbd_check_resource_name(adm_ctx.resource_name);
	if (retcode != NO_ERROR)
		goto out;

	if (adm_ctx.connection)
		goto out;

	if (!conn_create(adm_ctx.resource_name, &res_opts))
		retcode = ERR_NOMEM;
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_new_minor(struct sk_buff *skb, struct genl_info *info)
{
	struct drbd_genlmsghdr *dh = info->userhdr;
	enum drbd_ret_code retcode;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_RESOURCE);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	if (dh->minor > MINORMASK) {
		drbd_msg_put_info("requested minor out of range");
		retcode = ERR_INVALID_REQUEST;
		goto out;
	}
	if (adm_ctx.volume > DRBD_VOLUME_MAX) {
		drbd_msg_put_info("requested volume id out of range");
		retcode = ERR_INVALID_REQUEST;
		goto out;
	}

	if (adm_ctx.device)
		goto out;

	retcode = drbd_create_minor(adm_ctx.connection, dh->minor, adm_ctx.volume);
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

static enum drbd_ret_code adm_del_minor(struct drbd_device *device)
{
	if (device->state.disk == D_DISKLESS &&
	    /* no need to be device->state.conn == C_STANDALONE &&
	     * we may want to delete a minor from a live replication group.
	     */
	    device->state.role == R_SECONDARY) {
		_drbd_request_state(device, NS(conn, C_WF_REPORT_PARAMS),
				    CS_VERBOSE + CS_WAIT_COMPLETE);
		drbd_delete_minor(device);
		return NO_ERROR;
	} else
		return ERR_MINOR_CONFIGURED;
}

int drbd_adm_del_minor(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_MINOR);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	retcode = adm_del_minor(adm_ctx.device);
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_down(struct sk_buff *skb, struct genl_info *info)
{
	int retcode; /* enum drbd_ret_code rsp. enum drbd_state_rv */
	struct drbd_device *device;
	unsigned i;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_RESOURCE);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	/* demote */
	idr_for_each_entry(&adm_ctx.connection->volumes, device, i) {
		retcode = drbd_set_role(device, R_SECONDARY, 0);
		if (retcode < SS_SUCCESS) {
			drbd_msg_put_info("failed to demote");
			goto out;
		}
	}

	retcode = conn_try_disconnect(adm_ctx.connection, 0);
	if (retcode < SS_SUCCESS) {
		drbd_msg_put_info("failed to disconnect");
		goto out;
	}

	/* detach */
	idr_for_each_entry(&adm_ctx.connection->volumes, device, i) {
		retcode = adm_detach(device, 0);
		if (retcode < SS_SUCCESS) {
			drbd_msg_put_info("failed to detach");
			goto out;
		}
	}

	/* If we reach this, all volumes (of this connection) are Secondary,
	 * Disconnected, Diskless, aka Unconfigured. Make sure all threads have
	 * actually stopped, state handling only does drbd_thread_stop_nowait(). */
	drbd_thread_stop(&adm_ctx.connection->worker);

	/* Now, nothing can fail anymore */

	/* delete volumes */
	idr_for_each_entry(&adm_ctx.connection->volumes, device, i) {
		retcode = adm_del_minor(device);
		if (retcode != NO_ERROR) {
			/* "can not happen" */
			drbd_msg_put_info("failed to delete volume");
			goto out;
		}
	}

	/* delete connection */
	if (conn_lowest_minor(adm_ctx.connection) < 0) {
		struct drbd_resource *resource = adm_ctx.connection->resource;

		list_del_rcu(&resource->resources);
		synchronize_rcu();
		drbd_free_resource(resource);

		retcode = NO_ERROR;
	} else {
		/* "can not happen" */
		retcode = ERR_RES_IN_USE;
		drbd_msg_put_info("failed to delete connection");
	}
	goto out;
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

int drbd_adm_del_resource(struct sk_buff *skb, struct genl_info *info)
{
	enum drbd_ret_code retcode;

	retcode = drbd_adm_prepare(skb, info, DRBD_ADM_NEED_RESOURCE);
	if (!adm_ctx.reply_skb)
		return retcode;
	if (retcode != NO_ERROR)
		goto out;

	if (conn_lowest_minor(adm_ctx.connection) < 0) {
		struct drbd_resource *resource = adm_ctx.connection->resource;

		list_del_rcu(&resource->resources);
		synchronize_rcu();
		drbd_free_resource(resource);

		retcode = NO_ERROR;
	} else {
		retcode = ERR_RES_IN_USE;
	}

	if (retcode == NO_ERROR)
		drbd_thread_stop(&adm_ctx.connection->worker);
out:
	drbd_adm_finish(info, retcode);
	return 0;
}

void drbd_bcast_event(struct drbd_device *device, const struct sib_info *sib)
{
	static atomic_t drbd_genl_seq = ATOMIC_INIT(2); /* two. */
	struct sk_buff *msg;
	struct drbd_genlmsghdr *d_out;
	unsigned seq;
	int err = -ENOMEM;

	seq = atomic_inc_return(&drbd_genl_seq);
	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_NOIO);
	if (!msg)
		goto failed;

	err = -EMSGSIZE;
	d_out = genlmsg_put(msg, 0, seq, &drbd_genl_family, 0, DRBD_EVENT);
	if (!d_out) /* cannot happen, but anyways. */
		goto nla_put_failure;
	d_out->minor = mdev_to_minor(device);
	d_out->ret_code = NO_ERROR;

	if (nla_put_status_info(msg, device, sib))
		goto nla_put_failure;
	genlmsg_end(msg, d_out);
	err = drbd_genl_multicast_events(msg, 0);
	/* msg has been consumed or freed in netlink_broadcast() */
	if (err && err != -ESRCH)
		goto failed;

	return;

nla_put_failure:
	nlmsg_free(msg);
failed:
	dev_err(DEV, "Error %d while broadcasting event. "
			"Event seq:%u sib_reason:%u\n",
			err, seq, sib->sib_reason);
}
