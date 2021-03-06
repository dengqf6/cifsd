// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 */

#ifndef __USER_CONFIG_MANAGEMENT_H__
#define __USER_CONFIG_MANAGEMENT_H__

#include "../glob.h"  /* FIXME */
#include "../cifsd_server.h" /* FIXME */

struct cifsd_user {
	unsigned short		flags;

	unsigned int		uid;
	unsigned int		gid;

	char			*name;

	size_t			passkey_sz;
	char			*passkey;
};

static inline bool user_guest(struct cifsd_user *user)
{
	return user->flags & CIFSD_USER_FLAG_GUEST_ACCOUNT;
}

static inline void set_user_flag(struct cifsd_user *user, int flag)
{
	user->flags |= flag;
}

static inline int test_user_flag(struct cifsd_user *user, int flag)
{
	return user->flags & flag;
}

static inline void set_user_guest(struct cifsd_user *user)
{
}

static inline char *user_passkey(struct cifsd_user *user)
{
	return user->passkey;
}

static inline char *user_name(struct cifsd_user *user)
{
	return user->name;
}

static inline unsigned int user_uid(struct cifsd_user *user)
{
	return user->uid;
}

static inline unsigned int user_gid(struct cifsd_user *user)
{
	return user->gid;
}

struct cifsd_user *cifsd_alloc_user(const char *account);
void cifsd_free_user(struct cifsd_user *user);
#endif /* __USER_CONFIG_MANAGEMENT_H__ */
