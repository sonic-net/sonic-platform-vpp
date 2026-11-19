#!/usr/bin/env python3
"""sonic-ext MACsec SecY tests (GCM-AES-128/256 and XPN).

L2 xconnect pg0 <-> pg1.  Egress SecY on pg1 encrypts frames that
arrive on pg0; ingress SecY on pg0 decrypts frames injected there.
EAPOL (0x888E) must pass a protected port unmodified. LACP (0x8809)
is on the controlled port and is MACsec-encapsulated.
"""

import re
import struct
import unittest

from asfframework import VppTestRunner
from framework import VppTestCase
from scapy.compat import raw
from scapy.layers.inet import IP, UDP
from scapy.layers.l2 import Ether
from scapy.packet import Raw

# IEEE 802.1AE-2018 Annex published GCM-AES-128 test SAK (not a secret).
TEST_SAK = bytes.fromhex("ad7a2bd03eac835a6f620fdcb506b345")
# IEEE 802.1AEbn-2011 2.2.2 published GCM-AES-256 test SAK (not a secret).
TEST_SAK_256 = bytes.fromhex(
    "e3c08a8f06c6e3ad95a70557b23f75483ce33021a9c72b7025666204c69c0b72"
)
TEST_SCI = 0x12153524C0895E81
# Distinct test salt/SSCI for XPN IV; not a credential.
TEST_SALT = bytes.fromhex("000102030405060708090a0b")
TEST_SSCI = 0x01000000
MACSEC_ETHERTYPE = 0x88E5
EAPOL_ETHERTYPE = 0x888E
LACP_ETHERTYPE = 0x8809
CS_GCM_AES_128 = 0
CS_GCM_AES_256 = 1
CS_GCM_AES_XPN_128 = 2
CS_GCM_AES_XPN_256 = 3
DIR_EGRESS = 0
DIR_INGRESS = 1
# Explicit-SCI SecTAG (16) + ICV (16).
MACSEC_OVERHEAD_SCI = 32


def _aes_gcm_encrypt(key, iv, aad, plaintext):
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM

        return AESGCM(key).encrypt(iv, plaintext, aad)
    except ImportError:
        from Crypto.Cipher import AES

        cipher = AES.new(key, AES.MODE_GCM, nonce=iv)
        cipher.update(aad)
        ciphertext, tag = cipher.encrypt_and_digest(plaintext)
        return ciphertext + tag


def _gcm_available():
    try:
        _aes_gcm_encrypt(TEST_SAK, b"\x00" * 12, b"a" * 28, b"hello")
        return True
    except Exception:
        return False


HAVE_GCM = _gcm_available()


def xpn_iv(salt, ssci, xpn):
    """IEEE 802.1AEbw IV = salt XOR (SSCI_be32 || XPN_be64)."""
    nonce = struct.pack("!IQ", ssci, xpn)
    return bytes(a ^ b for a, b in zip(salt, nonce))


def macsec_protect(
    frame,
    sak,
    sci,
    an=0,
    pn=1,
    send_sci=True,
    salt=None,
    ssci=0,
    xpn_en=False,
):
    """Build a confidentiality (E=1,C=1) MACsec frame matching the plugin."""
    da, sa, rest = frame[:6], frame[6:12], frame[12:]
    tci = (0x20 if send_sci else 0) | 0x0C | (an & 3)
    sl = len(rest) if len(rest) < 48 else 0
    sectag = struct.pack("!BBBBI", 0x88, 0xE5, tci, sl, pn & 0xFFFFFFFF)
    if send_sci:
        sectag += struct.pack("!Q", sci)
    if xpn_en:
        iv = xpn_iv(salt, ssci, pn)
    else:
        iv = struct.pack("!QI", sci, pn & 0xFFFFFFFF)
    aad = da + sa + sectag
    return da + sa + sectag + _aes_gcm_encrypt(sak, iv, aad, rest)


class SonicExtMacsecBase(VppTestCase):
    """sonic-ext MACsec test base"""

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

    @classmethod
    def setUpClass(cls):
        super(SonicExtMacsecBase, cls).setUpClass()
        cls.create_pg_interfaces(range(2))

    def setUp(self):
        super(SonicExtMacsecBase, self).setUp()
        for interface in self.pg_interfaces:
            interface.admin_up()
        self.vapi.sw_interface_set_l2_xconnect(
            rx_sw_if_index=self.pg0.sw_if_index,
            tx_sw_if_index=self.pg1.sw_if_index,
            enable=True,
        )
        self.vapi.sw_interface_set_l2_xconnect(
            rx_sw_if_index=self.pg1.sw_if_index,
            tx_sw_if_index=self.pg0.sw_if_index,
            enable=True,
        )
        self._secy = []

    def tearDown(self):
        for args in reversed(self._secy):
            try:
                self.vapi.sonic_ext_macsec_port_add_del(**args, is_add=False)
            except Exception:
                pass
        self._secy = []
        super(SonicExtMacsecBase, self).tearDown()
        for interface in self.pg_interfaces:
            interface.admin_down()

    def sak32(self, sak=None):
        if sak is None:
            sak = TEST_SAK
        return (sak + bytes(32))[:32]

    def add_port_and_sc(
        self,
        sw_if_index,
        direction,
        send_sci=1,
        sci=TEST_SCI,
        cipher_suite=CS_GCM_AES_128,
        replay_protect=0,
        replay_window=0,
    ):
        self.vapi.sonic_ext_macsec_port_add_del(
            sw_if_index=sw_if_index,
            direction=direction,
            encrypt=1,
            send_sci=send_sci,
            replay_protect=replay_protect,
            replay_window=replay_window,
            is_add=True,
        )
        self._secy.append(
            dict(
                sw_if_index=sw_if_index,
                direction=direction,
                encrypt=1,
                send_sci=send_sci,
                replay_protect=replay_protect,
                replay_window=replay_window,
            )
        )
        self.vapi.sonic_ext_macsec_sc_add_del(
            sw_if_index=sw_if_index,
            direction=direction,
            sci=sci,
            cipher_suite=cipher_suite,
            explicit_sci=send_sci,
            confidentiality_offset=0,
            is_add=True,
        )

    def add_sa(
        self,
        sw_if_index,
        direction,
        an=0,
        sci=TEST_SCI,
        sak=None,
        sak_len=16,
        salt=None,
        ssci=0,
        xpn_en=0,
        init_xpn=1,
        set_encoding=True,
    ):
        if salt is None:
            salt = bytes(12)
        r = self.vapi.sonic_ext_macsec_sa_add_del(
            sw_if_index=sw_if_index,
            direction=direction,
            sci=sci,
            an=an,
            sak=self.sak32(sak),
            sak_len=sak_len,
            salt=salt,
            ssci=ssci,
            xpn_en=xpn_en,
            init_xpn=init_xpn,
            is_add=True,
        )
        if set_encoding:
            self.vapi.sonic_ext_macsec_sc_set_encoding_an(
                sw_if_index=sw_if_index, sci=sci, an=an
            )
        return getattr(r, "sa_index", None)

    def del_sa(self, sw_if_index, direction, an=0, sci=TEST_SCI):
        self.vapi.sonic_ext_macsec_sa_add_del(
            sw_if_index=sw_if_index,
            direction=direction,
            sci=sci,
            an=an,
            sak=bytes(32),
            sak_len=0,
            salt=bytes(12),
            ssci=0,
            xpn_en=0,
            init_xpn=0,
            is_add=False,
        )

    def add_secy(
        self,
        sw_if_index,
        direction,
        send_sci=1,
        an=0,
        sci=TEST_SCI,
        cipher_suite=CS_GCM_AES_128,
        sak=None,
        sak_len=16,
        salt=None,
        ssci=0,
        xpn_en=0,
        init_xpn=1,
        replay_protect=0,
        replay_window=0,
    ):
        self.add_port_and_sc(
            sw_if_index,
            direction,
            send_sci=send_sci,
            sci=sci,
            cipher_suite=cipher_suite,
            replay_protect=replay_protect,
            replay_window=replay_window,
        )
        self.add_sa(
            sw_if_index,
            direction,
            an=an,
            sci=sci,
            sak=sak,
            sak_len=sak_len,
            salt=salt,
            ssci=ssci,
            xpn_en=xpn_en,
            init_xpn=init_xpn,
        )

    def data_pkt(self):
        return (
            Ether(dst=self.pg1.local_mac, src=self.pg0.remote_mac)
            / IP(src="10.0.0.1", dst="10.0.0.2", ttl=64)
            / UDP(sport=1234, dport=4321)
            / Raw(b"macsec-payload")
        )

    def ctrl_pkt(self, etype):
        dst = "01:80:c2:00:00:03" if etype == EAPOL_ETHERTYPE else "01:80:c2:00:00:02"
        return Ether(dst=dst, src=self.pg0.remote_mac, type=etype) / Raw(
            b"\x01\x01" + b"\x00" * 40
        )

    def wire_pkt(self, frame):
        """Rebuild a captured/constructed frame without scapy trying to parse 0x88E5."""
        return Ether(dst=frame[:6], src=frame[6:12], type=int.from_bytes(frame[12:14], "big")) / Raw(
            frame[14:]
        )


class TestSonicExtMacsec(SonicExtMacsecBase):
    """sonic-ext MACsec GCM-AES-128"""

    def test_feature_default_enabled(self):
        """macsec is a SAIVPP-owned feature, on by default"""
        self.assertTrue(self.vapi.sonic_ext_feature_get(feature="macsec").enabled)

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library for the IEEE known-answer vector")
    def test_encrypt_known_answer(self):
        """plaintext on pg0 is GCM-AES-128 encapsulated on pg1"""
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS)
        pkt = self.data_pkt()
        expected = macsec_protect(raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=1, send_sci=True)
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        got = raw(rx)
        self.assertEqual(int.from_bytes(got[12:14], "big"), MACSEC_ETHERTYPE)
        self.assertEqual(got[: len(expected)], expected)

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build the decrypt input")
    def test_decrypt_round_trip(self):
        """ciphertext on pg0 decrypts to the original Ethernet frame on pg1"""
        pkt = self.data_pkt()
        protected = macsec_protect(raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=1, send_sci=True)
        self.add_secy(self.pg0.sw_if_index, DIR_INGRESS)
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(protected)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build a corrupted ICV frame")
    def test_bad_icv_dropped(self):
        """corrupted ICV is dropped and in-pkts-not-valid increments"""
        pkt = self.data_pkt()
        protected = bytearray(
            macsec_protect(raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=1, send_sci=True)
        )
        protected[-1] ^= 0xFF
        self.add_secy(self.pg0.sw_if_index, DIR_INGRESS)
        before = self.statistics.get_err_counter(
            "/err/sonic-ext-macsec-decrypt/dropped: ICV check failed"
        )
        self.send_and_assert_no_replies(self.pg0, [self.wire_pkt(bytes(protected))])
        self.assertEqual(
            self.statistics.get_err_counter(
                "/err/sonic-ext-macsec-decrypt/dropped: ICV check failed"
            ),
            before + 1,
        )
        sa_out = self.vapi.cli("show macsec sa")
        self.assertRegex(sa_out, r"in-not-valid 1")

    def test_eapol_bypass(self):
        """EAPOL on a protected port is unmodified"""
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS)
        self.add_secy(self.pg0.sw_if_index, DIR_INGRESS)
        pkt = self.ctrl_pkt(EAPOL_ETHERTYPE)
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))
        self.assertEqual(int.from_bytes(raw(rx)[12:14], "big"), EAPOL_ETHERTYPE)

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library for the LACP protect vector")
    def test_lacp_is_protected(self):
        """LACP is on the controlled port and is MACsec-encapsulated"""
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS)
        pkt = self.ctrl_pkt(LACP_ETHERTYPE)
        expected = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=1, send_sci=True
        )
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        got = raw(rx)
        self.assertEqual(int.from_bytes(got[12:14], "big"), MACSEC_ETHERTYPE)
        self.assertEqual(got[: len(expected)], expected)

    def test_shared_chassis_sci(self):
        """two ports may share one egress SCI (SONiC chassis SCI)"""
        chassis_sci = 0x0100A919D5D05922
        self.add_secy(self.pg0.sw_if_index, DIR_EGRESS, sci=chassis_sci)
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS, sci=chassis_sci)
        out = self.vapi.cli("show macsec")
        self.assertEqual(out.count("sc 0x0100a919d5d05922"), 2)
        sa_out = self.vapi.cli("show macsec sa")
        self.assertEqual(sa_out.count("sak-len 16"), 2)

    def test_sa_after_all_scs_created(self):
        """SA add still finds the SC after the SC pool has grown.

        orchagent creates every SC first, then every SA. That used to
        return VNET_API_ERROR_NO_SUCH_ENTRY (-6) because the SCI hash
        stored pointers into the SC pool, which moves on pool_get().
        """
        chassis_sci = 0x0100A919D5D05922
        peer0 = 0x12153524C0895E81
        peer1 = 0x12153524C0895E82
        self.add_port_and_sc(self.pg0.sw_if_index, DIR_EGRESS, sci=chassis_sci)
        self.add_port_and_sc(self.pg0.sw_if_index, DIR_INGRESS, sci=peer0)
        self.add_port_and_sc(self.pg1.sw_if_index, DIR_EGRESS, sci=chassis_sci)
        self.add_port_and_sc(self.pg1.sw_if_index, DIR_INGRESS, sci=peer1)
        self.add_sa(self.pg0.sw_if_index, DIR_EGRESS, sci=chassis_sci)
        self.add_sa(self.pg0.sw_if_index, DIR_INGRESS, sci=peer0)
        self.add_sa(self.pg1.sw_if_index, DIR_EGRESS, sci=chassis_sci)
        self.add_sa(self.pg1.sw_if_index, DIR_INGRESS, sci=peer1)
        sa_out = self.vapi.cli("show macsec sa")
        self.assertEqual(sa_out.count("sak-len 16"), 4)

    def test_show_macsec_hides_sak(self):
        """show macsec sa prints key length, never the SAK"""
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS)
        out = self.vapi.cli("show macsec sa")
        self.assertIn("sak-len 16", out)
        self.assertIn("gcm-aes-128", out)
        self.assertNotIn(TEST_SAK.hex(), out.lower())
        self.assertNotIn(TEST_SAK.hex().upper(), out)
        self.assertRegex(self.vapi.cli("show macsec"), r"macsec:\s+on")

    def test_accepts_aes256_and_xpn(self):
        """AES-256 and XPN SCs/SAs install; cipher vs sak_len/xpn_en must match"""
        self.add_port_and_sc(
            self.pg1.sw_if_index, DIR_EGRESS, cipher_suite=CS_GCM_AES_256
        )
        self.add_sa(
            self.pg1.sw_if_index,
            DIR_EGRESS,
            sak=TEST_SAK_256,
            sak_len=32,
        )
        out = self.vapi.cli("show macsec sa")
        self.assertIn("sak-len 32", out)
        self.assertIn("gcm-aes-256", out)
        self.assertNotIn(TEST_SAK_256.hex(), out.lower())

        self.add_port_and_sc(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            cipher_suite=CS_GCM_AES_XPN_256,
        )
        self.add_sa(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            sak=TEST_SAK_256,
            sak_len=32,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=1,
        )
        out = self.vapi.cli("show macsec sa")
        self.assertIn("gcm-aes-xpn-256", out)
        self.assertIn("xpn-en 1", out)
        self.assertNotIn(TEST_SALT.hex(), out.lower())

        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_macsec_sc_add_del(
                sw_if_index=self.pg1.sw_if_index,
                direction=DIR_EGRESS,
                sci=TEST_SCI + 1,
                cipher_suite=4,
                explicit_sci=1,
                confidentiality_offset=0,
                is_add=True,
            )

    def test_rejects_cipher_sak_mismatch(self):
        """sak_len and xpn_en must match the SC cipher suite"""
        self.add_port_and_sc(self.pg1.sw_if_index, DIR_EGRESS)
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_macsec_sa_add_del(
                sw_if_index=self.pg1.sw_if_index,
                direction=DIR_EGRESS,
                sci=TEST_SCI,
                an=0,
                sak=self.sak32(),
                sak_len=32,
                salt=bytes(12),
                ssci=0,
                xpn_en=0,
                init_xpn=1,
                is_add=True,
            )
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_macsec_sa_add_del(
                sw_if_index=self.pg1.sw_if_index,
                direction=DIR_EGRESS,
                sci=TEST_SCI,
                an=0,
                sak=self.sak32(),
                sak_len=16,
                salt=bytes(12),
                ssci=0,
                xpn_en=1,
                init_xpn=1,
                is_add=True,
            )

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library for the AES-256 vector")
    def test_encrypt_aes256_known_answer(self):
        """plaintext on pg0 is GCM-AES-256 encapsulated on pg1"""
        self.add_secy(
            self.pg1.sw_if_index,
            DIR_EGRESS,
            cipher_suite=CS_GCM_AES_256,
            sak=TEST_SAK_256,
            sak_len=32,
        )
        pkt = self.data_pkt()
        expected = macsec_protect(
            raw(pkt), TEST_SAK_256, TEST_SCI, an=0, pn=1, send_sci=True
        )
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        got = raw(rx)
        self.assertEqual(int.from_bytes(got[12:14], "big"), MACSEC_ETHERTYPE)
        self.assertEqual(got[: len(expected)], expected)

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library for the XPN-256 vector")
    def test_encrypt_xpn256_known_answer(self):
        """XPN-256 IV is salt XOR (SSCI || XPN)"""
        self.add_secy(
            self.pg1.sw_if_index,
            DIR_EGRESS,
            cipher_suite=CS_GCM_AES_XPN_256,
            sak=TEST_SAK_256,
            sak_len=32,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=1,
        )
        pkt = self.data_pkt()
        expected = macsec_protect(
            raw(pkt),
            TEST_SAK_256,
            TEST_SCI,
            an=0,
            pn=1,
            send_sci=True,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=True,
        )
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        got = raw(rx)
        self.assertEqual(int.from_bytes(got[12:14], "big"), MACSEC_ETHERTYPE)
        self.assertEqual(got[: len(expected)], expected)

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build the XPN decrypt input")
    def test_decrypt_xpn256_round_trip(self):
        """XPN-256 ciphertext on pg0 decrypts to the original Ethernet frame"""
        pkt = self.data_pkt()
        protected = macsec_protect(
            raw(pkt),
            TEST_SAK_256,
            TEST_SCI,
            an=0,
            pn=1,
            send_sci=True,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=True,
        )
        self.add_secy(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            cipher_suite=CS_GCM_AES_XPN_256,
            sak=TEST_SAK_256,
            sak_len=32,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=1,
        )
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(protected)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build XPN wrap frames")
    def test_xpn_wrap_recovery(self):
        """ingress reconstructs the upper 32 bits after the 32-bit PN wraps"""
        pkt = self.data_pkt()
        high_pn = 0xFFFFFF00
        wrap_pn = 0x100000010
        self.add_secy(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            cipher_suite=CS_GCM_AES_XPN_256,
            sak=TEST_SAK_256,
            sak_len=32,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=1,
            init_xpn=high_pn,
        )
        first = macsec_protect(
            raw(pkt),
            TEST_SAK_256,
            TEST_SCI,
            an=0,
            pn=high_pn,
            send_sci=True,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=True,
        )
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(first)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))

        wrapped = macsec_protect(
            raw(pkt),
            TEST_SAK_256,
            TEST_SCI,
            an=0,
            pn=wrap_pn,
            send_sci=True,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=True,
        )
        self.assertEqual(struct.unpack("!I", wrapped[16:20])[0], wrap_pn & 0xFFFFFFFF)
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(wrapped)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build replay frames")
    def test_replay_duplicate_dropped(self):
        """duplicate PN is dropped as late when replay protection is on"""
        pkt = self.data_pkt()
        protected = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=1, send_sci=True
        )
        self.add_secy(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            replay_protect=1,
            replay_window=0,
        )
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(protected)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))
        before = self.statistics.get_err_counter(
            "/err/sonic-ext-macsec-decrypt/dropped: replay late"
        )
        self.send_and_assert_no_replies(self.pg0, [self.wire_pkt(protected)])
        self.assertEqual(
            self.statistics.get_err_counter(
                "/err/sonic-ext-macsec-decrypt/dropped: replay late"
            ),
            before + 1,
        )
        sa_out = self.vapi.cli("show macsec sa")
        self.assertRegex(sa_out, r"in-late 1")

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build replay frames")
    def test_replay_forward_jump_and_window(self):
        """large forward PN jumps are accepted; out-of-window PNs are late"""
        pkt = self.data_pkt()
        self.add_secy(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            replay_protect=1,
            replay_window=4,
        )

        def send_pn(pn):
            protected = macsec_protect(
                raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=pn, send_sci=True
            )
            rx = self.send_and_expect(self.pg0, [self.wire_pkt(protected)], self.pg1)[0]
            self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))

        send_pn(1)
        send_pn(1000)
        send_pn(998)
        late = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=990, send_sci=True
        )
        before = self.statistics.get_err_counter(
            "/err/sonic-ext-macsec-decrypt/dropped: replay late"
        )
        self.send_and_assert_no_replies(self.pg0, [self.wire_pkt(late)])
        self.assertEqual(
            self.statistics.get_err_counter(
                "/err/sonic-ext-macsec-decrypt/dropped: replay late"
            ),
            before + 1,
        )

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build replay frames")
    def test_replay_new_an_pn_restart(self):
        """new AN has its own PN space; AN=1 PN=1 is not late after AN=0"""
        pkt = self.data_pkt()
        self.add_secy(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            replay_protect=1,
            replay_window=0,
        )
        first = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=5, send_sci=True
        )
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(first)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))

        self.add_sa(
            self.pg0.sw_if_index, DIR_INGRESS, an=1, set_encoding=False
        )
        before = self.statistics.get_err_counter(
            "/err/sonic-ext-macsec-decrypt/dropped: replay late"
        )
        nxt = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=1, pn=1, send_sci=True
        )
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(nxt)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))
        self.assertEqual(
            self.statistics.get_err_counter(
                "/err/sonic-ext-macsec-decrypt/dropped: replay late"
            ),
            before,
        )
        self.send_and_assert_no_replies(self.pg0, [self.wire_pkt(nxt)])
        self.assertEqual(
            self.statistics.get_err_counter(
                "/err/sonic-ext-macsec-decrypt/dropped: replay late"
            ),
            before + 1,
        )
        sa_out = self.vapi.cli("show macsec sa")
        self.assertRegex(sa_out, r"in-ok 1")
        self.assertRegex(sa_out, r"in-late 1")

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build XPN replay frames")
    def test_replay_new_an_xpn_pn_restart(self):
        """XPN recover/replay are per-SA so AN=1 PN=1 is not late after AN=0"""
        pkt = self.data_pkt()
        self.add_secy(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            cipher_suite=CS_GCM_AES_XPN_256,
            sak=TEST_SAK_256,
            sak_len=32,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=1,
            replay_protect=1,
            replay_window=0,
        )
        first = macsec_protect(
            raw(pkt),
            TEST_SAK_256,
            TEST_SCI,
            an=0,
            pn=1000,
            send_sci=True,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=True,
        )
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(first)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))

        self.add_sa(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            an=1,
            sak=TEST_SAK_256,
            sak_len=32,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=1,
            set_encoding=False,
        )
        before = self.statistics.get_err_counter(
            "/err/sonic-ext-macsec-decrypt/dropped: replay late"
        )
        nxt = macsec_protect(
            raw(pkt),
            TEST_SAK_256,
            TEST_SCI,
            an=1,
            pn=1,
            send_sci=True,
            salt=TEST_SALT,
            ssci=TEST_SSCI,
            xpn_en=True,
        )
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(nxt)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))
        self.assertEqual(
            self.statistics.get_err_counter(
                "/err/sonic-ext-macsec-decrypt/dropped: replay late"
            ),
            before,
        )

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library for the rekey vector")
    def test_rekey_encoding_an(self):
        """AN flip under traffic uses the new SA; deleting the old SA is lossless"""
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS, an=0)
        pkt = self.data_pkt()
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        self.assertEqual(raw(rx)[14] & 0x03, 0)

        self.add_sa(self.pg1.sw_if_index, DIR_EGRESS, an=1)
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        self.assertEqual(raw(rx)[14] & 0x03, 1)
        expected = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=1, pn=1, send_sci=True
        )
        self.assertEqual(raw(rx)[: len(expected)], expected)

        self.del_sa(self.pg1.sw_if_index, DIR_EGRESS, an=0)
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        self.assertEqual(raw(rx)[14] & 0x03, 1)
        expected = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=1, pn=2, send_sci=True
        )
        self.assertEqual(raw(rx)[: len(expected)], expected)

    def _sa_cli_field(self, name):
        out = self.vapi.cli("show macsec sa")
        vals = [int(v) for v in re.findall(r"%s (\d+)" % name, out)]
        self.assertTrue(vals, out)
        return vals

    def jumbo_pkt(self):
        return (
            Ether(dst=self.pg1.local_mac, src=self.pg0.remote_mac)
            / IP(src="10.0.0.1", dst="10.0.0.2", ttl=64)
            / UDP(sport=1234, dport=4321)
            / Raw(b"J" * 4000)
        )

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library for the counter datapath")
    def test_egress_counters_and_xpn(self):
        """out-enc / XPN / User Data octets advance after N encrypted frames"""
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS)
        pkt = self.data_pkt()
        user_len = len(raw(pkt)) - 12
        n = 5
        for _ in range(n):
            rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
            self.assertEqual(int.from_bytes(raw(rx)[12:14], "big"), MACSEC_ETHERTYPE)
        out = self.vapi.cli("show macsec sa")
        self.assertRegex(out, r"out-enc %u" % n)
        self.assertRegex(out, r"octets-enc %u" % (n * user_len))
        self.assertEqual(self._sa_cli_field("pn"), [1 + n])

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library for the counter datapath")
    def test_ingress_counters_and_xpn(self):
        """in-ok / XPN / User Data octets advance after N decrypted frames"""
        pkt = self.data_pkt()
        user_len = len(raw(pkt)) - 12
        n = 5
        self.add_secy(self.pg0.sw_if_index, DIR_INGRESS)
        for i in range(n):
            protected = macsec_protect(
                raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=i + 1, send_sci=True
            )
            rx = self.send_and_expect(self.pg0, [self.wire_pkt(protected)], self.pg1)[0]
            self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))
        out = self.vapi.cli("show macsec sa")
        self.assertRegex(out, r"in-ok %u" % n)
        self.assertRegex(out, r"in-octets-dec %u" % (n * user_len))
        self.assertEqual(self._sa_cli_field("pn"), [1 + n])

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library for the counter datapath")
    def test_clear_stats_leaves_pn(self):
        """clear macsec counters zeros STAT_* and leaves PN"""
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS)
        pkt = self.data_pkt()
        self.send_and_expect(self.pg0, [pkt], self.pg1)
        before = self.vapi.cli("show macsec sa")
        self.assertRegex(before, r"out-enc 1")
        self.assertRegex(before, r"pn 2")
        self.vapi.cli("clear macsec counters")
        after = self.vapi.cli("show macsec sa")
        self.assertRegex(after, r"out-enc 0")
        self.assertRegex(after, r"octets-enc 0")
        self.assertRegex(after, r"pn 2")

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build replay frames")
    def test_replay_late_bumps_in_pkts_late(self):
        """replay duplicate still bumps in-pkts-late after the counter move"""
        pkt = self.data_pkt()
        protected = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=1, send_sci=True
        )
        self.add_secy(
            self.pg0.sw_if_index,
            DIR_INGRESS,
            replay_protect=1,
            replay_window=0,
        )
        self.send_and_expect(self.pg0, [self.wire_pkt(protected)], self.pg1)
        self.send_and_assert_no_replies(self.pg0, [self.wire_pkt(protected)])
        sa_out = self.vapi.cli("show macsec sa")
        self.assertRegex(sa_out, r"in-late 1")
        self.assertRegex(sa_out, r"in-ok 1")

    def test_encrypt_jumbo_chained(self):
        """payload larger than one vlib_buffer is forwarded, not CHAINED-dropped"""
        self.add_secy(self.pg1.sw_if_index, DIR_EGRESS)
        pkt = self.jumbo_pkt()
        before = self.statistics.get_err_counter(
            "/err/sonic-ext-macsec-encrypt/dropped: chained buffer"
        )
        rx = self.send_and_expect(self.pg0, [pkt], self.pg1)[0]
        got = raw(rx)
        self.assertEqual(int.from_bytes(got[12:14], "big"), MACSEC_ETHERTYPE)
        self.assertGreaterEqual(len(got), len(raw(pkt)) + MACSEC_OVERHEAD_SCI)
        self.assertEqual(
            self.statistics.get_err_counter(
                "/err/sonic-ext-macsec-encrypt/dropped: chained buffer"
            ),
            before,
        )
        self.assertRegex(self.vapi.cli("show macsec sa"), r"out-enc 1")

    @unittest.skipUnless(HAVE_GCM, "no AES-GCM library to build a jumbo MACsec frame")
    def test_decrypt_jumbo_chained(self):
        """chained ciphertext decrypts instead of hitting the CHAINED drop"""
        pkt = self.jumbo_pkt()
        protected = macsec_protect(
            raw(pkt), TEST_SAK, TEST_SCI, an=0, pn=1, send_sci=True
        )
        self.add_secy(self.pg0.sw_if_index, DIR_INGRESS)
        before = self.statistics.get_err_counter(
            "/err/sonic-ext-macsec-decrypt/dropped: chained buffer"
        )
        rx = self.send_and_expect(self.pg0, [self.wire_pkt(protected)], self.pg1)[0]
        self.assertEqual(raw(rx)[: len(raw(pkt))], raw(pkt))
        self.assertEqual(
            self.statistics.get_err_counter(
                "/err/sonic-ext-macsec-decrypt/dropped: chained buffer"
            ),
            before,
        )


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
