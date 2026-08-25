"""Capability definitions for Taco's on/off feature controls.

Hardware-enforced capabilities (listening, camera) are the device's own
responsibility: the CoreS3 will not stream mic or camera data when its local
toggle is off, regardless of what the hub thinks. The hub's copy of those
keys reflects the last known device state and is used to push
`capabilities_set` on reconnect, not to gate hardware directly.

Software-only capabilities (memory, habit_tracking, cloud_ai, kids_mode,
proactive) are owned by the hub; it is the source of truth for those.
"""

from __future__ import annotations

from typing import Any

HARDWARE_KEYS = {"listening", "camera"}
SOFTWARE_KEYS = {"memory", "habit_tracking", "cloud_ai", "kids_mode", "proactive"}
ALL_KEYS = HARDWARE_KEYS | SOFTWARE_KEYS

DEFAULT_CAPABILITIES: dict[str, Any] = {
    "listening": True,
    "camera": True,
    "memory": True,
    "habit_tracking": True,
    "cloud_ai": True,
    "kids_mode": False,
    "proactive": True,
}


def validate_updates(updates: dict[str, Any]) -> dict[str, Any]:
    """Filter to known capability keys with boolean values."""
    cleaned: dict[str, Any] = {}
    for key, value in updates.items():
        if key not in ALL_KEYS:
            continue
        cleaned[key] = bool(value)
    return cleaned
