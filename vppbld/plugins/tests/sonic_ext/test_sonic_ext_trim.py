#!/usr/bin/env python3
"""sonic-ext packet-trim feature-registry (operator disable) tests

Packet trimming is registered in the plugin's generic per-feature registry
(foreach_sonic_ext_feature in sonic_ext.h), so an operator can force it off
with "sonic-ext { trim disable }" in startup.conf exactly like every other
sonic-ext feature. The admission node self-gates on this toggle, so these
tests pin down the two things the gate depends on:

  * trim is a *known* feature and defaults enabled (an unregistered feature
    reads back disabled, which would silently keep the admission node bypassed
    even once SAI pushes a policy), and
  * the startup stanza actually clears the toggle the node reads.

End-to-end truncation behavior (a packet is shortened only when trimming is
both enabled and SAI-configured) is covered by the sonic-mgmt packet_trimming
suite; here we only exercise the VPP-level feature toggle, which SAI does not
reach.
"""

import unittest

from framework import VppTestCase
from asfframework import VppTestRunner


class SonicExtTrimBase(VppTestCase):
    """sonic-ext packet-trim feature-toggle test base"""

    # sonic-ext's nodes name linux-cp's, so sonic-ext cannot load without it;
    # both plugins are default-disabled and must be enabled explicitly.
    extra_vpp_plugin_config = [
        "plugin",
        "linux_cp_plugin.so",
        "{",
        "enable",
        "}",
        "plugin",
        "sonic_ext_plugin.so",
        "{",
        "enable",
        "}",
    ]

    def trim_feature_enabled(self):
        return bool(self.vapi.sonic_ext_feature_get(feature="trim").enabled)


class TestSonicExtTrim(SonicExtTrimBase):
    """sonic-ext packet-trim feature registry"""

    def test_trim_registered_default_on(self):
        """trim is a known sonic-ext feature and defaults enabled

        Without the foreach_sonic_ext_feature registration, feature_get would
        return the unknown-feature default (disabled). Since the admission node
        gates every packet on this toggle, it must be registered and default on
        so SAI-driven trimming still activates with no startup stanza.
        """
        self.assertTrue(self.trim_feature_enabled())


class TestSonicExtTrimStartupOff(SonicExtTrimBase):
    """sonic-ext trim off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "trim", "off", "}"]

    def test_trim_startup_off(self):
        """trim off at startup clears the toggle the admission node reads"""
        self.assertFalse(self.trim_feature_enabled())


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
