/*
 * Copyright (C) 2022 Richard Yao
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/random.h>
#include <linux/vmalloc.h>

#define	POC_DRIVER "urandom-fast"

#define	MIN(a, b) ((a) < (b) ? (a) : (b))
#define	P2ROUNDUP(x, align)	((((x) - 1) | ((align) - 1)) + 1)

void __percpu *spl_pseudo_entropy;

/*
 * spl_rand_next()/spl_rand_jump() are copied from the following CC-0 licensed
 * file:
 *
 * http://xorshift.di.unimi.it/xorshift128plus.c
 */

static inline uint64_t
spl_rand_next(uint64_t *s)
{
	uint64_t s1 = s[0];
	const uint64_t s0 = s[1];
	s[0] = s0;
	s1 ^= s1 << 23; // a
	s[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5); // b, c
	return (s[1] + s0);
}

static inline void
spl_rand_jump(uint64_t *s)
{
	static const uint64_t JUMP[] =
	    { 0x8a5cd789635d2dff, 0x121fd2155c472f96 };

	uint64_t s0 = 0;
	uint64_t s1 = 0;
	int i, b;
	for (i = 0; i < sizeof (JUMP) / sizeof (*JUMP); i++)
		for (b = 0; b < 64; b++) {
			if (JUMP[i] & 1ULL << b) {
				s0 ^= s[0];
				s1 ^= s[1];
			}
			(void) spl_rand_next(s);
		}

	s[0] = s0;
	s[1] = s1;
}

int
random_get_pseudo_bytes(uint8_t *ptr, size_t len)
{
	uint64_t *xp, s[2];

	xp = get_cpu_ptr(spl_pseudo_entropy);

	s[0] = xp[0];
	s[1] = xp[1];

	while (len) {
		union {
			uint64_t ui64;
			uint8_t byte[sizeof (uint64_t)];
		}entropy;
		int i = MIN(len, sizeof (uint64_t));

		len -= i;
		entropy.ui64 = spl_rand_next(s);

		while (i--)
			*ptr++ = entropy.byte[i];
	}

	xp[0] = s[0];
	xp[1] = s[1];

	put_cpu_ptr(spl_pseudo_entropy);

	return (0);
}

static ssize_t
poc_read(struct file *filep, char *buffer, size_t len, loff_t *offset)
{
	int i;
	uint64_t *xp;
	char *buf;
	unsigned long copied = 0, left = len;

	if (!access_ok(buffer, len))
		return (-EFAULT);

	buf = vmalloc(P2ROUNDUP(len, sizeof (uint64_t)));

	if (buf == NULL)
		return (-ENOMEM);

	xp = get_cpu_ptr(spl_pseudo_entropy);

	for (i = 0; i < len; i += sizeof (uint64_t)) {
		*(uint64_t *)(buf + i) = spl_rand_next(xp);
	}

	put_cpu_ptr(spl_pseudo_entropy);

	while (left != 0) {
		left = copy_to_user(buffer + copied, buf + copied, len);
		copied = len - left;
	}

	vfree(buf);

	return (len);
}

static struct file_operations poc_fops =
{
	.read		= poc_read,
	.owner		= THIS_MODULE,
};

static struct miscdevice poc_misc = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= POC_DRIVER,
	.fops		= &poc_fops,
	.mode		= S_IRWXU | S_IRWXG | S_IRWXO
};

/*
 * We initialize the random number generator with 128 bits of entropy from the
 * system random number generator. In the improbable case that we have a zero
 * seed, we fallback to the system jiffies, unless it is also zero, in which
 * situation we use a preprogrammed seed. We step forward by 2^64 iterations to
 * initialize each of the per-cpu seeds so that the sequences generated on each
 * CPU are guaranteed to never overlap in practice.
 */
static int __init
spl_random_init(void)
{
	uint64_t s[2];
	int i = 0;

	spl_pseudo_entropy = __alloc_percpu(2 * sizeof (uint64_t),
	    sizeof (uint64_t));

	if (!spl_pseudo_entropy)
		return (-ENOMEM);

	get_random_bytes(s, sizeof (s));

	if (s[0] == 0 && s[1] == 0) {
		if (jiffies != 0) {
			s[0] = jiffies;
			s[1] = ~0 - jiffies;
		} else {
			(void) memcpy(s, "improbable seed", sizeof (s));
		}
		printk("SPL: get_random_bytes() returned 0 "
		    "when generating random seed. Setting initial seed to "
		    "0x%016llx%016llx.\n", cpu_to_be64(s[0]),
		    cpu_to_be64(s[1]));
	}

	for_each_possible_cpu(i) {
		uint64_t *wordp = per_cpu_ptr(spl_pseudo_entropy, i);

		spl_rand_jump(s);

		wordp[0] = s[0];
		wordp[1] = s[1];
	}

	return (0);
}

static void
spl_random_fini(void)
{
	free_percpu(spl_pseudo_entropy);
}

int
init_module(void)
{
	int error;

	if ((error = spl_random_init()))
		return (error);

	error = misc_register(&poc_misc);
	if (error) {
		spl_random_fini();
		return (error);
	}

	return (0);
}

void
cleanup_module(void)
{
	misc_deregister(&poc_misc);
	spl_random_fini();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard Yao");
MODULE_DESCRIPTION("Proof of Concept to demonstrate the speed of "
	"the PRNG in OpenZFS");
MODULE_VERSION("1.0");
