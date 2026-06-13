#!/usr/bin/env python3
"""Lightweight protocol checks that do not require ESP-IDF or hardware."""

import re
import unittest


NODE_RECV_RE = re.compile(r"^NODE_RECV:0x[0-9A-Fa-f]{4}:STATE:[01]$")
NODE_SEND_RE = re.compile(r"^NODE_SEND:0x[0-9A-Fa-f]{4}:STATE:[01]$")
GROUP_SEND_RE = re.compile(r"^GROUP_SEND:0x[0-9A-Fa-f]{4}:STATE:[01]$")


class ProtocolFormatTests(unittest.TestCase):
    def test_node_recv_format(self):
        self.assertTrue(NODE_RECV_RE.match("NODE_RECV:0x2638:STATE:1"))

    def test_downstream_formats(self):
        self.assertTrue(NODE_SEND_RE.match("NODE_SEND:0x2638:STATE:1"))
        self.assertTrue(GROUP_SEND_RE.match("GROUP_SEND:0x0001:STATE:0"))


if __name__ == "__main__":
    unittest.main()
