// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *   Copyright (C) 2018 Namjae Jeon <namjae.jeon@protocolfreedom.org>
 */

#include "smb_common.h"
#include "server.h"
#include "misc.h"
/* @FIXME */
#include "transport_tcp.h"

/*for shortname implementation */
static const char basechars[43] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_-!@#$%";
#define MANGLE_BASE       (sizeof(basechars)/sizeof(char)-1)
#define MAGIC_CHAR '~'
#define PERIOD '.'
#define mangle(V) ((char)(basechars[(V) % MANGLE_BASE]))

#ifdef CONFIG_CIFS_INSECURE_SERVER
#define CIFSD_MIN_SUPPORTED_HEADER_SIZE	(sizeof(struct smb_hdr))
#else
#define CIFSD_MIN_SUPPORTED_HEADER_SIZE	(sizeof(struct smb2_hdr))
#endif

struct smb_protocol {
	int		index;
	char		*name;
	char		*prot;
	__u16		prot_id;
};

static struct smb_protocol smb_protos[] = {
#ifdef CONFIG_CIFS_INSECURE_SERVER
	{
		SMB1_PROT,
		"\2NT LM 0.12",
		"NT1",
		SMB10_PROT_ID
	},
#endif
	{
		SMB311_PROT,
		"\2SMB 3.1.1",
		"SMB3_11",
		SMB311_PROT_ID
	},
	{
		SMB2_PROT,
		"\2SMB 2.002",
		"SMB2_02",
		SMB20_PROT_ID
	},
	{
		SMB21_PROT,
		"\2SMB 2.1",
		"SMB2_10",
		SMB21_PROT_ID
	},
	{
		SMB30_PROT,
		"\2SMB 3.0",
		"SMB3_00",
		SMB30_PROT_ID
	},
	{
		SMB302_PROT,
		"\2SMB 3.02",
		"SMB3_02",
		SMB302_PROT_ID
	},
};

unsigned int cifsd_max_msg_size(void)
{
	return 65536;
}

unsigned int cifsd_default_io_size(void)
{
	return (1024 * 1024);
}

unsigned int cifsd_small_buffer_size(void)
{
	return 448;
}

inline int cifsd_min_protocol(void)
{
#ifdef CONFIG_CIFS_INSECURE_SERVER
	return SMB1_PROT;
#else
	return SMB2_PROT;
#endif
}

inline int cifsd_max_protocol(void)
{
	return SMB311_PROT;
}

int cifsd_lookup_protocol_idx(char *str)
{
	int offt = ARRAY_SIZE(smb_protos) - 1;
	int len = strlen(str);

	while (offt >= 0) {
		if (!strncmp(str, smb_protos[offt].prot, len)) {
			cifsd_debug("selected %s dialect idx = %d\n",
					smb_protos[offt].prot, offt);
			return smb_protos[offt].index;
		}
		offt--;
	}
	return -1;
}

/**
 * check_message() - check for valid smb2 request header
 * @buf:       smb2 header to be checked
 *
 * check for valid smb signature and packet direction(request/response)
 *
 * Return:      0 on success, otherwise 1
 */
int cifsd_verify_smb_message(struct cifsd_work *work)
{
	struct smb2_hdr *smb2_hdr = REQUEST_BUF(work);

	if (smb2_hdr->ProtocolId == SMB2_PROTO_NUMBER) {
		cifsd_debug("got SMB2 command\n");
		return smb2_check_message(work);
	}

	return smb1_check_message(work);
}

/**
 * is_smb_request() - check for valid smb request type
 * @conn:     TCP server instance of connection
 * @type:	smb request type
 *
 * Return:      true on success, otherwise false
 */
bool cifsd_smb_request(struct cifsd_tcp_conn *conn)
{
	int type = *(char *)conn->request_buf;

	switch (type) {
	case RFC1002_SESSION_MESSAGE:
		/* Regular SMB request */
		return true;
	case RFC1002_SESSION_KEEP_ALIVE:
		cifsd_debug("RFC 1002 session keep alive\n");
		break;
	default:
		cifsd_debug("RFC 1002 unknown request type 0x%x\n", type);
	}

	return false;
}

static bool supported_protocol(int idx)
{
	return (server_conf.min_protocol <= idx &&
			idx <= server_conf.max_protocol);
}

static char *lower_dialect(char *head, char *tail)
{
	if (tail == head)
		return NULL;

	tail--;
	while (tail > head) {
		tail--;
		if ((char)(*tail) == '\0')
			return tail + 1;
	}
	return head;
}

static int cifsd_lookup_dialect_by_name(char *cli_dialects, __le16 byte_count)
{
	int i, bcount = le16_to_cpu(byte_count);
	char *prot = NULL;

	for (i = ARRAY_SIZE(smb_protos) - 1; i >= 0; i--) {
		prot = lower_dialect(cli_dialects, cli_dialects + bcount);

		while (prot) {
			cifsd_debug("client requested dialect %s\n", prot);
			if (!strcmp(prot, smb_protos[i].name)) {
				if (supported_protocol(smb_protos[i].index)) {
					cifsd_debug("selected %s dialect\n",
							smb_protos[i].name);
					return smb_protos[i].prot_id;
				}
			}
			prot = lower_dialect(cli_dialects, prot);
		}
	}
	return BAD_PROT_ID;
}

int cifsd_lookup_dialect_by_id(__le16 *cli_dialects, __le16 dialects_count)
{
	int i;
	int count;

	for (i = ARRAY_SIZE(smb_protos) - 1; i >= 0; i--) {
		count = le16_to_cpu(dialects_count);
		while (--count >= 0) {
			cifsd_debug("client requested dialect 0x%x\n",
				le16_to_cpu(cli_dialects[count]));
			if (le16_to_cpu(cli_dialects[count]) !=
					smb_protos[i].prot_id)
				continue;

			if (supported_protocol(smb_protos[i].index)) {
				cifsd_debug("selected %s dialect\n",
					smb_protos[i].name);
				return smb_protos[i].prot_id;
			}
		}
	}

	return BAD_PROT_ID;
}

int cifsd_negotiate_smb_dialect(void *buf)
{
	__le32 proto;

	proto = ((struct smb2_hdr *)buf)->ProtocolId;
	if (proto == SMB2_PROTO_NUMBER) {
		struct smb2_negotiate_req *req;

		req = (struct smb2_negotiate_req *)buf;
		return cifsd_lookup_dialect_by_id(req->Dialects,
					le16_to_cpu(req->DialectCount));
	}

	proto = *(__le32 *)((struct smb_hdr *)buf)->Protocol;
	if (proto == SMB1_PROTO_NUMBER) {
		NEGOTIATE_REQ *req;

		req = (NEGOTIATE_REQ *)buf;
		return cifsd_lookup_dialect_by_name(req->DialectsArray,
					le16_to_cpu(req->ByteCount));
	}

	return BAD_PROT_ID;
}

void cifsd_init_smb2_server_common(struct cifsd_tcp_conn *conn)
{
	if (init_smb2_0_server(conn) == -ENOTSUPP)
		init_smb2_1_server(conn);
}

int cifsd_init_smb_server(struct cifsd_work *work)
{
	struct cifsd_tcp_conn *conn = work->conn;
	void *buf = REQUEST_BUF(work);
	int proto;

	if (!conn->need_neg)
		return 0;

	proto = *(__le32 *)((struct smb_hdr *)buf)->Protocol;
	if (proto == SMB1_PROTO_NUMBER) {
		if (init_smb1_server(conn) == -ENOTSUPP)
			cifsd_init_smb2_server_common(conn);
	} else {
		cifsd_init_smb2_server_common(conn);
	}

	if (conn->ops->get_cmd_val(work) != SMB_COM_NEGOTIATE)
		conn->need_neg = false;
	return 0;
}

bool cifsd_pdu_size_has_room(unsigned int pdu)
{
	return (pdu >= CIFSD_MIN_SUPPORTED_HEADER_SIZE - 4);
}

int cifsd_populate_dot_dotdot_entries(struct cifsd_tcp_conn *conn,
				      int info_level,
				      struct cifsd_file *dir,
				      struct cifsd_dir_info *d_info,
				      char *search_pattern,
				      int (*fn)(struct cifsd_tcp_conn *,
						int,
						struct cifsd_dir_info *,
						struct cifsd_kstat *))
{
	int i, rc = 0;

	for (i = 0; i < 2; i++) {
		struct kstat kstat;
		struct cifsd_kstat cifsd_kstat;

		if (!dir->dot_dotdot[i]) { /* fill dot entry info */
			if (i == 0)
				d_info->name = ".";
			else
				d_info->name = "..";

			if (!is_matched(d_info->name, search_pattern)) {
				dir->dot_dotdot[i] = 1;
				continue;
			}

			generic_fillattr(PARENT_INODE(dir), &kstat);
			cifsd_kstat.file_attributes = ATTR_DIRECTORY;
			cifsd_kstat.kstat = &kstat;
			rc = fn(conn, info_level, d_info, &cifsd_kstat);
			if (rc)
				break;
			if (d_info->out_buf_len <= 0)
				break;

			dir->dot_dotdot[i] = 1;
		}
	}

	return rc;
}

/**
 * cifsd_extract_shortname() - get shortname from long filename
 * @conn:	TCP server instance of connection
 * @longname:	source long filename
 * @shortname:	destination short filename
 *
 * Return:	shortname length or 0 when source long name is '.' or '..'
 * TODO: Though this function comforms the restriction of 8.3 Filename spec,
 * but the result is different with Windows 7's one. need to check.
 */
int cifsd_extract_shortname(struct cifsd_tcp_conn *conn,
			    char *longname,
			    char *shortname)
{
	char *p, *sp;
	char base[9], extension[4];
	char out[13] = {0};
	int baselen = 0;
	int extlen = 0, len = 0;
	unsigned int csum = 0;
	unsigned char *ptr;
	bool dot_present = true;

	p = longname;
	if ((*p == '.') || (!(strcmp(p, "..")))) {
		/*no mangling required */
		shortname = NULL;
		return 0;
	}
	p = strrchr(longname, '.');
	if (p == longname) { /*name starts with a dot*/
		sp = "___";
		memcpy(extension, sp, 3);
		extension[3] = '\0';
	} else {
		if (p != NULL) {
			p++;
			while (*p && extlen < 3) {
				if (*p != '.')
					extension[extlen++] = toupper(*p);
				p++;
			}
			extension[extlen] = '\0';
		} else
			dot_present = false;
	}

	p = longname;
	if (*p == '.')
		*p++ = 0;
	while (*p && (baselen < 5)) {
		if (*p != '.')
			base[baselen++] = toupper(*p);
		p++;
	}

	base[baselen] = MAGIC_CHAR;
	memcpy(out, base, baselen+1);

	ptr = longname;
	len = strlen(longname);
	for (; len > 0; len--, ptr++)
		csum += *ptr;

	csum = csum % (MANGLE_BASE * MANGLE_BASE);
	out[baselen+1] = mangle(csum/MANGLE_BASE);
	out[baselen+2] = mangle(csum);
	out[baselen+3] = PERIOD;

	if (dot_present)
		memcpy(&out[baselen+4], extension, 4);
	else
		out[baselen+4] = '\0';
	smbConvertToUTF16((__le16 *)shortname, out, PATH_MAX,
			conn->local_nls, 0);
	len = strlen(out) * 2;
	return len;
}

/**
 * cifsd_fill_dirent() - populates a dirent details in readdir
 * @ctx:	dir_context information
 * @name:	dirent name
 * @namelen:	dirent name length
 * @offset:	dirent offset in directory
 * @ino:	dirent inode number
 * @d_type:	dirent type
 *
 * Return:	0 on success, otherwise -EINVAL
 */
int cifsd_fill_dirent(struct dir_context *ctx,
		      const char *name,
		      int namlen,
		      loff_t offset,
		      u64 ino,
		      unsigned int d_type)
{
	struct cifsd_readdir_data *buf =
		container_of(ctx, struct cifsd_readdir_data, ctx);
	struct cifsd_dirent *de = (void *)(buf->dirent + buf->used);
	unsigned int reclen;

	reclen = ALIGN(sizeof(struct cifsd_dirent) + namlen, sizeof(u64));
	if (buf->used + reclen > PAGE_SIZE) {
		buf->full = 1;
		return -EINVAL;
	}

	de->namelen = namlen;
	de->offset = offset;
	de->ino = ino;
	de->d_type = d_type;
	memcpy(de->name, name, namlen);
	buf->used += reclen;
	buf->dirent_count++;

	return 0;
}

static int __smb2_negotiate(struct cifsd_tcp_conn *conn)
{
	return (conn->dialect >= SMB20_PROT_ID &&
			conn->dialect <= SMB311_PROT_ID);
}

#ifndef CONFIG_CIFS_INSECURE_SERVER
int smb_handle_negotiate(struct cifsd_work *work)
{
	NEGOTIATE_RSP *neg_rsp = (NEGOTIATE_RSP *)RESPONSE_BUF(work);

	cifsd_err("Unsupported SMB protocol\n");
	neg_rsp->hdr.Status.CifsError = NT_STATUS_INVALID_LOGON_TYPE;
	return -EINVAL;
}
#endif

int cifsd_smb_negotiate_common(struct cifsd_work *work, unsigned int command)
{
	struct cifsd_tcp_conn *conn = work->conn;
	int ret;

	conn->dialect = cifsd_negotiate_smb_dialect(REQUEST_BUF(work));
	cifsd_debug("conn->dialect 0x%x\n", conn->dialect);

	if (command == SMB2_NEGOTIATE_HE) {
		struct smb2_hdr *smb2_hdr = REQUEST_BUF(work);

		if (smb2_hdr->ProtocolId != SMB2_PROTO_NUMBER) {
			cifsd_debug("Downgrade to SMB1 negotiation\n");
			command = SMB_COM_NEGOTIATE;
		}
	}

	if (command == SMB2_NEGOTIATE_HE) {
		ret = smb2_handle_negotiate(work);
		init_smb2_neg_rsp(work);
		return ret;
	}

	if (command == SMB_COM_NEGOTIATE) {
		if (__smb2_negotiate(conn)) {
			conn->need_neg = true;
			cifsd_init_smb2_server_common(conn);
			init_smb2_neg_rsp(work);
			cifsd_debug("Upgrade to SMB2 negotiation\n");
			return 0;
		}
		return smb_handle_negotiate(work);
	}

	cifsd_err("Unknown SMB negotiation command: %u\n", command);
	return -EINVAL;
}