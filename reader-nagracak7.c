#include "globals.h"
#ifdef READER_NAGRA_MERLIN
#include "math.h"
#include "cscrypt/bn.h"
#include "cscrypt/idea.h"
#include "csctapi/icc_async.h"
#include "ncam-time.h"
#include "reader-common.h"
#include "reader-nagra-common.h"
#include "ncam-work.h"
#include "cscrypt/des.h"
#include "cscrypt/mdc2.h"

static const uint8_t public_exponent[] = { 0x01, 0x00, 0x01 };
static const uint8_t d00ff[] = { 0x00, 0xFF, 0xFF, 0xFF };
static const uint8_t irdid[] = { 0x64, 0x65, 0x6D, 0x6F }; // fake -> ASCII HEX-bytes of "demo"
static const uint8_t data1[] = { 0x00, 0x00, 0x00, 0x01 };

// Datatypes
#define IRDINFO 0x03
#define TIERS   0x0C
#define SYSID   0x05

static time_t tier_date(uint64_t date, char *buf, int32_t l)
{
	time_t ut = +694224000L + (date >> 1);
	if(buf)
	{
		struct tm t;
		cs_gmtime_r(&ut, &t);
		l = 27;
		snprintf(buf, l, "%04d/%02d/%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
	}
	return ut;
}

void rsa_decrypt(uint8_t *edata50, int len, uint8_t *out, uint8_t *key, int keylen)
{
	BN_CTX *ctx0 = BN_CTX_new();
#ifdef WITH_LIBCRYPTO
	BN_CTX_start(ctx0);
#endif
	BIGNUM *bnN0 = BN_CTX_get(ctx0);
	BIGNUM *bnE0 = BN_CTX_get(ctx0);
	BIGNUM *bnCT0 = BN_CTX_get(ctx0);
	BIGNUM *bnPT0 = BN_CTX_get(ctx0);
	BN_bin2bn(&key[0], keylen, bnN0);
	BN_bin2bn(public_exponent, 0x03, bnE0);
	BN_bin2bn(&edata50[0], len, bnCT0);
	BN_mod_exp(bnPT0, bnCT0, bnE0, bnN0, ctx0);
	memset(out,0x00,len);
	BN_bn2bin(bnPT0, out+ (len- BN_num_bytes(bnPT0)));
	BN_CTX_end(ctx0);
	BN_CTX_free(ctx0);
}

static void addProvider(struct s_reader *reader, uint8_t *cta_res)
{
	uint8_t i;
	bool toadd = true;

	for(i = 0; i < reader->nprov; i++)
	{
		if((cta_res[19] == reader->prid[i][2]) && (cta_res[20] == reader->prid[i][3]))
		{
			toadd = false;
		}
	}

	if(toadd)
	{
		reader->prid[reader->nprov][0] = 0;
		reader->prid[reader->nprov][1] = 0;
		reader->prid[reader->nprov][2] = cta_res[19];
		reader->prid[reader->nprov][3] = cta_res[20];
		memcpy(reader->sa[reader->nprov], reader->sa[0], 0x04);
		reader->nprov += 1;
	}
}

static int32_t ParseDataType(struct s_reader *reader, uint8_t dt, uint8_t *cta_res, uint16_t cta_lr)
{
	char ds[11], de[11];

	switch(dt)
	{
		case 0x02:
		{
			reader->prid[0][0] = 0x00;
			reader->prid[0][1] = 0x00;
			reader->prid[0][2] = cta_res[19];
			reader->prid[0][3] = cta_res[20];

			reader->prid[1][0] = 0x00;
			reader->prid[1][1] = 0x00;
			reader->prid[1][2] = 0x00;
			reader->prid[1][3] = 0x00;
			memcpy(reader->sa[1], reader->sa[0], 0x04);
			reader->nprov += 1;
			reader->caid = (SYSTEM_NAGRA | cta_res[25]);
			rdr_log_dbg(reader, D_READER, "CAID : %04X", reader->caid);
			return OK;
		}

		case IRDINFO: // case 0x03
		{
			if(cta_res[21] == 0x9C)
			{
				uint32_t timestamp = b2i(0x04, cta_res + 22);
				reader->card_valid_to = tier_date(timestamp, de, 11);
				rdr_log(reader, "Provider Sys ID: %02X %02X is active to: %s", cta_res[19], cta_res[20], de);
			}
			return OK;
		}

		case SYSID: // case 0x05
		{
			IDEA_KEY_SCHEDULE ks;
			memcpy(reader->edata,cta_res + 26, 0x70);
			reader->dt5num = cta_res[20];
			rsa_decrypt(reader->edata, 0x70, reader->out, reader->mod1, reader->mod1_length);

			if(reader->dt5num == 0x00)
			{
				memcpy(reader->kdt05_00,&reader->out[18], 0x5C + 2);
				memcpy(&reader->kdt05_00[0x5C + 2], cta_res + 26 + 0x70, 6);
				memcpy(reader->ideakey1, reader->out, 16);
				memcpy(reader->block3, cta_res + 26 + 0x70 + 6, 8);
				idea_set_encrypt_key(reader->ideakey1, &ks);
				memset(reader->v, 0, sizeof(reader->v));
				idea_cbc_encrypt(reader->block3, reader->iout, 8, &ks, reader->v, IDEA_DECRYPT);
				memcpy(&reader->kdt05_00[0x5C + 2 + 6],reader->iout, 8);
				rdr_log_dump_dbg(reader, D_READER, reader->kdt05_00, sizeof(reader->kdt05_00), "DT05_00: ");
			}

			if(reader->dt5num == 0x10)
			{
				memcpy(reader->kdt05_10, &reader->out[16], 6 * 16);
				memcpy(reader->ideakey1, reader->out, 16);
				memcpy(reader->block3, cta_res + 26 + 0x70, 8);
				idea_set_encrypt_key(reader->ideakey1, &ks);
				memset(reader->v, 0, sizeof(reader->v));
				idea_cbc_encrypt(reader->block3, reader->iout, 8, &ks, reader->v, IDEA_DECRYPT);
				memcpy(&reader->kdt05_10[6 * 16],reader->iout,8);
				rdr_log_dump_dbg(reader, D_READER, reader->kdt05_10, sizeof(reader->kdt05_10), "DT05_10: ");
			}
			return OK;
		}

		case TIERS: // case 0x0C
		{
			uint16_t chid;
			if((cta_lr >= 0x30) && (chid = b2i(0x02, cta_res + 23)))
			{
				uint32_t id = b2i(0x02, cta_res + 19);
				uint32_t start_date;
				uint32_t expire_date1;
				uint32_t expire_date2;
				uint32_t expire_date;

				switch(reader->caid)
				{
					case 0x1860: // HD03
						start_date = b2i(0x04, cta_res + 42);
						expire_date1 = b2i(0x04, cta_res + 28);
						expire_date2 = b2i(0x04, cta_res + 46);
						expire_date = expire_date1 <= expire_date2 ? expire_date1 : expire_date2;
						break;

					case 0x186A: // HD04, HD05
						start_date = b2i(0x04, cta_res + 53);
						expire_date1 = b2i(0x04, cta_res + 39);
						expire_date2 = b2i(0x04, cta_res + 57);
						expire_date = expire_date1 <= expire_date2 ? expire_date1 : expire_date2;
						break;

					default: // unknown card
						start_date = 1;
						expire_date = 0x569EFB7F;
				}

				cs_add_entitlement(reader,
					reader->caid,
					id,
					chid,
					0,
					tier_date(start_date, ds, 11),
					tier_date(expire_date, de, 11),
					4,
					1);
				rdr_log(reader, "|%04X|%04X    |%s  |%s  |", id, chid, ds, de);
				addProvider(reader, cta_res);
			}
			return OK;
		}

		default:
			return OK;
	}
	return ERROR;
}

static int32_t CAK7do_cmd(struct s_reader *reader, uint8_t dt, uint8_t len, uint8_t *res, uint16_t *rlen, int32_t sub, uint8_t retlen)
{
	uint8_t dtdata[0x10];
	memset(dtdata, 0xCC, len);

	dtdata[7] = 0x04;
	dtdata[8] = 0x04;

	dtdata[9]  = (sub >> 16) & 0xFF;
	dtdata[10] = (sub >> 8) & 0xFF;
	dtdata[11] = (sub) & 0xFF;

	dtdata[12] = dt;

	do_cak7_cmd(reader, res, rlen, dtdata, sizeof(dtdata), retlen);

	return OK;
}

static int32_t CAK7GetDataType(struct s_reader *reader, uint8_t dt)
{
	def_resp;

	int32_t sub = 0x00;
	uint8_t retlen = 0x10;
	while(1)
	{
		CAK7do_cmd(reader, dt, 0x10, cta_res, &cta_lr, sub, retlen);
		// hier eigentlich check auf 90 am ende usw... obs halt klarging ...

		uint32_t newsub = (cta_res[9] << 16) + (cta_res[10] << 8) + (cta_res[11]);
		if(newsub == 0xFFFFFF)
		{
			break;
		}

		if(cta_res[12] == dt)
		{
			uint8_t oretlen = retlen;
			retlen = cta_res[13] + 0x10 + 0x2;

			while(retlen % 0x10 != 0x00)
			{
				retlen++;
			}

			if(retlen == oretlen)
			{
				sub = newsub + 1;
				retlen = 0x10;
				ParseDataType(reader, dt, cta_res, cta_lr);
			}
		}
		else
		{
			break;
		}
	}

	return OK;
}

void sub_6AD78(uint32_t *dinit) // gbox function
{
	uint32_t v0 = (uint32_t) * dinit;
	double f0;
	f0 = v0;
	double f12 = 16807;
	double f15 = 2147483647;
	f12 = f0 * f12;
	double v12;
	v12 = fmod(f12, f15);
	*dinit = v12;
}

void calc_cak7_exponent(uint32_t *dinit, uint8_t *out, uint8_t len)
{
	memset(out, 0x00, len);

	sub_6AD78(dinit);

	int nR4 = 0;
	int nR5 = 0;
	while(true)
	{
		uint32_t nR0 = (uint32_t)* dinit;
		int nR3 = nR4 + 3;
		nR5 += 4;

		if(nR3 > len)
		{
			break;
		}

		out[nR5 - 1] = ((nR0    ) & 0xFF);
		out[nR5 - 2] = ((nR0 >> 8) & 0xFF);
		out[nR5 - 3] = ((nR0 >> 16) & 0xFF);
		out[nR5 - 4] = ((nR0 >> 24) & 0xFF);
		nR4 += 4;
		sub_6AD78(dinit);

	}

	uint32_t nR0 = (uint32_t)* dinit;
	while(nR4 < len)
	{
		out[nR4] = nR0 & 0xFF;
		nR4++;
		nR0 >>= 8;
	}

	out[0] &= 0x03;
	out[0x10] |= 0x01;

}

void CAK7_getCamKey(struct s_reader *reader)
{
	def_resp;
	uint8_t cmd0e[] = {0xCC,0xCC,0xCC,0xCC,0x00,0x00,0x09,0x0E,0x83,0x00,0x00,0x00,0x00,0x00,0x64,0x65,0x6D,0x6F,0x34,0x11,0x9D,
	0x7E,0xEE,0xCE,0x53,0x09,0x80,0xAE,0x6B,0x5A,0xEE,0x3A,0x41,0xCE,0x09,0x75,0xEF,0xA6,0xBF,0x1E,0x98,0x4F,
	0xA4,0x11,0x6F,0x43,0xCA,0xCD,0xD0,0x6E,0x69,0xFA,0x25,0xC1,0xF9,0x11,0x8E,0x7A,0xD0,0x19,0xC0,0xEB,0x00,
	0xC0,0x57,0x2A,0x40,0xB7,0xFF,0x8A,0xBB,0x25,0x21,0xD7,0x50,0xE7,0x35,0xA1,0x85,0xCD,0xA6,0xD3,0xDE,0xB3,
	0x3D,0x16,0xD4,0x94,0x76,0x8A,0x82,0x8C,0x70,0x25,0xD4,0x00,0xD0,0x64,0x8C,0x26,0xB9,0x5F,0x44,0xFF,0x73,
	0x70,0xAB,0x43,0xF5,0x68,0xA2,0xB1,0xB5,0x8A,0x8E,0x02,0x5F,0x96,0x06,0xA8,0xC3,0x4F,0x15,0xCD,0x99,0xC2,
	0x69,0xB8,0x35,0x68,0x11,0x4C,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0xCC,0xCC,0xCC,0xCC};

	if (reader->nuid_length == 4)
	{
		memcpy(cmd0e + 132, reader->nuid, reader->nuid_length); // inject NUID
	}

	do_cak7_cmd(reader,cta_res, &cta_lr, cmd0e, sizeof(cmd0e), 0x20);
	reader->dword_83DBC =  (cta_res[18] << 24);
	reader->dword_83DBC += (cta_res[19] << 16);
	reader->dword_83DBC += (cta_res[20] <<  8);
	reader->dword_83DBC += (cta_res[21]      );
	calc_cak7_exponent(&reader->dword_83DBC, reader->cak7expo, 0x11);
	memcpy(reader->cardid,cta_res + 14, 4);
	rdr_log_dump_dbg(reader, D_READER, reader->cardid, 0x04, "CardSerial: ");
	memcpy(reader->hexserial + 2, reader->cardid, 4);
	memcpy(reader->sa[0], reader->cardid, 3);
	memcpy(reader->sa[1], reader->sa[0], 4);
	unsigned long datal = (cta_res[9] << 24) + (cta_res[10] << 16) + (cta_res[11] << 8) + (cta_res[12]);
	datal++;
	reader->data2[0] = (datal >> 24) & 0xFF;
	reader->data2[1] = (datal >> 16) & 0xFF;
	reader->data2[2] = (datal >>  8) & 0xFF;
	reader->data2[3] = (datal      ) & 0xFF;

	BN_CTX *ctx0 = BN_CTX_new();
#ifdef WITH_LIBCRYPTO
	BN_CTX_start(ctx0);
#endif
	BIGNUM *bnN0 = BN_CTX_get(ctx0);
	BIGNUM *bnE0 = BN_CTX_get(ctx0);
	BIGNUM *bnCT0 = BN_CTX_get(ctx0);
	BIGNUM *bnPT0 = BN_CTX_get(ctx0);
	BN_bin2bn(&reader->mod50[0], 0x50, bnN0);
	BN_bin2bn(&reader->cak7expo[0], 0x11, bnE0);
	BN_bin2bn(&reader->data50[0], 0x50, bnCT0);
	BN_mod_exp(bnPT0, bnCT0, bnE0, bnN0, ctx0);
	memset(reader->data, 0x00, sizeof(reader->data));
	BN_bn2bin(bnPT0, reader->data + (0x50 - BN_num_bytes(bnPT0)));
	BN_CTX_end(ctx0);
	BN_CTX_free(ctx0);

	memcpy(&reader->step1[0], d00ff, 4);
	memcpy(&reader->step1[4], reader->data, 0x50);
	memcpy(&reader->step1[4 + 0x50], irdid, 0x04);
	memcpy(&reader->step1[4 + 4 + 0x50], data1, 0x04);
	memcpy(&reader->step1[4 + 4 + 4 + 0x50], reader->data2, 0x04);

	BN_CTX *ctx1 = BN_CTX_new();
#ifdef WITH_LIBCRYPTO
	BN_CTX_start(ctx1);
#endif
	BIGNUM *bnN1 = BN_CTX_get(ctx1);
	BIGNUM *bnE1 = BN_CTX_get(ctx1);
	BIGNUM *bnCT1 = BN_CTX_get(ctx1);
	BIGNUM *bnPT1 = BN_CTX_get(ctx1);
	BN_bin2bn(&reader->key60[0], 0x60, bnN1);
	BN_bin2bn(&reader->exp60[0], 0x60, bnE1);
	BN_bin2bn(&reader->step1[0], 0x60, bnCT1);
	BN_mod_exp(bnPT1, bnCT1, bnE1, bnN1, ctx1);
	BN_bn2bin(bnPT1, reader->data + (0x60 - BN_num_bytes(bnPT1)));
	BN_CTX_end(ctx1);
	BN_CTX_free(ctx1);

	memcpy(&reader->step2[0], d00ff, 4);
	memcpy(&reader->step2[4], reader->cardid, 4);
	memcpy(&reader->step2[8], reader->data, 0x60);

	BN_CTX *ctx2 = BN_CTX_new();
#ifdef WITH_LIBCRYPTO
	BN_CTX_start(ctx2);
#endif
	BIGNUM *bnN2 = BN_CTX_get(ctx2);
	BIGNUM *bnE2 = BN_CTX_get(ctx2);
	BIGNUM *bnCT2 = BN_CTX_get(ctx2);
	BIGNUM *bnPT2 = BN_CTX_get(ctx2);
	BN_bin2bn(&reader->kdt05_10[0], 0x68, bnN2);
	BN_bin2bn(public_exponent, 3, bnE2);
	BN_bin2bn(&reader->step2[0], 0x68, bnCT2);
	BN_mod_exp(bnPT2, bnCT2, bnE2, bnN2, ctx2);
	BN_bn2bin(bnPT2, reader->data + (0x68 - BN_num_bytes(bnPT2)));
	BN_CTX_end(ctx2);
	BN_CTX_free(ctx2);

	memcpy(&reader->step3[0], d00ff, 4);
	memcpy(&reader->step3[4], reader->data, 0x68);

	BN_CTX *ctx3 = BN_CTX_new();
#ifdef WITH_LIBCRYPTO
	BN_CTX_start(ctx3);
#endif
	BIGNUM *bnN3 = BN_CTX_get(ctx3);
	BIGNUM *bnE3 = BN_CTX_get(ctx3);
	BIGNUM *bnCT3 = BN_CTX_get(ctx3);
	BIGNUM *bnPT3 = BN_CTX_get(ctx3);
	BN_bin2bn(&reader->kdt05_00[0], 0x6c, bnN3);
	BN_bin2bn(public_exponent, 3, bnE3);
	BN_bin2bn(&reader->step3[0], 0x6c, bnCT3);
	BN_mod_exp(bnPT3, bnCT3, bnE3, bnN3, ctx3);
	BN_bn2bin(bnPT3, reader->data + (0x6c - BN_num_bytes(bnPT3)));
	BN_CTX_end(ctx3);
	BN_CTX_free(ctx3);

	uint8_t cmd03[] = {0xCC,0xCC,0xCC,0xCC, 0x00,0x00,0x0A,0x03,0x6C,
	0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
	0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
	0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
	0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
	0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
	0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
	0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,
	0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC };

	memcpy(&cmd03[9],reader->data,0x6c);
	do_cak7_cmd(reader,cta_res,&cta_lr,cmd03,sizeof(cmd03),0x90);
	memcpy(reader->encrypted,&cta_res[10],0x68);

	BN_CTX *ctx = BN_CTX_new();
#ifdef WITH_LIBCRYPTO
	BN_CTX_start(ctx);
#endif
	BIGNUM *bnN = BN_CTX_get(ctx);
	BIGNUM *bnE = BN_CTX_get(ctx);
	BIGNUM *bnCT = BN_CTX_get(ctx);
	BIGNUM *bnPT = BN_CTX_get(ctx);
	BN_bin2bn(&reader->kdt05_10[0], 104, bnN);
	BN_bin2bn(public_exponent, 3, bnE);
	BN_bin2bn(&reader->encrypted[0], 104, bnCT);
	BN_mod_exp(bnPT, bnCT, bnE, bnN, ctx);
	memset(reader->result, 0, 104);
	BN_bn2bin(bnPT, reader->result + (104 - BN_num_bytes(bnPT)));
	BN_CTX_end(ctx);
	BN_CTX_free(ctx);

	//uint8_t stillencrypted[0x50];
	memcpy(reader->stillencrypted,&reader->result[12],0x50);

	//uint8_t resultrsa[0x50];
	BN_CTX *ctxs = BN_CTX_new();
#ifdef WITH_LIBCRYPTO
	BN_CTX_start(ctxs);
#endif
	BIGNUM *bnNs  = BN_CTX_get(ctxs);
	BIGNUM *bnEs  = BN_CTX_get(ctxs);
	BIGNUM *bnCTs = BN_CTX_get(ctxs);
	BIGNUM *bnPTs = BN_CTX_get(ctxs);
	BN_bin2bn(&reader->mod50[0], reader->mod50_length, bnNs);
	BN_bin2bn(&reader->cak7expo[0], 0x11, bnEs);
	BN_bin2bn(&reader->stillencrypted[0], 0x50, bnCTs);
	BN_mod_exp(bnPTs, bnCTs, bnEs, bnNs, ctxs);
	BN_bn2bin(bnPTs, reader->resultrsa + (0x50 - BN_num_bytes(bnPTs)));
	BN_CTX_end(ctxs);
	BN_CTX_free(ctxs);

	uint8_t mdc_hash[MDC2_DIGEST_LENGTH];
	memset(mdc_hash,0x00,MDC2_DIGEST_LENGTH);
	MDC2_CTX c;
	MDC2_Init(&c);
	MDC2_Update(&c, reader->resultrsa, sizeof(reader->resultrsa));
	MDC2_Final(&(mdc_hash[0]), &c);

	memcpy(&reader->cak7_aes_key[16],mdc_hash,16);
	memcpy(reader->cak7_aes_key,mdc_hash,16);
}

static int32_t nagra3_card_init(struct s_reader *reader, ATR *newatr)
{
	get_atr;

	memset(reader->hexserial, 0x00, 0x08);
	reader->cak7_seq = 0;
	cs_clear_entitlement(reader);

	if(memcmp(atr + 11, "DNASP4", 6) == 0)
	{
		memcpy(reader->rom, atr + 11, 15);
		rdr_log(reader,"Rom revision: %.15s", reader->rom);
	}
	else
	{
		return ERROR;
	}

	// check the completeness of the required CAK7 keys
	if(reader->mod1_length && reader->data50_length && reader->mod50_length && reader->key60_length && reader->exp60_length && reader->nuid_length)
	{
		rdr_log_dbg(reader, D_READER, "All parameters are set.");
	}
	else
	{
		rdr_log(reader, "ERROR: Not all required parameters are set!");
		reader->card_status = CARD_FAILURE;
		return ERROR;
	}

	reader->nprov = 1;

	//CAK7GetDataType(reader, 0x09);
	CAK7GetDataType(reader, 0x05);
	CAK7_getCamKey(reader);
	//CAK7GetDataType(reader, 0x09);
	CAK7GetDataType(reader, 0x02); // sysid+caid

	rdr_log(reader, "ready for requests");
	return OK;
}

static int32_t nagra3_card_info(struct s_reader *reader)
{
	char tmp[4 * 3 + 1];
	rdr_log(reader, "ROM:    %c %c %c %c %c %c %c %c", reader->rom[0], reader->rom[1], reader->rom[2], reader->rom[3], reader->rom[4], reader->rom[5], reader->rom[6], reader->rom[7]);
	rdr_log(reader, "REV:    %c %c %c %c %c %c", reader->rom[9], reader->rom[10], reader->rom[11], reader->rom[12], reader->rom[13], reader->rom[14]);
	rdr_log_sensitive(reader, "SER:    {%s}", cs_hexdump(1, reader->hexserial + 2, 4, tmp, sizeof(tmp)));
	rdr_log(reader, "CAID:   %04X", reader->caid);
	rdr_log(reader, "Prv.ID: %s(sysid)", cs_hexdump(1, reader->prid[0], 4, tmp, sizeof(tmp)));
	CAK7GetDataType(reader, 0x03);
	cs_clear_entitlement(reader); // reset the entitlements
	rdr_log(reader, "-----------------------------------------");
	rdr_log(reader, "|id  |tier    |valid from  |valid to    |");
	rdr_log(reader, "+----+--------+------------+------------+");
	CAK7GetDataType(reader, 0x0C);
	rdr_log(reader, "-----------------------------------------");
	uint8_t i;
	for(i = 1; i < reader->nprov; i++)
	{
		rdr_log(reader, "Prv.ID: %s", cs_hexdump(1, reader->prid[i], 4, tmp, sizeof(tmp)));
	}

	return OK;
}

static void nagra3_post_process(struct s_reader *reader)
{
	if((reader->cak7_camstate & 64) == 64)
	{
		rdr_log(reader, "renew Session Key: CAK7");
		CAK7_getCamKey(reader);
	}
}

static int32_t nagra3_do_ecm(struct s_reader *reader, const ECM_REQUEST *er, struct s_ecm_answer *ea)
{
	def_resp;

	uint8_t ecmreq[0xC0];
	memset(ecmreq,0xCC,0xC0);

	ecmreq[ 7] = 0x05;
	ecmreq[ 8] = 0x8A;
	ecmreq[ 9] = 0x00;
	ecmreq[10] = 0x00;
	ecmreq[11] = 0x00;
	ecmreq[12] = 0x00;
	ecmreq[13] = 0x01;
	memcpy(&ecmreq[14], er->ecm + 4, er->ecm[4] + 1);

	do_cak7_cmd(reader, cta_res, &cta_lr, ecmreq, sizeof(ecmreq), 0xB0);

	if(cta_res[cta_lr - 2] != 0x90 && cta_res[cta_lr - 1] != 0x00)
	{
		rdr_log(reader, "(ECM) Reader will be restart now cause: %02X %02X card answer!!!", cta_res[cta_lr - 2], cta_res[cta_lr - 1]);
		reader->card_status = CARD_NEED_INIT;
		add_job(reader->client, ACTION_READER_RESTART, NULL, 0);
	}

	if(cta_res[27] == 0x5C)
	{
		uint8_t _cwe0[8];
		uint8_t _cwe1[8];

		if(cta_res[78] == 0x01)
		{
			rdr_log (reader,"Swap dcw is at use !");
			memcpy(_cwe0,&cta_res[52], 0x08);
			memcpy(_cwe1,&cta_res[28], 0x08);
		}
		else
		{
			memcpy(_cwe0,&cta_res[28], 0x08);
			memcpy(_cwe1,&cta_res[52], 0x08);
		}

		if(!reader->cwekey_length)
		{
			rdr_log_dbg(reader, D_READER, "ERROR: CWPK is not set, can not decrypt CW");
			return ERROR;
		}
		des_ecb3_decrypt(_cwe0, reader->cwekey);
		des_ecb3_decrypt(_cwe1, reader->cwekey);

		int chkok = 1;
		if(((_cwe0[0] + _cwe0[1] + _cwe0[2]) & 0xFF) != _cwe0[3])
		{
			chkok = 0;
			rdr_log_dbg(reader, D_READER, "CW0 checksum error [0]");
		}

		if(((_cwe0[4] + _cwe0[5] + _cwe0[6]) & 0xFF) != _cwe0[7])
		{
			chkok = 0;
			rdr_log_dbg(reader, D_READER, "CW0 checksum error [1]");
		}

		if(((_cwe1[0] + _cwe1[1] + _cwe1[2]) & 0xFF) != _cwe1[3])
		{
			chkok = 0;
			rdr_log_dbg(reader, D_READER, "CW1 checksum error [0]");
		}

		if(((_cwe1[4] + _cwe1[5] + _cwe1[6]) & 0xFF) != _cwe1[7])
		{
			chkok = 0;
			rdr_log_dbg(reader, D_READER, "CW1 checksum error [1]");
		}

		reader->cak7_camstate = cta_res[4];
		if(chkok == 1)
		{
			rdr_log_dbg(reader, D_READER, "CW Decrypt ok");
			memcpy(ea->cw, _cwe0, 0x08);
			memcpy(ea->cw + 8, _cwe1, 0x08);
			return OK;
		}
	}

	return ERROR;
}

static int32_t nagra3_do_emm(struct s_reader *reader, EMM_PACKET *ep)
{
	def_resp;
	uint8_t emmreq[0xC0];
	memset(emmreq, 0xCC, 0xC0);
	emmreq[ 7] = 0x05;
	emmreq[ 8] = 0x8A;
	emmreq[ 9] = 0x00;
	emmreq[10] = 0x00;
	emmreq[11] = 0x00;
	emmreq[12] = 0x00;
	emmreq[13] = 0x01;
	memcpy(&emmreq[14], ep->emm + 9, ep->emm[9] + 1);
	do_cak7_cmd(reader, cta_res, &cta_lr, emmreq, sizeof(emmreq), 0xB0);

	if(cta_res[cta_lr-2] != 0x90 && cta_res[cta_lr-1] != 0x00)
	{
		rdr_log(reader, "(EMM) Reader will be restart now cause: %02X %02X card answer!!!", cta_res[cta_lr - 2], cta_res[cta_lr - 1]);
		reader->card_status = CARD_NEED_INIT;
		add_job(reader->client, ACTION_READER_RESTART, NULL, 0);
	}
	return OK;
}

const struct s_cardsystem reader_nagracak7 =
{
	.desc           = "nagra merlin",
	.caids          = (uint16_t[]){ 0x18, 0 },
	.do_emm         = nagra3_do_emm,
	.do_ecm         = nagra3_do_ecm,
	.post_process   = nagra3_post_process,
	.card_info      = nagra3_card_info,
	.card_init      = nagra3_card_init,
	.get_emm_type   = nagra_get_emm_type,
	.get_emm_filter = nagra_get_emm_filter,
};

#endif
