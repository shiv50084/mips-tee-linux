/*
 * Copyright (c) 2017-2018, MIPS Tech, LLC and/or its affiliated group companies
 * (“MIPS”).
 * Copyright (c) 2015, Linaro Limited
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/fs.h>
#include "mipstee_private.h"
#include "tipc_private.h"

/* Requires the filpstate mutex to be held */
static struct mipstee_session *find_session(struct mipstee_context_data *ctxdata,
					  u32 session_id)
{
	struct mipstee_session *sess;

	list_for_each_entry(sess, &ctxdata->sess_list, list_node)
		if (sess->session_id == session_id)
			return sess;

	return NULL;
}

/**
 * mipstee_do_call_with_arg() - Send message to TEE
 * @ctx:	calling context
 * @msg_arg:	ptr to message to send
 *
 * Returns 0 on success or <0 on failure
 */
u32 mipstee_do_call_with_arg(struct tee_context *ctx,
			     struct mipstee_msg_arg *msg_arg)
{
	struct mipstee_context_data *ctxdata = ctx->data;
	struct tipc_dn_chan *channel = ctxdata->cmd_ch;
	size_t msg_len;
	int rc;

	pr_devel("%s ctx %p sess %u\n", __func__, ctx, msg_arg->session);

	msg_len = MIPSTEE_MSG_GET_ARG_SIZE(msg_arg->num_params);

	if (msg_arg->cmd == MIPSTEE_MSG_CMD_CANCEL)
		rc = tipc_write(channel, msg_arg, msg_len);
	else
		rc = tipc_call(channel, msg_arg, msg_len);

	if (rc < 0) {
		pr_err("%s failed cmd %u sess %u err %d\n", __func__,
				msg_arg->cmd, msg_arg->session, rc);
		return rc;
	}
	return 0;
}

static struct tee_shm *get_msg_arg(struct tee_context *ctx, size_t num_params,
				   struct mipstee_msg_arg **msg_arg,
				   phys_addr_t *msg_parg)
{
	int rc;
	struct tee_shm *shm;
	struct mipstee_msg_arg *ma;

	shm = tee_shm_alloc(ctx, MIPSTEE_MSG_GET_ARG_SIZE(num_params),
			    TEE_SHM_MAPPED);
	if (IS_ERR(shm))
		return shm;

	ma = tee_shm_get_va(shm, 0);
	if (IS_ERR(ma)) {
		rc = PTR_ERR(ma);
		goto out;
	}

	rc = tee_shm_get_pa(shm, 0, msg_parg);
	if (rc)
		goto out;

	memset(ma, 0, MIPSTEE_MSG_GET_ARG_SIZE(num_params));
	ma->num_params = num_params;
	*msg_arg = ma;
out:
	if (rc) {
		tee_shm_free(shm);
		return ERR_PTR(rc);
	}

	return shm;
}

static int mipstee_authenticate_client(struct mipstee_msg_arg *msg_arg,
				   u32 clnt_login)
{
	u8 clnt_uuid[TEE_IOCTL_UUID_LEN] = { 0 };
	// u64 *tmp = &value->c;

	switch(clnt_login) {
	/* TODO: Implement actual authorization based upon TEEC_LOGIN_TYPE */
	case TEEC_LOGIN_PUBLIC:
	/*
	 * The client is in the Rich Execution Environment and is neither
	 * identified nor authenticated. The client has no identity and the UUID is
	 * the Nil UUID.
	 */
	case TEEC_LOGIN_USER:
	/*
	 * The Client Application has been identified by the Rich Execution
	 * Environment and the client UUID reflects the actual user that runs the
	 * calling application independently of the actual application.
	 */
	case TEEC_LOGIN_GROUP:
	/*
	 * The client UUID reflects a group identity that is executing the calling
	 * application. The notion of group identity and the corresponding UUID is
	 * REE-specific.
	 */
	case TEEC_LOGIN_APPLICATION:
	/*
	 * The Client Application has been identified by the Rich Execution
	 * Environment independently of the identity of the user executing the
	 * application. The nature of this identification and the corresponding UUID
	 * is REE-specific.
	 */
	case TEEC_LOGIN_USER_APPLICATION:
	/*
	 * The client UUID identifies both the calling application and the user that
	 * is executing it.
	 */
	case TEEC_LOGIN_GROUP_APPLICATION:
	/*
	 * The client UUID identifies both the calling application and a group that
	 * is executing it.
	 */
		break;
	default:
		return EACCES;
	}

	memcpy(&msg_arg->params[1].u.value, clnt_uuid, sizeof(clnt_uuid));
	msg_arg->params[1].u.value.c = clnt_login;

	return 0;
}

static int match_cancel_id(int id, void *p, void *data)
{
	if (p == data)
		return id;

	return 0;
}

static int mipstee_alloc_cancel_idr(struct mipstee_context_data *ctxdata,
				    u32 cancel_id)
{
	int idr_id = 0;

	if (!cancel_id)
		goto out;

	mutex_lock(&ctxdata->mutex);

	/* detect cancel_id collision */
	idr_id = idr_for_each(&ctxdata->cancel_idr, match_cancel_id,
			(void *)cancel_id);
	if (idr_id)
		idr_id = -EINVAL;
	else
		idr_id = idr_alloc_cyclic(&ctxdata->cancel_idr,
				(void *)cancel_id, 1, 0, GFP_KERNEL);

	mutex_unlock(&ctxdata->mutex);
out:
	pr_devel("%s cancel_id %x idr_id %d\n", __func__, cancel_id, idr_id);
	return idr_id;
}

static void mipstee_remove_cancel_idr(struct mipstee_context_data *ctxdata,
				      int idr_id)
{
	pr_devel("%s idr_id %d\n", __func__, idr_id);

	if (!idr_id)
		return;

	mutex_lock(&ctxdata->mutex);
	idr_remove(&ctxdata->cancel_idr, idr_id);
	mutex_unlock(&ctxdata->mutex);
}

static int mipstee_find_cancel_idr(struct mipstee_context_data *ctxdata,
				   u32 cancel_id)
{
	int idr_id = 0;

	if (!cancel_id)
		goto out;

	mutex_lock(&ctxdata->mutex);
	idr_id = idr_for_each(&ctxdata->cancel_idr, match_cancel_id,
			(void *)cancel_id);
	mutex_unlock(&ctxdata->mutex);
out:
	pr_devel("%s cancel_id %x idr_id %d\n", __func__, cancel_id, idr_id);
	return idr_id;
}

int mipstee_open_session(struct tee_context *ctx,
		       struct tee_ioctl_open_session_arg *arg,
		       struct tee_param *param)
{
	struct mipstee_context_data *ctxdata = ctx->data;
	struct mipstee *mipstee = tee_get_drvdata(ctx->teedev);
	int rc;
	struct tee_shm *shm;
	struct mipstee_msg_arg *msg_arg;
	struct mipstee_session *sess = NULL;
	int idr_id = 0;

	pr_devel("%s ctx %p\n", __func__, ctx);

	/* +2 for the meta parameters added below */
	shm = get_msg_arg(ctx, arg->num_params + 2, &msg_arg, NULL);
	if (IS_ERR(shm))
		return PTR_ERR(shm);

	msg_arg->cmd = MIPSTEE_MSG_CMD_OPEN_SESSION;

	rc = mipstee_alloc_cancel_idr(ctxdata, arg->cancel_id);
	if (rc < 0)
		goto out;
	idr_id = rc;
	msg_arg->cancel_id = idr_id;

	/*
	 * Initialize and add the meta parameters needed when opening a
	 * session.
	 */
	msg_arg->params[0].attr = MIPSTEE_MSG_ATTR_TYPE_VALUE_INPUT |
				  MIPSTEE_MSG_ATTR_META;
	msg_arg->params[1].attr = MIPSTEE_MSG_ATTR_TYPE_VALUE_INPUT |
				  MIPSTEE_MSG_ATTR_META;
	memcpy(&msg_arg->params[0].u.value, arg->uuid, sizeof(arg->uuid));

	/* Authenticate client identity */
	rc = mipstee_authenticate_client(msg_arg, arg->clnt_login);
	if (rc) {
		pr_err("\nAuthentication error!!!\n");
		goto out;
	}

	rc = mipstee_to_msg_param(msg_arg->params + 2, arg->num_params,
			param, mipstee->shm_base);
	if (rc)
		goto out;

	sess = kzalloc(sizeof(*sess), GFP_KERNEL);
	if (!sess) {
		rc = -ENOMEM;
		goto out;
	}

	if (mipstee_do_call_with_arg(ctx, msg_arg)) {
		msg_arg->ret = TEEC_ERROR_COMMUNICATION;
		msg_arg->ret_origin = TEEC_ORIGIN_COMMS;
	}

	if (msg_arg->ret == TEEC_SUCCESS) {
		/* A new session has been created, add it to the list. */
		sess->session_id = msg_arg->session;
		mutex_lock(&ctxdata->mutex);
		list_add(&sess->list_node, &ctxdata->sess_list);
		mutex_unlock(&ctxdata->mutex);
	} else {
		kfree(sess);
	}

	if (mipstee_from_msg_param(param, arg->num_params, msg_arg->params + 2)) {
		pr_devel("%s msg_param error ctx %p sess %u ret code %x\n",
				__func__, ctx, msg_arg->session, msg_arg->ret);
		arg->ret = TEEC_ERROR_COMMUNICATION;
		arg->ret_origin = TEEC_ORIGIN_COMMS;
		/* Close session again to avoid leakage */
		mipstee_close_session(ctx, msg_arg->session);
	} else {
		arg->session = msg_arg->session;
		arg->ret = msg_arg->ret;
		arg->ret_origin = msg_arg->ret_origin;
	}
out:
	mipstee_remove_cancel_idr(ctxdata, idr_id);
	tee_shm_free(shm);

	pr_devel("%s done ctx %p sess %u\n", __func__, ctx, arg->session);
	return rc;
}

int mipstee_close_session(struct tee_context *ctx, u32 session)
{
	struct mipstee_context_data *ctxdata = ctx->data;
	struct tee_shm *shm;
	struct mipstee_msg_arg *msg_arg;
	struct mipstee_session *sess;

	pr_devel("%s ctx %p sess %u\n", __func__, ctx, session);

	/* Check that the session is valid and remove it from the list */
	mutex_lock(&ctxdata->mutex);
	sess = find_session(ctxdata, session);
	if (sess)
		list_del(&sess->list_node);
	mutex_unlock(&ctxdata->mutex);
	if (!sess)
		return -EINVAL;
	kfree(sess);

	shm = get_msg_arg(ctx, 0, &msg_arg, NULL);
	if (IS_ERR(shm))
		return PTR_ERR(shm);

	msg_arg->cmd = MIPSTEE_MSG_CMD_CLOSE_SESSION;
	msg_arg->session = session;
	mipstee_do_call_with_arg(ctx, msg_arg);

	tee_shm_free(shm);
	pr_devel("%s done ctx %p\n", __func__, ctx);
	return 0;
}

int mipstee_invoke_func(struct tee_context *ctx, struct tee_ioctl_invoke_arg *arg,
		      struct tee_param *param)
{
	struct mipstee_context_data *ctxdata = ctx->data;
	struct mipstee *mipstee = tee_get_drvdata(ctx->teedev);
	struct tee_shm *shm;
	struct mipstee_msg_arg *msg_arg;
	struct mipstee_session *sess;
	int idr_id = 0;
	int rc;

	pr_devel("%s ctx %p sess %u\n", __func__, ctx, arg->session);

	/* Check that the session is valid */
	mutex_lock(&ctxdata->mutex);
	sess = find_session(ctxdata, arg->session);
	mutex_unlock(&ctxdata->mutex);
	if (!sess)
		return -EINVAL;

	shm = get_msg_arg(ctx, arg->num_params, &msg_arg, NULL);
	if (IS_ERR(shm))
		return PTR_ERR(shm);
	msg_arg->cmd = MIPSTEE_MSG_CMD_INVOKE_COMMAND;
	msg_arg->func = arg->func;
	msg_arg->session = arg->session;

	rc = mipstee_alloc_cancel_idr(ctxdata, arg->cancel_id);
	if (rc < 0)
		goto out;
	idr_id = rc;
	msg_arg->cancel_id = idr_id;

	rc = mipstee_to_msg_param(msg_arg->params, arg->num_params,
			param, mipstee->shm_base);
	if (rc)
		goto out;

	if (mipstee_do_call_with_arg(ctx, msg_arg)) {
		msg_arg->ret = TEEC_ERROR_COMMUNICATION;
		msg_arg->ret_origin = TEEC_ORIGIN_COMMS;
	}

	if (mipstee_from_msg_param(param, arg->num_params, msg_arg->params)) {
		msg_arg->ret = TEEC_ERROR_COMMUNICATION;
		msg_arg->ret_origin = TEEC_ORIGIN_COMMS;
	}

	arg->ret = msg_arg->ret;
	arg->ret_origin = msg_arg->ret_origin;
out:
	mipstee_remove_cancel_idr(ctxdata, idr_id);
	tee_shm_free(shm);
	pr_devel("%s done ctx %p\n", __func__, ctx);
	return rc;
}

int mipstee_cancel_req(struct tee_context *ctx, u32 cancel_id, u32 session)
{
	struct mipstee_context_data *ctxdata = ctx->data;
	struct tee_shm *shm;
	struct mipstee_msg_arg *msg_arg;
	struct mipstee_session *sess;
	int idr_id;

	pr_devel("%s ctx %p sess %u cancel_id %x\n", __func__, ctx, session,
			cancel_id);

	/*
	 * For open session a session does not yet exist; Check that the
	 * session is valid if it's provided.
	 */
	if (session) {
		mutex_lock(&ctxdata->mutex);
		sess = find_session(ctxdata, session);
		mutex_unlock(&ctxdata->mutex);
		if (!sess)
			return -EINVAL;
	}

	idr_id = mipstee_find_cancel_idr(ctxdata, cancel_id);
	if (!idr_id)
		return -EINVAL;

	shm = get_msg_arg(ctx, 0, &msg_arg, NULL);
	if (IS_ERR(shm))
		return PTR_ERR(shm);

	msg_arg->cmd = MIPSTEE_MSG_CMD_CANCEL;
	msg_arg->session = session;
	msg_arg->cancel_id = idr_id;
	mipstee_do_call_with_arg(ctx, msg_arg);

	tee_shm_free(shm);
	return 0;
}
