/*
 * pkcs15-crypt.c: Tool for cryptography operations with smart cards
 *
 * Copyright (C) 2001  Juha Yrjölä <juha.yrjola@iki.fi>
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

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>
#ifdef ENABLE_OPENSSL
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/dsa.h>
#endif

#include "common/compat_getpass.h"
#include "libopensc/opensc.h"
#include "libopensc/pkcs15.h"
#include "util.h"

static const char *app_name = "pkcs15-crypt";

static int verbose = 0, opt_wait = 0, opt_raw = 0;
static char * opt_reader;
static char * opt_pincode = NULL, * opt_key_id = NULL;
static char * opt_input = NULL, * opt_output = NULL;
static char * opt_bind_to_aid = NULL;
static int opt_crypt_flags = 0;

enum {
	OPT_SHA1 = 	0x100,
	OPT_SHA256,
	OPT_SHA384,
	OPT_SHA512,
	OPT_SHA224,
	OPT_MD5,
	OPT_PKCS1,
	OPT_BIND_TO_AID,
	OPT_HASH_NONE,
};

static const struct option options[] = {
	{ "sign",		0, NULL,		's' },
	{ "decipher",		0, NULL,		'c' },
	{ "key",		1, NULL,		'k' },
	{ "reader",		1, NULL,		'r' },
	{ "input",		1, NULL,		'i' },
	{ "output",		1, NULL,		'o' },
	{ "raw",		0, NULL,		'R' },
	{ "sha-1",		0, NULL,		OPT_SHA1 },
	{ "sha-256",		0, NULL,		OPT_SHA256 },
	{ "sha-384",		0, NULL,		OPT_SHA384 },
	{ "sha-512",		0, NULL,		OPT_SHA512 },
	{ "sha-224",		0, NULL,		OPT_SHA224 },
	{ "md5",		0, NULL,		OPT_MD5 },
	{ "hash-none",		0, NULL,		OPT_HASH_NONE },
	{ "pkcs1",		0, NULL,		OPT_PKCS1 },
	{ "pin",		1, NULL,		'p' },
	{ "aid",		1, NULL,		OPT_BIND_TO_AID },
	{ "wait",		0, NULL,		'w' },
	{ "verbose",		0, NULL,		'v' },
	{ NULL, 0, NULL, 0 }
};

static const char *option_help[] = {
	"Performs digital signature operation",
	"Decipher operation",
	"Selects the private key ID to use",
	"Uses reader number <arg>",
	"Selects the input file to use",
	"Outputs to file <arg>",
	"Outputs raw 8 bit data",
	"Input file is a SHA-1 hash",
	"Input file is a SHA-256 hash",
	"Input file is a SHA-384 hash",
	"Input file is a SHA-512 hash",
	"Input file is a SHA-224 hash",
	"Input file is a MD5 hash",
	"Use PKCS #1 v1.5 padding",
	"Uses password (PIN) <arg> (use - for reading PIN from STDIN)",
	"Specify AID of the on-card PKCS#15 application to be binded to (in hexadecimal form)",
	"Wait for card insertion",
	"Verbose operation. Use several times to enable debug output.",
};

static sc_context_t *ctx = NULL;
static sc_card_t *card = NULL;
static struct sc_pkcs15_card *p15card = NULL;

static char *readpin_stdin(void)
{
	char buf[128];
	char *p;

	p = fgets(buf, sizeof(buf), stdin);
	if (p != NULL) {
		p = strchr(buf, '\n');
		if (p != NULL)
			*p = '\0';
		return strdup(buf);
	}
	return NULL;
}

static char * get_pin(struct sc_pkcs15_object *obj)
{
	char buf[80];
	char *pincode;
	struct sc_pkcs15_auth_info *pinfo = (struct sc_pkcs15_auth_info *) obj->data;

	if (pinfo->auth_type != SC_PKCS15_PIN_AUTH_TYPE_PIN)
		return NULL;

	if (opt_pincode != NULL) {
		if (strcmp(opt_pincode, "-") == 0)
			return readpin_stdin();
		else
			return strdup(opt_pincode);
	}
	
	sprintf(buf, "Enter PIN [%s]: ", obj->label);
	while (1) {
		pincode = getpass(buf);
		if (strlen(pincode) == 0)
			return NULL;
		if (strlen(pincode) < pinfo->attrs.pin.min_length ||
		    strlen(pincode) > pinfo->attrs.pin.max_length)
		    	continue;
		return strdup(pincode);
	}
}

static int read_input(u8 *buf, int buflen)
{
	FILE *inf;
	int c;
	
	inf = fopen(opt_input, "rb");
	if (inf == NULL) {
		fprintf(stderr, "Unable to open '%s' for reading.\n", opt_input);
		return -1;
	}
	c = fread(buf, 1, buflen, inf);
	fclose(inf);
	if (c < 0) {
		perror("read");
		return -1;
	}
	return c;
}

static int write_output(const u8 *buf, int len)
{
	FILE *outf;
	int output_binary = (opt_output == NULL && opt_raw == 0 ? 0 : 1);
	
	if (opt_output != NULL) {
		outf = fopen(opt_output, "wb");
		if (outf == NULL) {
			fprintf(stderr, "Unable to open '%s' for writing.\n", opt_output);
			return -1;
		}
	} else {
		outf = stdout;
	}
	if (output_binary == 0)
		util_print_binary(outf, buf, len);
	else
		fwrite(buf, len, 1, outf);
	if (outf != stdout)
		fclose(outf);
	return 0;
}

static int sign(struct sc_pkcs15_object *obj)
{
	u8 buf[1024], out[1024];
	struct sc_pkcs15_prkey_info *key = (struct sc_pkcs15_prkey_info *) obj->data;
	int r, c, len;
	
	if (opt_input == NULL) {
		fprintf(stderr, "No input file specified.\n");
		return 2;
	}

	c = read_input(buf, sizeof(buf));
	if (c < 0)
		return 2;
	len = sizeof(out);
	if (obj->type == SC_PKCS15_TYPE_PRKEY_RSA 
			&& !(opt_crypt_flags & SC_ALGORITHM_RSA_PAD_PKCS1)
			&& (size_t)c != key->modulus_length/8) {
		fprintf(stderr, "Input has to be exactly %lu bytes, when using no padding.\n",
			(unsigned long) key->modulus_length/8);
		return 2;
	}
	if (!key->native) {
		fprintf(stderr, "Deprecated non-native key detected! Upgrade your smart cards.\n");
		return SC_ERROR_NOT_SUPPORTED;
	}

	r = sc_pkcs15_compute_signature(p15card, obj, opt_crypt_flags, buf, c, out, len);
	if (r < 0) {
		fprintf(stderr, "Compute signature failed: %s\n", sc_strerror(r));
		return 1;
	}

	r = write_output(out, r);
	
	return 0;
}

static int decipher(struct sc_pkcs15_object *obj)
{
	u8 buf[1024], out[1024];
	int r, c, len;
	
	if (opt_input == NULL) {
		fprintf(stderr, "No input file specified.\n");
		return 2;
	}
	c = read_input(buf, sizeof(buf));
	if (c < 0)
		return 2;

	len = sizeof(out);
	if (!((struct sc_pkcs15_prkey_info *) obj->data)->native) {
                fprintf(stderr, "Deprecated non-native key detected! Upgrade your smart cards.\n");
		return SC_ERROR_NOT_SUPPORTED;
	}

	r = sc_pkcs15_decipher(p15card, obj, opt_crypt_flags & SC_ALGORITHM_RSA_PAD_PKCS1, buf, c, out, len);
	if (r < 0) {
		fprintf(stderr, "Decrypt failed: %s\n", sc_strerror(r));
		return 1;
	}
	r = write_output(out, r);
	
	return 0;
}

static int get_key(unsigned int usage, sc_pkcs15_object_t **result)
{
	sc_pkcs15_object_t *key, *pin;
	const char	*usage_name;
	sc_pkcs15_id_t	id;
	int		r;

	usage_name = (usage & SC_PKCS15_PRKEY_USAGE_SIGN)? "signature" : "decryption";

	if (opt_key_id != NULL) {
		sc_pkcs15_hex_string_to_id(opt_key_id, &id);
		r = sc_pkcs15_find_prkey_by_id_usage(p15card, &id, usage, &key);
		if (r < 0) {
			fprintf(stderr, "Unable to find private %s key '%s': %s\n",
				usage_name, opt_key_id, sc_strerror(r));
			return 2;
		}
	} else {
		r = sc_pkcs15_find_prkey_by_id_usage(p15card, NULL, usage, &key);
		if (r < 0) {
			fprintf(stderr, "Unable to find any private %s key: %s\n",
				usage_name, sc_strerror(r));
			return 2;
		}
	}

	*result = key;

	if (key->auth_id.len) {
		static sc_pkcs15_object_t *prev_pin = NULL;
		char	*pincode;

		r = sc_pkcs15_find_pin_by_auth_id(p15card, &key->auth_id, &pin);
		if (r) {
			fprintf(stderr, "Unable to find PIN code for private key: %s\n",
				sc_strerror(r));
			return 1;
		}

		/* Pin already verified previously */
		if (pin == prev_pin)
			return 0;

		pincode = get_pin(pin);
		if (((pincode == NULL || *pincode == '\0')) &&
		    !(p15card->card->reader->capabilities & SC_READER_CAP_PIN_PAD))
				return 5;

		r = sc_pkcs15_verify_pin(p15card, pin, (const u8 *)pincode, pincode ? strlen(pincode) : 0);
		if (r) {
			fprintf(stderr, "PIN code verification failed: %s\n", sc_strerror(r));
			return 5;
		}
		free(pincode);
		if (verbose)
			fprintf(stderr, "PIN code correct.\n");
		prev_pin = pin;
	}

	return 0;
}

int main(int argc, char * const argv[])
{
	int err = 0, r, c, long_optind = 0;
	int do_decipher = 0;
	int do_sign = 0;
	int action_count = 0;
        struct sc_pkcs15_object *key;
	sc_context_param_t ctx_param;
		
	while (1) {
		c = getopt_long(argc, argv, "sck:r:i:o:Rp:vw", options, &long_optind);
		if (c == -1)
			break;
		if (c == '?')
			util_print_usage_and_die(app_name, options, option_help);
		switch (c) {
		case 's':
			do_sign++;
			action_count++;
			break;
		case 'c':
			do_decipher++;
			action_count++;
			break;
		case 'k':
			opt_key_id = optarg;
			action_count++;
			break;
		case 'r':
			opt_reader = optarg;
			break;
		case 'i':
			opt_input = optarg;
			break;
		case 'o':
			opt_output = optarg;
			break;
		case 'R':
			opt_raw = 1;
			break;
		case OPT_SHA1:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_SHA1;
			break;
		case OPT_SHA256:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_SHA256;
			break;
		case OPT_SHA384:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_SHA384;
			break;
		case OPT_SHA512:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_SHA512;
			break;
		case OPT_SHA224:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_SHA224;
			break;
		case OPT_MD5:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_MD5;
			break;
		case OPT_HASH_NONE:
			opt_crypt_flags |= SC_ALGORITHM_RSA_HASH_NONE;
			break;
		case OPT_PKCS1:
			opt_crypt_flags |= SC_ALGORITHM_RSA_PAD_PKCS1;
			break;
		case 'v':
			verbose++;
			break;
		case 'p':
			opt_pincode = optarg;
			break;
		case OPT_BIND_TO_AID:
			opt_bind_to_aid = optarg;
			break;
		case 'w':
			opt_wait = 1;
			break;
		}
	}
	if (action_count == 0)
		util_print_usage_and_die(app_name, options, option_help);

	memset(&ctx_param, 0, sizeof(ctx_param));
	ctx_param.ver      = 0;
	ctx_param.app_name = app_name;

	r = sc_context_create(&ctx, &ctx_param);
	if (r) {
		fprintf(stderr, "Failed to establish context: %s\n", sc_strerror(r));
		return 1;
	}

	if (verbose > 1) {
		ctx->debug = verbose;
		sc_ctx_log_to_file(ctx, "stderr");
	}

	err = util_connect_card(ctx, &card, opt_reader, opt_wait, verbose);
	if (err)
		goto end;

	if (verbose)
		fprintf(stderr, "Trying to find a PKCS #15 compatible card...\n");
	if (opt_bind_to_aid)   {
		struct sc_aid aid;

		aid.len = sizeof(aid.value);
		if (sc_hex_to_bin(opt_bind_to_aid, aid.value, &aid.len))   {
			fprintf(stderr, "Invalid AID value: '%s'\n", opt_bind_to_aid);
			return 1;
		}

		r = sc_pkcs15_bind(card, &aid, &p15card);
	}
	else   {
		r = sc_pkcs15_bind(card, NULL, &p15card);
	}
	if (r) {
		fprintf(stderr, "PKCS #15 binding failed: %s\n", sc_strerror(r));
		err = 1;
		goto end;
	}
	if (verbose)
		fprintf(stderr, "Found %s!\n", p15card->tokeninfo->label);

	if (do_decipher) {
		if ((err = get_key(SC_PKCS15_PRKEY_USAGE_DECRYPT, &key))
		 || (err = decipher(key)))
			goto end;
		action_count--;
	}
	
	if (do_sign) {
		if ((err = get_key(SC_PKCS15_PRKEY_USAGE_SIGN|
				   SC_PKCS15_PRKEY_USAGE_SIGNRECOVER|
				   SC_PKCS15_PRKEY_USAGE_NONREPUDIATION, &key))
		 || (err = sign(key)))
			goto end;
		action_count--;
	}
end:
	if (p15card)
		sc_pkcs15_unbind(p15card);
	if (card) {
		sc_unlock(card);
		sc_disconnect_card(card);
	}
	if (ctx)
		sc_release_context(ctx);
	return err;
}
