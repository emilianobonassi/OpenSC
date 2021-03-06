/*
 * card-acos5.c: Support for ACS ACOS5 cards.
 *
 * Copyright (C) 2007  Ian A. Young <ian@iay.org.uk>
 * Copyright (c) 2011  Pace Willisson <pace@alum.mit.edu>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>

#include "internal.h"
#include "cardctl.h"
#include "asn1.h"

static struct sc_atr_table acos5_atrs[] = {
	{"3b:be:18:00:00:41:05:10:00:00:00:00:00:00:00:00:00:90:00", NULL, NULL,
	 SC_CARD_TYPE_ACOS5_GENERIC, 0, NULL},
	{NULL, NULL, NULL, 0, 0, NULL}
};

static struct sc_card_operations *iso_ops;
static struct sc_card_operations acos5_ops;
static struct sc_card_driver acos5_drv = {
	"ACS ACOS5 card",
	"acos5",
	&acos5_ops,
	NULL, 0, NULL
};

static int acos5_match_card(sc_card_t * card)
{
	int i;

	i = _sc_match_atr(card, acos5_atrs, &card->type);
	if (i < 0)
		return 0;
	return 1;
}

static int acos5_init(sc_card_t * card)
{
	unsigned long	flags;

	flags = 0;
	flags |= SC_ALGORITHM_RSA_RAW;
	flags |= SC_ALGORITHM_RSA_HASH_NONE;
	flags |= SC_ALGORITHM_ONBOARD_KEY_GEN;
	flags |= SC_ALGORITHM_NEED_USAGE;

	_sc_card_add_rsa_alg(card, 512, flags, 0);
	_sc_card_add_rsa_alg(card, 1024, flags, 0);
	/*
	 * card also supports 2048 bits, but we'll need driver updates
	 * in several places to add chaining for the bigger data
	 * blocks
	 */

	card->caps |= SC_CARD_CAP_USE_FCI_AC;
	card->max_recv_size = 255;
	card->max_send_size = 255;
	return SC_SUCCESS;
}

static int acos5_select_file_by_path(sc_card_t * card,
				     const sc_path_t * in_path,
				     sc_file_t ** file_out)
{
	struct sc_context *ctx = card->ctx;
	struct acos5_drv_data *drv_data = card->drv_data;
	sc_path_t path;
	int r;
	int offset;

	/*
	 * The select command can't swallow a path -
	 * we can only pass it one file id at a time.
	 * 
	 * One solution is to re-do the selects from the root
	 * directory each time, but, if we've established security
	 * parameters, we lose them as soon as we select a different
	 * DF. card-mcrd.c tries to manage the same problem by keeping
	 * track of the current directory, and avoiding unnecessary
	 * selects away from it.  But that's pretty complicated (after
	 * a select, you have to figure out whether the current file
	 * is a directory or not, among other things)
	 *
	 * Luckily, the acos5 card implements a search path in the
	 * select command, documented as follows:
	 *
	 * current DF
	 * current DF's children
	 * current DF's parent
	 * current DF's siblings
	 * MF
	 * MF's children
	 *
	 * Crazy for an ordinary operating system, but it actually
	 * covers all the cases we care about, since we do all our
	 * work in a single AppDF immediately under the root.
	 *
	 * So, we just dig the last item out of the path and select it.
	 */
	 
	SC_FUNC_CALLED(ctx, SC_LOG_DEBUG_VERBOSE);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL,
		 "select by path", sc_print_path(in_path));

	if (in_path->len == 0)
		return SC_SUCCESS;

	if (in_path->len % 2 != 0)
		return SC_ERROR_INVALID_ARGUMENTS;

	memset(&path, 0, sizeof path);
	path.len = 2;
	path.type = SC_PATH_TYPE_FILE_ID;
	memcpy (path.value, in_path->value + in_path->len - 2, 2);

	r = iso_ops->select_file(card, &path, file_out);
	if (r == 0)
		return (SC_SUCCESS);

	/*
	 * the card's built-in search path didn't work...
	 * try doing it file by file
	 */
	for (offset = 0; offset < in_path->len; offset += 2) {
		memcpy(path.value, in_path->value + offset, 2);
		r = iso_ops->select_file(card, &path, file_out);
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r,
			    "select_file_by_path failed");
	}
	SC_FUNC_RETURN(ctx, SC_LOG_DEBUG_NORMAL, SC_SUCCESS);
}

static int acos5_select_file(sc_card_t * card,
			     const sc_path_t * in_path, sc_file_t ** file_out)
{
	switch (in_path->type) {

	case SC_PATH_TYPE_PATH:
		return acos5_select_file_by_path(card, in_path, file_out);

	default:
		return iso_ops->select_file(card, in_path, file_out);
	}
}

static int acos5_set_security_env(sc_card_t *card,
				  const sc_security_env_t *env,
				  int se_num)
{
	sc_apdu_t apdu;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 *p;
	int r, locked = 0;

	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	assert(card != NULL && env != NULL);

	/* manual section 4.2.5 */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x01, 0xb8);
	p = sbuf;
	*p++ = 0x95;
	*p++ = 0x01;
	*p++ = 0xff;
	
	*p++ = 0x80;
	*p++ = 0x01;
	*p++ = 0x12;
		
	if (env->flags & SC_SEC_ENV_FILE_REF_PRESENT) {
		*p++ = 0x81;
		*p++ = env->file_ref.len;
		assert(env->file_ref.len == 2);
		assert(sizeof(sbuf) - (p - sbuf) >= env->file_ref.len);
		memcpy(p, env->file_ref.value, env->file_ref.len);
		p += env->file_ref.len;
	}

	r = p - sbuf;
	apdu.lc = r;
	apdu.datalen = r;
	apdu.data = sbuf;
	if (se_num > 0) {
		r = sc_lock(card);
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r,
			    "sc_lock() failed");
		locked = 1;
	}

	r = sc_transmit_apdu(card, &apdu);
	if (r) {
		sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
			 "%s: APDU transmit failed", sc_strerror(r));
		goto err;
	}

	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r) {
		sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
			 "%s: Card returned error", sc_strerror(r));
		goto err;
	}

	if (se_num <= 0)
		return 0;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0xF2, se_num);
	r = sc_transmit_apdu(card, &apdu);
	sc_unlock(card);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
err:
	if (locked)
		sc_unlock(card);

	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_NORMAL, r);
}

static int acos5_decipher(sc_card_t *card,
			  const u8 * crgram, size_t crgram_len,
			  u8 * out, size_t outlen)
{
	int       r;
	sc_apdu_t apdu;
	u8        *sbuf = NULL;

	assert(card != NULL && crgram != NULL && out != NULL);
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_NORMAL);

	sbuf = malloc(crgram_len);
	if (sbuf == NULL)
		return SC_ERROR_OUT_OF_MEMORY;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4, 0x2A, 0x80, 0x84);
	apdu.resp    = out;
	apdu.resplen = outlen;
	/* if less than 256 bytes are expected than set Le to 0x00
	 * to tell the card the we want everything available (note: we
	 * always have Le <= crgram_len) */
	apdu.le      = (outlen >= 256 && crgram_len < 256) ? 256 : outlen;
	/*
	 * Use APDU chaining with 2048bit RSA keys if the card does
	 * not do extended APDU-s
	 */
	if ((crgram_len > 255) && !(card->caps & SC_CARD_CAP_APDU_EXT))
		apdu.flags |= SC_APDU_FLAGS_CHAINING;
	
	memcpy(sbuf, crgram, crgram_len);
	sc_mem_reverse(sbuf, crgram_len);
	apdu.data = sbuf;
	apdu.lc = crgram_len;
	apdu.datalen = crgram_len;
	r = sc_transmit_apdu(card, &apdu);
	sc_mem_clear(sbuf, crgram_len);
	free(sbuf);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	sc_mem_reverse(out, crgram_len);
	if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, apdu.resplen);
	else
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE,
			       sc_check_sw(card, apdu.sw1, apdu.sw2));
}

static int acos5_compute_signature(sc_card_t *card,
				   const u8 * data, size_t datalen,
				   u8 * out, size_t outlen)
{
	int r;
	sc_apdu_t apdu;
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE];

	assert(card != NULL && data != NULL && out != NULL);
	if (datalen > 255)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE,
			       SC_ERROR_INVALID_ARGUMENTS);

	if (outlen < datalen)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE,
			       SC_ERROR_INVALID_ARGUMENTS);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x2A, 0x80, 0x84);
	apdu.resp = rbuf;
	apdu.resplen = datalen;
	apdu.le = datalen;

	memcpy(sbuf, data, datalen);
	sc_mem_reverse(sbuf, datalen);
	apdu.data = sbuf;
	apdu.lc = datalen;
	apdu.datalen = datalen;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		size_t len = apdu.resplen > outlen ? outlen : apdu.resplen;

		memcpy(out, apdu.resp, len);
		sc_mem_reverse(out, len);
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, len);
	}
	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE,
		       sc_check_sw(card, apdu.sw1, apdu.sw2));
}

static int acos5_delete_file(sc_card_t *card, const sc_path_t *path)
{
	int r;
	sc_apdu_t apdu;
	sc_file_t	*file;

	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);
	if (path->type != SC_PATH_TYPE_FILE_ID
	    || (path->len != 0 && path->len != 2)) {
		sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
			 "File type has to be SC_PATH_TYPE_FILE_ID");
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_NORMAL,
			       SC_ERROR_INVALID_ARGUMENTS);
	}

	r = sc_select_file(card, path, &file);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r,
		    "can't select file to delete");
	sc_file_free(file);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_1, 0xE4, 0x00, 0x00);
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}

static int acos5_get_serialnr(sc_card_t * card, sc_serial_number_t * serial)
{
	int r;
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	sc_apdu_t apdu;

	/*
	 * Check arguments.
	 */
	if (!serial)
		return SC_ERROR_INVALID_ARGUMENTS;

	/*
	 * Return a cached serial number, if we have one.
	 */
	if (card->serialnr.len) {
		memcpy(serial, &card->serialnr, sizeof(*serial));
		return SC_SUCCESS;
	}

	/*
	 * Fetch serial number using GET CARD INFO.
	 */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0x14, 0, 0);
	apdu.cla |= 0x80;
	apdu.resp = rbuf;
	apdu.resplen = sizeof(rbuf);
	apdu.le = 6;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
		return SC_ERROR_INTERNAL;

	/*
	 * Cache serial number.
	 */
	memcpy(card->serialnr.value, apdu.resp,
	       MIN(apdu.resplen, SC_MAX_SERIALNR));
	card->serialnr.len = MIN(apdu.resplen, SC_MAX_SERIALNR);

	/*
	 * Copy and return serial number.
	 */
	memcpy(serial, &card->serialnr, sizeof(*serial));
	return SC_SUCCESS;
}

static int acos5_card_ctl(sc_card_t * card, unsigned long cmd, void *ptr)
{
	switch (cmd) {

	case SC_CARDCTL_GET_SERIALNR:
		return acos5_get_serialnr(card, (sc_serial_number_t *) ptr);

	case SC_CARDCTL_LIFECYCLE_SET:
		return 0;

	default:
		return SC_ERROR_NOT_SUPPORTED;
	}
}

static int acos5_list_files(sc_card_t * card, u8 * buf, size_t buflen)
{
	sc_apdu_t apdu;
	int r;
	size_t count;
	u8 *bufp = buf;		/* pointer into buf */
	int fno = 0;		/* current file index */

	/*
	 * Check parameters.
	 */
	if (!buf || (buflen & 1))
		return SC_ERROR_INVALID_ARGUMENTS;

	/*
	 * Use CARD GET INFO to fetch the number of files under the
	 * curently selected DF.
	 */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_1, 0x14, 0x01, 0x00);
	apdu.cla |= 0x80;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	if (apdu.sw1 != 0x90)
		return SC_ERROR_INTERNAL;
	count = apdu.sw2;

	while (count--) {
		u8 info[8];

		/*
		 * Truncate the scan if no more room left in output buffer.
		 */
		if (buflen == 0)
			break;

		sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0x14, 0x02,
			       fno++);
		apdu.cla |= 0x80;
		apdu.resp = info;
		apdu.resplen = sizeof(info);
		apdu.le = sizeof(info);
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r,
			    "APDU transmit failed");
		if (apdu.sw1 != 0x90 || apdu.sw2 != 0x00)
			return SC_ERROR_INTERNAL;

		*bufp++ = info[2];
		*bufp++ = info[3];
		buflen -= 2;
	}

	return bufp - buf;
}

struct acos5_sac_buf {
	u8 flags;
	u8 const *data;
	int datalen;
	int offset;
	int mask;
};

static int
acos5_sac_buf_get (struct acos5_sac_buf *sac)
{
	int expect_byte;

	expect_byte = (sac->mask & sac->flags);

	sac->mask >>= 1;

	if (expect_byte && sac->offset < sac->datalen)
		return (sac->data[sac->offset++]);

	return (0);
}

static void
acos5_add_acl (sc_file_t *file, int op, int rawval)
{
	unsigned int keyref, method;
	
	keyref = SC_AC_KEY_REF_NONE;

	if (rawval == 0) {
		method = SC_AC_NONE;
	} else if (rawval == 0xff) {
		method = SC_AC_NEVER;
	} else {
		method = SC_AC_CHV;
		keyref = rawval & 0x0f;
	}
	sc_file_add_acl_entry (file, op, method, keyref);
}

static int (*process_fci_orig)(sc_card_t *card, sc_file_t *file,
			       const u8 *buf, size_t buflen);

static int acos5_process_fci(sc_card_t *card, sc_file_t *file,
			     const u8 *buf, size_t buflen)
{
	sc_context_t *ctx = card->ctx;
	size_t taglen, len = buflen;
	const u8 *tag = NULL, *p = buf;
	int r;
	struct acos5_sac_buf sac;

	r = (*process_fci_orig)(card, file, buf, buflen);
	SC_TEST_RET(ctx, SC_LOG_DEBUG_NORMAL, r, "error parsing fci");

	tag = sc_asn1_find_tag(ctx, p, len, 0x8c, &taglen);
	if (tag && taglen >= 1) {
		sac.flags = tag[0];
		sac.data = tag + 1;
		sac.datalen = taglen - 1;
		sac.offset = 0;
		sac.mask = 0x80;

		acos5_sac_buf_get (&sac); /* bit 7 not used */
		acos5_add_acl (file, SC_AC_OP_DELETE,
			       acos5_sac_buf_get (&sac));
		acos5_sac_buf_get (&sac); /* bit 5 (terminate) not in opensc */
		acos5_add_acl (file, SC_AC_OP_REHABILITATE,
			       acos5_sac_buf_get (&sac));
		acos5_add_acl (file, SC_AC_OP_INVALIDATE,
			       acos5_sac_buf_get (&sac));
		acos5_add_acl (file, SC_AC_OP_CRYPTO,
			       acos5_sac_buf_get (&sac));
		acos5_add_acl (file, SC_AC_OP_UPDATE,
			       acos5_sac_buf_get (&sac));
		acos5_add_acl (file, SC_AC_OP_READ,
			       acos5_sac_buf_get (&sac));
	}
		
	return (SC_SUCCESS);
}

static int acos5_construct_fci(sc_card_t *card, const sc_file_t *file,
	u8 *out, size_t *outlen)
{
	u8 *p = out;
	u8 buf[64];

	if (*outlen < 2)
		return SC_ERROR_BUFFER_TOO_SMALL;
	*p++ = 0x62;
	p++;
	
	buf[0] = (file->size >> 8) & 0xFF;
	buf[1] = file->size & 0xFF;
	sc_asn1_put_tag(0x80, buf, 2, p, *outlen - (p - out), &p);

	if (file->type_attr_len) {
		if ((p - out) + file->type_attr_len > *outlen)
			return SC_ERROR_BUFFER_TOO_SMALL;
		memcpy(p, file->type_attr, file->type_attr_len);
		p += file->type_attr_len;
	} else {
		/* file->shareable ? */
		buf[0] = 0;
		switch (file->type) {
		case SC_FILE_TYPE_INTERNAL_EF:
			buf[0] |= 0x08;
			/* fall in */
		case SC_FILE_TYPE_WORKING_EF:
			buf[0] |= file->ef_structure & 7;
			break;
		case SC_FILE_TYPE_DF:
			buf[0] |= 0x38;
			break;
		default:
			return SC_ERROR_NOT_SUPPORTED;
		}
		sc_asn1_put_tag(0x82, buf, 1, p, *outlen - (p - out), &p);
	}
	buf[0] = (file->id >> 8) & 0xFF;
	buf[1] = file->id & 0xFF;
	sc_asn1_put_tag(0x83, buf, 2, p, *outlen - (p - out), &p);

	if (file->prop_attr_len) {
		if ((p - out) + file->prop_attr_len > *outlen)
			return SC_ERROR_BUFFER_TOO_SMALL;
		memcpy(p, file->prop_attr, file->prop_attr_len);
		p += file->prop_attr_len;
	}
	if (file->sec_attr_len) {
		if ((p - out) + file->sec_attr_len > *outlen)
			return SC_ERROR_BUFFER_TOO_SMALL;
		memcpy(p, file->sec_attr, file->sec_attr_len);
		p += file->sec_attr_len;
	}

	out[1] = p - out - 2;

	*outlen = p - out;
	return 0;
}

static int acos5_unblock (sc_card_t *card, struct sc_pin_cmd_data *pindata,
			   int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	u8 data[100];
	int dlen;
	sc_apdu_t apdu;
	int r;
	sc_path_t path;
	int file_id;
	int tries;

	file_id = 0x6001;
	memset(&path, 0, sizeof path);
	path.len = 2;
	path.type = SC_PATH_TYPE_FILE_ID;
	path.value[0] = (file_id >> 8) & 0xff;
	path.value[1] = file_id & 0xff;

	r = iso_ops->select_file(card, &path, NULL);
	SC_TEST_RET(ctx, SC_LOG_DEBUG_NORMAL, r, "can't select pinfile");

	dlen = pindata->pin1.len;
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x20, 0, 0x82);
	apdu.lc = dlen;
	apdu.datalen = dlen;
	apdu.data = pindata->pin1.data;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");	
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(ctx, SC_LOG_DEBUG_NORMAL, r, "verify puk failed");
	

	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xb2, pindata->pin_reference, 4);
	apdu.resplen = 18;
	apdu.le = apdu.resplen;
	apdu.resp = data;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");	
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(ctx, SC_LOG_DEBUG_NORMAL, r, "read pin info failed");

	tries = data[1] & 0x0f;
	if (tries == 0)
		tries = 1;

	/* manual section 3.1.1 PIN Data Structure */
	dlen = 0;
	data[dlen++] = 0x80 | pindata->pin_reference;
	data[dlen++] = (tries << 4) | tries;
	memcpy (data + dlen, pindata->pin2.data, pindata->pin2.len);
	dlen += pindata->pin2.len;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xdc, pindata->pin_reference, 4);
	apdu.lc = dlen;
	apdu.datalen = dlen;
	apdu.data = data;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");	
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(ctx, SC_LOG_DEBUG_NORMAL, r, "write pinrec failed");

	return SC_SUCCESS;
}



static int acos5_pin_cmd(sc_card_t *card, struct sc_pin_cmd_data *data,
			 int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	sc_apdu_t apdu;
	struct sc_path path;
	struct sc_pin_cmd_pin *pin1, *pin2;
	int r;
	u8 xbuf[100];
	int xlen;
	int puk_reference;
	
	switch (data->cmd) {
	case SC_PIN_CMD_VERIFY:
		pin1 = &data->pin1;
		
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT,
			       0x20, 0x00, data->pin_reference | 0x80);
		apdu.lc = pin1->len;
		apdu.datalen = pin1->len;
		apdu.data = pin1->data;
		r = sc_transmit_apdu (card, &apdu);
		SC_TEST_RET(ctx, SC_LOG_DEBUG_NORMAL, r,
			    "APDU transmit failed");
		r = sc_check_sw(card, apdu.sw1, apdu.sw2);
		if (r == SC_ERROR_PIN_CODE_INCORRECT)
			*tries_left = apdu.sw2 & 0xf;
		SC_TEST_RET(ctx, SC_LOG_DEBUG_VERBOSE, r,
			    "pin verify failed");
		return SC_SUCCESS;
		
	case SC_PIN_CMD_CHANGE:
		pin1 = &data->pin1;
		pin2 = &data->pin2;

		xlen = pin1->len + pin2->len;
		if (xlen > sizeof xbuf)
			return SC_ERROR_INTERNAL;
		memcpy (xbuf, pin1->data, pin1->len);
		memcpy (xbuf + pin1->len, pin2->data, pin2->len);

		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT,
			       0x24, 0x00, data->pin_reference | 0x80);
		apdu.lc = xlen;
		apdu.datalen = xlen;
		apdu.data = xbuf;
		r = sc_transmit_apdu (card, &apdu);
		SC_TEST_RET(ctx, SC_LOG_DEBUG_NORMAL, r,
			    "APDU transmit failed");
		r = sc_check_sw(card, apdu.sw1, apdu.sw2);
		if (r == SC_ERROR_PIN_CODE_INCORRECT)
			*tries_left = apdu.sw2 & 0xf;
		SC_TEST_RET(ctx, SC_LOG_DEBUG_VERBOSE, r,
			    "couldn't store new pin");
		return SC_SUCCESS;

	case SC_PIN_CMD_UNBLOCK:
		return acos5_unblock (card, data, tries_left);
	}

	sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
		 "acos5_pin_cmd: can't handle cmd %d", data->cmd);
	return SC_ERROR_NOT_SUPPORTED;
}

static struct sc_card_driver *sc_get_driver(void)
{
	struct sc_card_driver *iso_drv = sc_get_iso7816_driver();

	/* these default values have names like iso7816_create_file */
	iso_ops = iso_drv->ops;
	acos5_ops = *iso_ops;

	acos5_ops.match_card = acos5_match_card;
	acos5_ops.init = acos5_init;
	// finish
	// read_binary
	// write_binary
	// update_binary
	// erase_binary
	// read_record
	// write_record
	// append_record
	// update_record
	acos5_ops.select_file = acos5_select_file;
	// get_response
	// get_challenge
	// verify
	// logout
	// restore_security_env
	acos5_ops.set_security_env = acos5_set_security_env;
	acos5_ops.decipher = acos5_decipher;
	acos5_ops.compute_signature = acos5_compute_signature;
	// change_reference_data
	// reset_retry_counter
	// create_file
	acos5_ops.delete_file = acos5_delete_file;
	acos5_ops.list_files = acos5_list_files;
	// check_sw
	acos5_ops.card_ctl = acos5_card_ctl;

	process_fci_orig = acos5_ops.process_fci;
	acos5_ops.process_fci = acos5_process_fci;

	acos5_ops.construct_fci = acos5_construct_fci;
	acos5_ops.pin_cmd = acos5_pin_cmd;
	// get_data
	// put_data
	// delete_record
	return &acos5_drv;
}

struct sc_card_driver *sc_get_acos5_driver(void)
{
	return sc_get_driver();
}
