/*
 * Copyright (c) 2026 SONiC-VPP contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/**
 * @file
 * @brief Policy Based Hashing control plane: profiles, tables and
 * interface attachment.
 */

#include <sonic_ext/sonic_ext.h>
#include <sonic_ext/pbh.h>
#include <sonic_ext/sonic_ext_vnet_buf.h>

#include <vnet/feature/feature.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/ip/ip.h>
#include <vnet/l2/l2_input.h>
#include <vnet/l2/l2_bd.h>
#include <vnet/bonding/node.h>

sonic_ext_pbh_main_t sonic_ext_pbh_main;

/*
 * Profiles
 */

static int
sonic_ext_pbh_field_cmp (void *va, void *vb)
{
  sonic_ext_pbh_hash_field_t *a = va, *b = vb;

  /* Only seq orders the vector; ties keep whatever order the caller gave,
   * which is immaterial because a tie is XOR-folded. */
  return (int) a->seq - (int) b->seq;
}

/*
 * The datapath walks the field vector as runs of equal sequence_id, so the
 * sort is not a convenience -- it is what makes the walk correct.  Doing it
 * here means the node never sorts and never re-checks.
 *
 * On add, *profile_index selects between create and replace: ~0 allocates a
 * new profile, anything else replaces the field vector of the profile
 * already at that index.  Replace exists because SAI models a PBH hash
 * update as a set on an existing object, and rules carry the profile index
 * -- reallocating would strand every rule that references the hash.  Safe
 * without a barrier of its own because the API handler is not mp-safe, so
 * workers are parked for the duration (api_shared.c).
 */
int
sonic_ext_pbh_profile_add_del (sonic_ext_pbh_hash_field_t *fields, int is_add,
                               u32 *profile_index)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  sonic_ext_pbh_profile_t *p;
  u32 i, n_groups = 0;

  if (!is_add)
    {
      if (pool_is_free_index (pm->profiles, *profile_index))
        return VNET_API_ERROR_NO_SUCH_ENTRY;

      p = pool_elt_at_index (pm->profiles, *profile_index);
      vec_free (p->fields);
      pool_put (pm->profiles, p);
      return 0;
    }

  if (*profile_index != ~0 && pool_is_free_index (pm->profiles, *profile_index))
    {
      vec_free (fields);
      return VNET_API_ERROR_NO_SUCH_ENTRY;
    }

  if (vec_len (fields) == 0)
    {
      vec_free (fields);
      return VNET_API_ERROR_INVALID_VALUE;
    }

  for (i = 0; i < vec_len (fields); i++)
    if (fields[i].field >= SONIC_EXT_PBH_HF_N_FIELDS)
      {
        vec_free (fields);
        return VNET_API_ERROR_INVALID_VALUE;
      }

  vec_sort_with_function (fields, sonic_ext_pbh_field_cmp);

  for (i = 0; i < vec_len (fields); i++)
    if (i == 0 || fields[i].seq != fields[i - 1].seq)
      n_groups++;

  /* Every way of failing is behind us, so the old vector can go. */
  if (*profile_index != ~0)
    {
      p = pool_elt_at_index (pm->profiles, *profile_index);
      vec_free (p->fields);
    }
  else
    pool_get_zero (pm->profiles, p);

  p->fields = fields;
  p->n_groups = n_groups;
  *profile_index = p - pm->profiles;
  return 0;
}

/*
 * Rules
 */

/*
 * Which encapsulation the qualifiers describe.
 *
 * VXLAN is identified by "UDP to the port the rule names" -- there is
 * nothing in the packet that says VXLAN, so a rule that does not pin the
 * destination port cannot be a VXLAN rule.  Everything GRE-shaped is
 * self-describing and is left to ip_inner_resolve().
 */
sonic_ext_pbh_encap_t
sonic_ext_pbh_encap_from_match (const sonic_ext_pbh_match_t *m)
{
  u8 proto = 0;

  if (m->present & SONIC_EXT_PBH_Q_IP_PROTOCOL)
    proto = m->ip_protocol;
  else if (m->present & SONIC_EXT_PBH_Q_IPV6_NEXT_HEADER)
    proto = m->ipv6_next_header;
  else
    return SONIC_EXT_PBH_ENCAP_NONE;

  if (proto == IP_PROTOCOL_UDP)
    return (m->present & SONIC_EXT_PBH_Q_L4_DST_PORT) ?
             SONIC_EXT_PBH_ENCAP_VXLAN :
             SONIC_EXT_PBH_ENCAP_NONE;

  if (proto == IP_PROTOCOL_GRE || proto == IP_PROTOCOL_IP_IN_IP ||
      proto == IP_PROTOCOL_IPV6)
    return SONIC_EXT_PBH_ENCAP_IP_GRE;

  return SONIC_EXT_PBH_ENCAP_NONE;
}

static int
sonic_ext_pbh_rule_cmp (void *va, void *vb)
{
  sonic_ext_pbh_rule_t *a = va, *b = vb;

  /* Descending: the node takes the first match and stops. */
  if (a->priority != b->priority)
    return a->priority > b->priority ? -1 : 1;
  /* Priorities are unique in SONiC, but a tiebreak keeps `show` stable. */
  return (int) a->rule_id - (int) b->rule_id;
}

static int
sonic_ext_pbh_rules_validate (sonic_ext_pbh_main_t *pm,
                              sonic_ext_pbh_rule_t *rules)
{
  sonic_ext_pbh_rule_t *r;

  vec_foreach (r, rules)
    {
      if (r->ecmp_profile != ~0 &&
          pool_is_free_index (pm->profiles, r->ecmp_profile))
        return VNET_API_ERROR_NO_SUCH_ENTRY;
      if (r->lag_profile != ~0 &&
          pool_is_free_index (pm->profiles, r->lag_profile))
        return VNET_API_ERROR_NO_SUCH_ENTRY;

      /* A rule with no action is not an error in SAI -- the entry exists and
       * its counter still advances -- but one that needs an inner header and
       * cannot name the encapsulation never would have matched anyway. */
      if ((r->ecmp_profile != ~0 || r->lag_profile != ~0) &&
          r->encap == SONIC_EXT_PBH_ENCAP_NONE)
        return VNET_API_ERROR_INVALID_VALUE;
    }

  return 0;
}

/* SET_LAG_HASH parks its result in the per-buffer side-band, which is only
 * allocated while something needs it. */
static int
sonic_ext_pbh_rules_need_sideband (sonic_ext_pbh_rule_t *rules)
{
  sonic_ext_pbh_rule_t *r;

  vec_foreach (r, rules)
    if (r->lag_profile != ~0)
      return 1;

  return 0;
}

/*
 * Take and release the per-buffer side-band on behalf of a table.
 *
 * The bond TX override reads side-band slots, so it must be live exactly
 * while the table it reads exists -- hence a PBH-level count rather than
 * reusing sonic_ext_vnet_buf_main.refs, which a future non-LAG feature may
 * also hold.  Registration order is deliberate: the table is built before
 * the override that reads it, and cleared before the table is released.
 */
static int
sonic_ext_pbh_sideband_ref (void)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;

  if (sonic_ext_vnet_buf_ref (vlib_get_main ()))
    return 1;

  if (pm->n_sideband_tables++ == 0)
    bond_main.lag_hash_override = sonic_ext_pbh_lag_hash_override;

  return 0;
}

static void
sonic_ext_pbh_sideband_unref (void)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;

  ASSERT (pm->n_sideband_tables > 0);

  if (--pm->n_sideband_tables == 0)
    bond_main.lag_hash_override = 0;

  sonic_ext_vnet_buf_unref (vlib_get_main ());
}

/*
 * Replace is atomic by construction: the new rule vector and its counters
 * are built to one side and swapped in, so a packet in flight sees either
 * the old rule set or the new one and never a partially rebuilt table.
 */
int
sonic_ext_pbh_table_add_replace (u8 *name, sonic_ext_pbh_rule_t *rules,
                                 u32 *table_index)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  sonic_ext_pbh_table_t *t;
  sonic_ext_pbh_rule_t *old_rules;
  int need_sideband, rv;

  if ((rv = sonic_ext_pbh_rules_validate (pm, rules)))
    {
      vec_free (rules);
      vec_free (name);
      return rv;
    }

  vec_sort_with_function (rules, sonic_ext_pbh_rule_cmp);
  need_sideband = sonic_ext_pbh_rules_need_sideband (rules);

  if (*table_index == ~0)
    {
      pool_get_zero (pm->tables, t);
      *table_index = t - pm->tables;
    }
  else
    {
      if (pool_is_free_index (pm->tables, *table_index))
        {
          vec_free (rules);
          vec_free (name);
          return VNET_API_ERROR_NO_SUCH_ENTRY;
        }
      t = pool_elt_at_index (pm->tables, *table_index);
    }

  /* Claim the side-band before installing the rules, and refuse the whole
   * configuration if it cannot be claimed: the node would otherwise index a
   * table that does not exist.  Claiming fails only when something else owns
   * the buffer free callback, e.g. `set buffer traces on`. */
  if (need_sideband && !t->holds_sideband)
    {
      if (sonic_ext_pbh_sideband_ref ())
        {
          if (*table_index != ~0 && vec_len (t->rules) == 0 && t->name == 0)
            pool_put (pm->tables, t);
          vec_free (rules);
          vec_free (name);
          return VNET_API_ERROR_UNSPECIFIED;
        }
      t->holds_sideband = 1;
    }

  if (name)
    {
      vec_free (t->name);
      t->name = name;
    }

  old_rules = t->rules;
  t->rules = rules;
  vec_free (old_rules);

  /* Released only after the rules that needed it are gone. */
  if (!need_sideband && t->holds_sideband)
    {
      sonic_ext_pbh_sideband_unref ();
      t->holds_sideband = 0;
    }

  /* Counters are indexed by rule position, so a replace resets them.  That
   * matches SAI: the counter is an attribute of the entry, and replace
   * destroys and recreates the entries. */
  if (t->counters.name == 0)
    t->counters.name = (char *) format (0, "/sonic-ext/pbh/%u/matches%c",
                                        *table_index, 0);
  vlib_validate_combined_counter (&t->counters,
                                  vec_len (rules) ? vec_len (rules) - 1 : 0);
  vlib_clear_combined_counters (&t->counters);

  return 0;
}

int
sonic_ext_pbh_table_del (u32 table_index)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  sonic_ext_pbh_table_t *t;

  if (pool_is_free_index (pm->tables, table_index))
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  t = pool_elt_at_index (pm->tables, table_index);

  /* Detach first: leaving a dangling attachment would let the node index a
   * freed pool entry.  Drain rather than iterate -- detaching vec_del1's the
   * very vector being walked. */
  while (vec_len (t->sw_if_indices) > 0)
    sonic_ext_pbh_interface_attach_detach (t->sw_if_indices[0], table_index,
                                           0);

  if (t->holds_sideband)
    sonic_ext_pbh_sideband_unref ();

  vec_free (t->sw_if_indices);
  vec_free (t->rules);
  vec_free (t->name);
  vlib_free_combined_counter (&t->counters);
  /* vlib_free_combined_counter() does not own the name we formatted. */
  vec_free (t->counters.name);
  clib_memset (t, 0, sizeof (*t));
  pool_put (pm->tables, t);
  return 0;
}

/*
 * Interface attachment
 */

static void
sonic_ext_pbh_arc_enable_disable (u32 sw_if_index, int enable)
{
  vnet_feature_enable_disable ("ip4-unicast", "sonic-ext-pbh-ip4", sw_if_index,
                               enable, 0, 0);
  vnet_feature_enable_disable ("ip6-unicast", "sonic-ext-pbh-ip6", sw_if_index,
                               enable, 0, 0);
}

/* The BVI of the bridge domain sw_if_index is a member of, or ~0 if it is
 * not bridged, the domain has no BVI, or it is itself the BVI. */
static u32
sonic_ext_pbh_bvi_of (u32 sw_if_index)
{
  l2input_main_t *l2im = &l2input_main;
  l2_input_config_t *config;
  l2_bridge_domain_t *bd;

  if (sw_if_index >= vec_len (l2im->configs))
    return ~0;

  config = vec_elt_at_index (l2im->configs, sw_if_index);
  if (!l2_input_is_bridge (config) || l2_input_is_bvi (config))
    return ~0;

  if (config->bd_index >= vec_len (l2im->bd_configs))
    return ~0;

  bd = vec_elt_at_index (l2im->bd_configs, config->bd_index);
  if (!bd_is_valid (bd))
    return ~0;

  return bd->bvi_sw_if_index;
}

/* Take a reference on the BVI of sw_if_index's bridge domain, attaching the
 * arc there on the first one.  A port that is not bridged records ~0 and
 * costs nothing.
 *
 * Resolution happens once, here.  A port that joins a bridge domain after its
 * table was attached keeps no shadow and the table will not run on bridged
 * traffic until it is rebound; VPP offers no bridge-membership callback to
 * hang a re-resolve off, and SONiC programs VLAN membership before PBH. */
static void
sonic_ext_pbh_bvi_attach (u32 sw_if_index, u32 table_index)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  u32 bvi = sonic_ext_pbh_bvi_of (sw_if_index);

  pm->bvi_by_sw_if_index[sw_if_index] = ~0;

  if (bvi == ~0)
    return;

  vec_validate_init_empty (pm->table_index_by_sw_if_index, bvi, ~0);
  vec_validate (pm->bvi_refcount_by_sw_if_index, bvi);

  if (pm->bvi_refcount_by_sw_if_index[bvi] == 0)
    {
      /* The dataplane narrows the shadow back to the bound ports using the
       * ingress sw_if_index in the buffer cookie, which only exists if the
       * capture node is running.  Capture is a one-way latch and derived
       * from its consumers, so asking for it here is all that is needed. */
      sonic_ext_capture_enable_all ();
      pm->table_index_by_sw_if_index[bvi] = table_index;
      sonic_ext_pbh_arc_enable_disable (bvi, 1);
    }
  else if (pm->table_index_by_sw_if_index[bvi] != table_index)
    {
      /* Two tables bound to members of one bridge domain would both want the
       * BVI, and by ip4-input their traffic is no longer distinguishable.
       * Leave the first owner in place rather than let the second silently
       * displace it, and take no reference we cannot honour. */
      clib_warning ("PBH: %U already shadows table %u for its bridge domain; "
                    "table %u will not take effect on bridged traffic",
                    format_vnet_sw_if_index_name, vnet_get_main (), bvi,
                    pm->table_index_by_sw_if_index[bvi], table_index);
      return;
    }

  pm->bvi_refcount_by_sw_if_index[bvi]++;
  pm->bvi_by_sw_if_index[sw_if_index] = bvi;
}

/* Release the reference taken by sonic_ext_pbh_bvi_attach(), using the BVI
 * recorded then rather than re-deriving it, because the port may have left
 * the bridge domain in between. */
static void
sonic_ext_pbh_bvi_detach (u32 sw_if_index)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  u32 bvi;

  if (sw_if_index >= vec_len (pm->bvi_by_sw_if_index))
    return;

  bvi = pm->bvi_by_sw_if_index[sw_if_index];
  pm->bvi_by_sw_if_index[sw_if_index] = ~0;

  if (bvi == ~0 || bvi >= vec_len (pm->bvi_refcount_by_sw_if_index))
    return;

  /* Zero already means the BVI itself went away and dropped the shadow
   * wholesale; the reference being released no longer exists. */
  if (pm->bvi_refcount_by_sw_if_index[bvi] == 0)
    return;

  if (--pm->bvi_refcount_by_sw_if_index[bvi] == 0)
    {
      pm->table_index_by_sw_if_index[bvi] = ~0;
      sonic_ext_pbh_arc_enable_disable (bvi, 0);
    }
}

int
sonic_ext_pbh_interface_attach_detach (u32 sw_if_index, u32 table_index,
                                       int is_attach)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  sonic_ext_main_t *sem = &sonic_ext_main;
  sonic_ext_pbh_table_t *t;
  u32 i;

  if (!sem->pbh)
    return VNET_API_ERROR_FEATURE_DISABLED;
  if (pool_is_free_index (pm->tables, table_index))
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  t = pool_elt_at_index (pm->tables, table_index);
  vec_validate_init_empty (pm->table_index_by_sw_if_index, sw_if_index, ~0);
  vec_validate_init_empty (pm->bvi_by_sw_if_index, sw_if_index, ~0);

  if (is_attach)
    {
      /* SAI_PORT_ATTR_PBH_* is a single OID, so an interface has at most one
       * table; rebinding replaces rather than stacking. */
      if (pm->table_index_by_sw_if_index[sw_if_index] == table_index)
        return 0;
      if (pm->table_index_by_sw_if_index[sw_if_index] != ~0)
        sonic_ext_pbh_interface_attach_detach (
          sw_if_index, pm->table_index_by_sw_if_index[sw_if_index], 0);

      pm->table_index_by_sw_if_index[sw_if_index] = table_index;
      vec_add1 (t->sw_if_indices, sw_if_index);
      pm->n_attachments++;
      sonic_ext_pbh_arc_enable_disable (sw_if_index, 1);
      sonic_ext_pbh_bvi_attach (sw_if_index, table_index);
      return 0;
    }

  if (pm->table_index_by_sw_if_index[sw_if_index] != table_index)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  pm->table_index_by_sw_if_index[sw_if_index] = ~0;
  for (i = 0; i < vec_len (t->sw_if_indices); i++)
    if (t->sw_if_indices[i] == sw_if_index)
      {
        vec_del1 (t->sw_if_indices, i);
        break;
      }
  pm->n_attachments--;
  sonic_ext_pbh_arc_enable_disable (sw_if_index, 0);
  sonic_ext_pbh_bvi_detach (sw_if_index);
  return 0;
}

/* An interface going away takes its attachment with it; otherwise the index
 * would be reused by the next interface and silently inherit the table. */
static clib_error_t *
sonic_ext_pbh_sw_interface_add_del (vnet_main_t *vnm, u32 sw_if_index,
                                    u32 is_add)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  u32 table_index;

  if (is_add || sw_if_index >= vec_len (pm->table_index_by_sw_if_index))
    return 0;

  /* A shadow attachment is owned by the member ports that pulled it in, not
   * by the BVI, so it is not a normal attachment to unwind: there is no entry
   * in t->sw_if_indices and n_attachments never counted it.  Drop it whole;
   * the members release into a zero refcount, which reads as already gone. */
  if (sw_if_index < vec_len (pm->bvi_refcount_by_sw_if_index) &&
      pm->bvi_refcount_by_sw_if_index[sw_if_index] != 0)
    {
      pm->bvi_refcount_by_sw_if_index[sw_if_index] = 0;
      pm->table_index_by_sw_if_index[sw_if_index] = ~0;
      return 0;
    }

  table_index = pm->table_index_by_sw_if_index[sw_if_index];
  if (table_index != ~0)
    sonic_ext_pbh_interface_attach_detach (sw_if_index, table_index, 0);

  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (sonic_ext_pbh_sw_interface_add_del);

/*
 * Formatting
 */

static const char *sonic_ext_pbh_hash_field_names[] = {
#define _(sym, str) str,
  foreach_sonic_ext_pbh_hash_field
#undef _
};

u8 *
format_sonic_ext_pbh_profile (u8 *s, va_list *args)
{
  sonic_ext_pbh_profile_t *p = va_arg (*args, sonic_ext_pbh_profile_t *);
  sonic_ext_pbh_hash_field_t *f;

  vec_foreach (f, p->fields)
    {
      s = format (s, "%s[seq %u", f != p->fields ? " " : "", f->seq);
      switch (f->field)
        {
        case SONIC_EXT_PBH_HF_INNER_SRC_IPV4:
        case SONIC_EXT_PBH_HF_INNER_DST_IPV4:
          s = format (s, " mask %U", format_ip4_address, &f->mask.ip4);
          break;
        case SONIC_EXT_PBH_HF_INNER_SRC_IPV6:
        case SONIC_EXT_PBH_HF_INNER_DST_IPV6:
          s = format (s, " mask %U", format_ip6_address, &f->mask.ip6);
          break;
        default:
          break;
        }
      s = format (s, "] %s", sonic_ext_pbh_hash_field_names[f->field]);
    }

  return s;
}

u8 *
format_sonic_ext_pbh_rule (u8 *s, va_list *args)
{
  sonic_ext_pbh_rule_t *r = va_arg (*args, sonic_ext_pbh_rule_t *);
  sonic_ext_pbh_match_t *m = &r->match;

  s = format (s, "rule %u prio %u match", r->rule_id, r->priority);

  if (m->present & SONIC_EXT_PBH_Q_ETHER_TYPE)
    s = format (s, " ether-type 0x%04x", m->ether_type);
  if (m->present & SONIC_EXT_PBH_Q_IP_PROTOCOL)
    s = format (s, " ip-protocol 0x%02x", m->ip_protocol);
  if (m->present & SONIC_EXT_PBH_Q_IPV6_NEXT_HEADER)
    s = format (s, " ipv6-next-header 0x%02x", m->ipv6_next_header);
  if (m->present & SONIC_EXT_PBH_Q_L4_DST_PORT)
    s = format (s, " l4-dst-port %u", m->l4_dst_port);
  if (m->present & SONIC_EXT_PBH_Q_GRE_KEY)
    s = format (s, " gre-key 0x%08x/0x%08x", m->gre_key, m->gre_key_mask);
  if (m->present & SONIC_EXT_PBH_Q_INNER_ETHER_TYPE)
    s = format (s, " inner-ether-type 0x%04x", m->inner_ether_type);
  if (m->present == 0)
    s = format (s, " any");

  if (r->ecmp_profile != ~0)
    s = format (s, " set-ecmp-hash %u", r->ecmp_profile);
  if (r->lag_profile != ~0)
    s = format (s, " set-lag-hash %u", r->lag_profile);
  if (r->ecmp_profile == ~0 && r->lag_profile == ~0)
    s = format (s, " no-action");

  return s;
}

static clib_error_t *
sonic_ext_pbh_init (vlib_main_t *vm)
{
  clib_memset (&sonic_ext_pbh_main, 0, sizeof (sonic_ext_pbh_main));
  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_pbh_init);
