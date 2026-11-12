/*
 * Copyright (c) 2026 SONiC-VPP contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include <sonic_ext/sonic_ext_vnet_buf.h>

sonic_ext_vnet_buf_main_t sonic_ext_vnet_buf_main;

static vlib_main_t vm;
static vlib_buffer_main_t bm;
static vlib_buffer_t buffers[2] __clib_aligned (CLIB_CACHE_LINE_BYTES);
static sonic_ext_vnet_buf_pool_t pools[1];
static sonic_ext_vnet_buf_t slots[2];

#define CHECK(condition)                                                                           \
  do                                                                                               \
    {                                                                                              \
      if (!(condition))                                                                            \
	{                                                                                          \
	  fprintf (stderr, "%s:%d: %s\n", __func__, __LINE__, #condition);                         \
	  return 1;                                                                                \
	}                                                                                          \
    }                                                                                              \
  while (0)

static void
setup (void)
{
  memset (&vm, 0, sizeof (vm));
  memset (&bm, 0, sizeof (bm));
  memset (buffers, 0, sizeof (buffers));
  memset (slots, 0, sizeof (slots));
  memset (pools, 0, sizeof (pools));
  vm.buffer_main = &bm;
  bm.buffer_mem_start = (uword) buffers;
  pools[0].stride = sizeof (buffers[0]) / CLIB_CACHE_LINE_BYTES;
  sonic_ext_vnet_buf_main.slots = slots;
  sonic_ext_vnet_buf_main.pools = pools;
  sonic_ext_vnet_buf_main.refs = 1;
}

#define PBH SONIC_EXT_VNET_BUF_PBH_LAG_HASH
#define TEST_FLAG_ONE  (1 << 1)
#define TEST_FLAG_PAIR (3 << 2)

static int
first_claim (void)
{
  sonic_ext_vnet_buf_t expected;
  sonic_ext_vnet_buf_t *sb;

  memset (&slots[0], 0xff, sizeof (slots[0]));
  memset (&expected, 0, sizeof (expected));
  sb = sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], PBH, PBH);
  expected.flags = PBH;
  CHECK (sb == &slots[0]);
  CHECK (memcmp (sb, &expected, sizeof (*sb)) == 0);
  CHECK (buffers[0].flags & SONIC_EXT_BUFFER_F_VNET_BUF);
  return 0;
}

static int
repeated_claim (void)
{
  sonic_ext_vnet_buf_t *sb;

  sb = sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], PBH, PBH);
  sb->pbh_lag_hash = 0x12345678;
  CHECK (sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], PBH, PBH) == sb);
  CHECK (sb->pbh_lag_hash == 0x12345678);
  CHECK (sonic_ext_vnet_buf_slot_if_set (&vm, &buffers[0], PBH) == sb);
  sb->flags &= ~PBH;
  CHECK (sonic_ext_vnet_buf_slot_if_set (&vm, &buffers[0], PBH) == 0);
  CHECK (buffers[0].flags & SONIC_EXT_BUFFER_F_VNET_BUF);
  sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], PBH, PBH);
  CHECK (sb->pbh_lag_hash == 0x12345678);
  return 0;
}

static int
unclaimed_and_clone (void)
{
  sonic_ext_vnet_buf_t *sb;

  memset (&slots[0], 0xff, sizeof (slots[0]));
  CHECK (sonic_ext_vnet_buf_slot_if_set (&vm, &buffers[0], PBH) == 0);
  sb = sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], PBH, PBH);
  sb->pbh_lag_hash = 1234;
  buffers[1].flags = buffers[0].flags;
  CHECK (sonic_ext_vnet_buf_slot_if_set (&vm, &buffers[1], PBH) == 0);

  buffers[0].flags = 0;
  sonic_ext_vnet_buf_main.slots = 0;
  sonic_ext_vnet_buf_main.pools = 0;
  CHECK (sonic_ext_vnet_buf_slot_if_set (&vm, &buffers[0], PBH) == 0);
  return 0;
}

static int
flags (void)
{
  sonic_ext_vnet_buf_t *sb;

  memset (&slots[0], 0xff, sizeof (slots[0]));
  CHECK (sonic_ext_vnet_buf_flags_get (&vm, &buffers[0], TEST_FLAG_ONE) == 0);
  CHECK (sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], TEST_FLAG_ONE, 0) == 0);
  CHECK ((buffers[0].flags & SONIC_EXT_BUFFER_F_VNET_BUF) == 0);

  sb = sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], TEST_FLAG_PAIR, 2 << 2);
  CHECK (sb == &slots[0]);
  CHECK (sb->flags == (2 << 2));
  CHECK (sonic_ext_vnet_buf_flags_get (&vm, &buffers[0], TEST_FLAG_ONE) == 0);
  CHECK (sonic_ext_vnet_buf_slot_if_set (&vm, &buffers[0], PBH) == 0);

  sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], TEST_FLAG_ONE, TEST_FLAG_ONE);
  sb = sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], PBH, PBH);
  sb->pbh_lag_hash = 42;
  CHECK (sonic_ext_vnet_buf_flags_get (&vm, &buffers[0], TEST_FLAG_PAIR) == (2 << 2));
  CHECK (sonic_ext_vnet_buf_flags_get (&vm, &buffers[0], TEST_FLAG_ONE) == TEST_FLAG_ONE);

  CHECK (sonic_ext_vnet_buf_flags_set (&vm, &buffers[0], TEST_FLAG_PAIR, 0) == sb);
  CHECK (sonic_ext_vnet_buf_flags_get (&vm, &buffers[0], TEST_FLAG_PAIR) == 0);
  CHECK (sonic_ext_vnet_buf_flags_get (&vm, &buffers[0], TEST_FLAG_ONE) == TEST_FLAG_ONE);
  CHECK (sonic_ext_vnet_buf_slot_if_set (&vm, &buffers[0], PBH) == sb);
  CHECK (sb->pbh_lag_hash == 42);

  buffers[1].flags = buffers[0].flags;
  CHECK (sonic_ext_vnet_buf_flags_get (&vm, &buffers[1], 0xffff) == 0);
  return 0;
}

int
main (int argc, char **argv)
{
  setup ();
  if (argc != 2)
    return 2;
  if (strcmp (argv[1], "first-claim") == 0)
    return first_claim ();
  if (strcmp (argv[1], "repeated-claim") == 0)
    return repeated_claim ();
  if (strcmp (argv[1], "unclaimed-clone") == 0)
    return unclaimed_and_clone ();
  if (strcmp (argv[1], "flags") == 0)
    return flags ();
  return 2;
}
