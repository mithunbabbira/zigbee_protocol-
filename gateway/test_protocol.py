#!/usr/bin/env python3
"""Unit tests for JSON gateway protocol helpers."""

from protocol import (
    format_turn_led_off,
    format_turn_led_on,
    normalize_ieee_address,
    parse_gateway_line,
)


def test_normalize_ieee():
    assert normalize_ieee_address("0x1051DBFFFE1C4A78") == "0x1051dbfffe1c4a78"


def test_parse_events():
    ev = parse_gateway_line('{"event":"NODE_ONLINE","ieee":"0x1051dbfffe1c4a78","short":"0x3b4c"}')
    assert ev is not None
    assert ev.kind == "join"
    assert ev.ieee == "0x1051dbfffe1c4a78"

    ev = parse_gateway_line('{"event":"BUTTON_PRESSED","ieee":"0x1051dbfffe1c4a78"}')
    assert ev is not None
    assert ev.kind == "button"

    ev = parse_gateway_line('{"event":"LED_STATE","ieee":"0x1051dbfffe1c4a78","state":1}')
    assert ev is not None
    assert ev.kind == "state"
    assert ev.state == 1


def test_downstream_format():
    assert '"TURN_LED_ON"' in format_turn_led_on("0x1051dbfffe1c4a78")
    assert '"TURN_LED_OFF"' in format_turn_led_off("0x1051dbfffe1c4a78")


if __name__ == "__main__":
    test_normalize_ieee()
    test_parse_events()
    test_downstream_format()
    print("protocol tests OK")
