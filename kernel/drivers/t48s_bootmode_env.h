/* SPDX-License-Identifier: GPL-2.0 */
/*
 * U-Boot's "userenv" block on the Yealink T48S, as plain functions with no kernel
 * dependencies so that tools/test-bootmode.sh can run them on a PC against the
 * dumps. The includer defines t48s_env_crc(p, len): CRC-32 as zlib computes it.
 *
 * Layout (read from the stock flash, both before and after U-Boot rewrote it):
 * 4096 bytes at the start of the partition's first good block. Bytes 0 to 3 are
 * the CRC-32, little-endian, of bytes 4 to 4095. From byte 4: "name=value"
 * strings, each ending in a zero byte, and one more zero byte after the last.
 * What follows the list is not read (U-Boot leaves old text there).
 */
#ifndef T48S_BOOTMODE_ENV_H
#define T48S_BOOTMODE_ENV_H

#define T48S_ENV_SIZE	0x1000

static unsigned int t48s_env_le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (unsigned int)p[3] << 24;
}

/* the entry after p, or 0 at the end of the list or if the list runs off the block */
static const unsigned char *t48s_env_next(const unsigned char *env, const unsigned char *p)
{
	const unsigned char *end = env + T48S_ENV_SIZE;

	while (p < end && *p)
		p++;
	return p + 1 < end ? p + 1 : 0;
}

/* 1 if the CRC is right and the list ends inside the block */
static int t48s_env_valid(const unsigned char *env)
{
	const unsigned char *p = env + 4;

	if (t48s_env_crc(env + 4, T48S_ENV_SIZE - 4) != t48s_env_le32(env))
		return 0;
	while (*p) {
		p = t48s_env_next(env, p);
		if (!p)
			return 0;
	}
	return 1;
}

/* does the entry at p set this name? */
static int t48s_env_is(const unsigned char *p, const char *name)
{
	while (*name)
		if (*p++ != (unsigned char)*name++)
			return 0;
	return *p == '=';
}

/* Copy the value of name into val (n bytes, always terminated). 1 if set, else 0.
 * The block must be valid. */
static int t48s_env_get(const unsigned char *env, const char *name, char *val, int n)
{
	const unsigned char *p;
	int i = 0;

	val[0] = 0;
	for (p = env + 4; *p; p = t48s_env_next(env, p)) {
		if (!t48s_env_is(p, name))
			continue;
		while (*p != '=')
			p++;
		for (p++; *p && i < n - 1; p++)
			val[i++] = *p;
		val[i] = 0;
		return 1;
	}
	return 0;
}

/* Write into out the same list without name, in the same order, zero filled, with
 * its CRC. Returns how many entries were left out. The block must be valid. */
static int t48s_env_remove(const unsigned char *env, unsigned char *out, const char *name)
{
	const unsigned char *p, *q;
	unsigned char *o = out + 4;
	unsigned int crc;
	int i, removed = 0;

	for (i = 0; i < T48S_ENV_SIZE; i++)
		out[i] = 0;
	for (p = env + 4; *p; p = q) {
		q = t48s_env_next(env, p);
		if (t48s_env_is(p, name)) {
			removed++;
			continue;
		}
		while (p < q)
			*o++ = *p++;
	}
	crc = t48s_env_crc(out + 4, T48S_ENV_SIZE - 4);
	out[0] = crc;
	out[1] = crc >> 8;
	out[2] = crc >> 16;
	out[3] = crc >> 24;
	return removed;
}

#endif
