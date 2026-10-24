# SPDX-License-Identifier: GPL-2.0-or-later
"""GODZILLA's trophies (CUSA02046) for the launcher's Achievements page: their texts and grades in
trophy id order (the runtime's own copy is src/runtime_services.c, godzilla_trophies; a test keeps
the two the same), the icons from the dump's trophy00.trp and the unlocked set the runtime saves.

The texts in trophy00.trp are encrypted (TROP.ESFM); its icons (TROP000.PNG...) are not.
"""
import struct
from pathlib import Path

GRADES = ('Platinum', 'Gold', 'Silver', 'Bronze')
# (name, description, grade) by trophy id.
TROPHIES = (
    ('GODZILLA', 'Earn all trophies.', 'Platinum'),  # 0
    ('The Scars of Disaster', 'Finish the tutorial.', 'Bronze'),  # 1
    ('Recurring Nightmare', 'Go ashore for the first time in God of Destruction mode.', 'Bronze'),  # 2
    ('Path of Humanity', 'Clear Stage 7 in God of Destruction mode.', 'Bronze'),  # 3
    ('Deep-Sea Slumber', 'Clear Stage 9 in God of Destruction mode.', 'Bronze'),  # 4
    ("A Monster's Legacy", 'Clear the Final Stage in God of Destruction mode.', 'Silver'),  # 5
    ('Mechagodzilla Hunter', 'Destroy all 4 Mechagodzillas in God of Destruction mode.', 'Silver'),  # 6
    ('Amateur Photographer', 'Collect first set of data at a photo location in God of Destruction mode.', 'Bronze'),  # 7
    ('Photography Enthusiast', 'Collect 50% of data at photo locations in God of Destruction mode.', 'Bronze'),  # 8
    ('Photographer Extraordinaire', 'Collect 100% of data at photo locations in God of Destruction mode.', 'Gold'),  # 9
    ('God of Destruction', 'Clear all areas in God of Destruction mode.', 'Silver'),  # 10
    ('Bonus Beginner', 'Get your first Extra Bonus', 'Bronze'),  # 11
    ('Our Hero Jet-Jaguar', 'Defeat Jet-Jaguar 3 times in one playthrough of God of Destruction mode.', 'Silver'),  # 12
    ('Kaiju Killer', 'Defeat all types of Kaiju in God of Destruction mode.', 'Silver'),  # 13
    ('Advanced Weapon Destroyer', 'Destroy all G-Force advanced weaponry in God of Destruction mode.', 'Silver'),  # 14
    ('Helicopter Swatter', 'Destroy 20 or more helicopters in one area in God of Destruction mode.', 'Bronze'),  # 15
    ('Jet Fighter Downer', 'Destroy 5 or more jet fighters in one area in God of Destruction mode.', 'Bronze'),  # 16
    ('Fury 10', 'Reach 10 fury in God of Destruction mode.', 'Bronze'),  # 17
    ('Fury 30', 'Reach 30 fury in God of Destruction mode.', 'Bronze'),  # 18
    ('Fury 50', 'Reach 50 fury in God of Destruction mode.', 'Silver'),  # 19
    ('100% Destruction', 'Reach 100% destruction rate in any area in God of Destruction mode.', 'Bronze'),  # 20
    ('80m and Growing', 'Grow Godzilla to a height of 80m or more in God of Destruction mode.', 'Bronze'),  # 21
    ('90m and Still Growing', 'Grow Godzilla to a height of 90m or more in God of Destruction mode.', 'Bronze'),  # 22
    ('100m Tall', 'Grow Godzilla to a height of 100m in God of Destruction mode.', 'Silver'),  # 23
    ('Challenger', 'Play King of Kaiju mode for the first time.', 'Bronze'),  # 24
    ('King of Kaiju', 'Clear King of Kaiju mode for the first time.', 'Bronze'),  # 25
    ('Finished in 10', 'Clear King of Kaiju mode in 10 minutes or less.', 'Bronze'),  # 26
    ('Finished in 8', 'Clear King of Kaiju mode in 8 minutes or less.', 'Silver'),  # 27
    ('Finished in 6', 'Clear King of Kaiju mode in 6 minutes or less.', 'Gold'),  # 28
    ('VS Beginner', 'First time playing VS Mode', 'Bronze'),  # 29
    ('VS Winner', 'First victory in VS Mode', 'Bronze'),  # 30
    ('VS 10', 'Play VS Mode 10 times', 'Bronze'),  # 31
    ('Indication of Evolution', 'Evolve Godzilla in Evolution mode for the first time', 'Bronze'),  # 32
    ('Showa Demon', 'Obtain Showa-style Godzilla skills in Evolution mode. (Jump Attack, Back Charge)', 'Bronze'),  # 33
    ('Killer Breath', 'Unlock all atomic breath types for Godzilla in Evolution mode.', 'Bronze'),  # 34
    ('Perfect Being', 'Complete all evolutions for Godzilla in Evolution mode.', 'Silver'),  # 35
    ('Kaiju Sighting', 'See your first Kaiju outside of Godzilla and unlock it for play.', 'Bronze'),  # 36
    ('Kaiju Collector', 'See and unlock all available Kaiju.', 'Bronze'),  # 37
    ('Kaiju Master', 'Evolve every Kaiju to maximum level.', 'Gold'),  # 38
    ('Diorama Beginner', 'Use Diorama mode for the first time.', 'Bronze'),  # 39
    ('Figure Collector', 'Earn all figures in Diorama mode.', 'Bronze'),  # 40
    ('Base Master', 'Earn all bases in Diorama mode.', 'Bronze'),  # 41
    ('Godzilla Fan', 'View the Kaiju Guide for the first time.', 'Bronze'),  # 42
    ('Godzilla Expert', 'Collect all entries in the Kaiju Guide.', 'Silver'),  # 43
    ('Crazy for Godzilla', 'View all entries in the Kaiju Guide.', 'Bronze'),  # 44
)


def read_trp_icons(path):
    """{trophy id: PNG bytes} of a trophy00.trp (big-endian header: magic DC A2 4D 00, the file
    count at 0x10, the entry size at 0x14; entries from 0x60: a 32-byte name, offset, size)."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return {}
    if len(data) < 0x60 or data[:4] != bytes((0xDC, 0xA2, 0x4D, 0x00)):
        return {}
    count, size = struct.unpack_from('>II', data, 0x10)
    if not 0x30 <= size <= 0x100:
        return {}
    icons = {}
    for i in range(count):
        entry = 0x60 + i * size
        if entry + 0x30 > len(data):
            break
        name = data[entry:entry + 32].split(b'\0')[0].decode('ascii', 'replace')
        offset, length = struct.unpack_from('>QQ', data, entry + 0x20)
        if name.startswith('TROP') and name.endswith('.PNG') and name[4:7].isdigit() and offset + length <= len(data):
            icons[int(name[4:7])] = data[offset:offset + length]
    return icons


def read_unlocked(path):
    """The ids the runtime saved as unlocked (trophies.dat: four little-endian 32-bit masks)."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return set()
    if len(data) < 16:
        return set()
    masks = struct.unpack_from('<4I', data)
    return {i for i in range(128) if masks[i // 32] >> (i % 32) & 1}
