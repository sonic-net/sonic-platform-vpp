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

static int
first_claim (void)
{
  sonic_ext_vnet_buf_t expected;
  sonic_ext_vnet_buf_t *sb;

  memset (&slots[0], 0xff, sizeof (slots[0]));
  memset (&expected, 0, sizeof (expected));
  sb = sonic_ext_vnet_buf_claim (&vm, &buffers[0], SONIC_EXT_VNET_BUF_PBH_LAG_HASH);
  expected.valid = SONIC_EXT_VNET_BUF_PBH_LAG_HASH;
  CHECK (sb == &slots[0]);
  CHECK (memcmp (sb, &expected, sizeof (*sb)) == 0);
  CHECK (buffers[0].flags & SONIC_EXT_BUFFER_F_VNET_BUF);
  return 0;
}

static int
shared_claim (void)
{
  sonic_ext_vnet_buf_t *sb;

  for (int trim_first = 0; trim_first < 2; ++trim_first)
    {
      setup ();
      if (trim_first)
	{
	  sb = sonic_ext_vnet_buf_claim (&vm, &buffers[0], SONIC_EXT_VNET_BUF_TRIM);
	  sb->trim_flags = SONIC_EXT_TRIM_BUF_F_TRIMMED;
	  sonic_ext_vnet_buf_claim (&vm, &buffers[0], SONIC_EXT_VNET_BUF_PBH_LAG_HASH);
	  CHECK (sb->trim_flags == SONIC_EXT_TRIM_BUF_F_TRIMMED);
	  sb->pbh_lag_hash = 0x12345678;
	}
      else
	{
	  sb = sonic_ext_vnet_buf_claim (&vm, &buffers[0], SONIC_EXT_VNET_BUF_PBH_LAG_HASH);
	  sb->pbh_lag_hash = 0x12345678;
	  sonic_ext_vnet_buf_claim (&vm, &buffers[0], SONIC_EXT_VNET_BUF_TRIM);
	  CHECK (sb->pbh_lag_hash == 0x12345678);
	  sb->trim_flags = SONIC_EXT_TRIM_BUF_F_TRIMMED;
	}
      CHECK (sonic_ext_vnet_buf_find (&vm, &buffers[0], SONIC_EXT_VNET_BUF_TRIM) == sb);
      sb->trim_flags = 0;
      sonic_ext_vnet_buf_release (sb, SONIC_EXT_VNET_BUF_TRIM);
      CHECK (sonic_ext_vnet_buf_find (&vm, &buffers[0], SONIC_EXT_VNET_BUF_TRIM) == 0);
      CHECK (sonic_ext_vnet_buf_find (&vm, &buffers[0], SONIC_EXT_VNET_BUF_PBH_LAG_HASH) == sb);
      sonic_ext_vnet_buf_claim (&vm, &buffers[0], SONIC_EXT_VNET_BUF_TRIM);
      CHECK (sb->trim_flags == 0);
      sonic_ext_vnet_buf_release (sb, SONIC_EXT_VNET_BUF_PBH_LAG_HASH);
      CHECK (sonic_ext_vnet_buf_find (&vm, &buffers[0], SONIC_EXT_VNET_BUF_TRIM) == sb);
      CHECK (buffers[0].flags & SONIC_EXT_BUFFER_F_VNET_BUF);
    }
  return 0;
}

static int
unclaimed_and_clone (void)
{
  sonic_ext_vnet_buf_t *sb;

  memset (&slots[0], 0xff, sizeof (slots[0]));
  CHECK (sonic_ext_vnet_buf_find (&vm, &buffers[0], SONIC_EXT_VNET_BUF_PBH_LAG_HASH) == 0);
  sb = sonic_ext_vnet_buf_claim (&vm, &buffers[0], SONIC_EXT_VNET_BUF_PBH_LAG_HASH);
  sb->pbh_lag_hash = 1234;
  buffers[1].flags = buffers[0].flags;
  CHECK (sonic_ext_vnet_buf_find (&vm, &buffers[1], SONIC_EXT_VNET_BUF_PBH_LAG_HASH) == 0);

  buffers[0].flags = 0;
  sonic_ext_vnet_buf_main.slots = 0;
  sonic_ext_vnet_buf_main.pools = 0;
  CHECK (sonic_ext_vnet_buf_find (&vm, &buffers[0], SONIC_EXT_VNET_BUF_PBH_LAG_HASH) == 0);
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
  if (strcmp (argv[1], "shared-claim") == 0)
    return shared_claim ();
  if (strcmp (argv[1], "unclaimed-clone") == 0)
    return unclaimed_and_clone ();
  return 2;
}
