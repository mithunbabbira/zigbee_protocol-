#!/usr/bin/env python3
"""Mirror shelf_gateway_parse_line behavior for automated validation."""

import unittest


def parse_line(line: str):
    line = line.strip()
    for prefix, cmd_type in (("NODE_SEND:", "node"), ("GROUP_SEND:", "group")):
        if not line.startswith(prefix):
            continue
        rest = line[len(prefix) :]
        if ":STATE:" not in rest:
            return None
        addr_part, state_part = rest.split(":STATE:", 1)
        addr_part = addr_part.removeprefix("0x").removeprefix("0X")
        try:
            return {
                "type": cmd_type,
                "addr": int(addr_part, 16),
                "state": 1 if int(state_part) else 0,
            }
        except ValueError:
            return None
    return None


class ParseLogicTests(unittest.TestCase):
    def test_node_send(self):
        parsed = parse_line("NODE_SEND:0x2638:STATE:1")
        self.assertEqual(parsed["type"], "node")
        self.assertEqual(parsed["addr"], 0x2638)
        self.assertEqual(parsed["state"], 1)

    def test_group_send(self):
        parsed = parse_line("GROUP_SEND:0x0001:STATE:0")
        self.assertEqual(parsed["type"], "group")
        self.assertEqual(parsed["addr"], 0x0001)
        self.assertEqual(parsed["state"], 0)


if __name__ == "__main__":
    unittest.main()
