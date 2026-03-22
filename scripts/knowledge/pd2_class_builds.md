# PD2 Class Builds & Skill Trees

## Sorceress

### Lightning Sorceress (Chain Lightning)
**Primary skills**: Chain Lightning, Lightning, Lightning Mastery
**Support skills**: Teleport, Static Field, Frozen Armor, Warmth
**Key stats**: FCR (117% breakpoint), FHR (86% breakpoint), +Lightning skills
**Key gear**: Eschuta's Temper, Griffon's Eye, Infinity (merc weapon)
**Immunities**: Lightning immunes — broken by Infinity Conviction aura
**Skiller GC tab**: Sorc Lightning (vanilla tab 4, PD2 may differ)

### Fire Sorceress (Fireball/Meteor)
**Skiller GC tab**: Sorc Fire (vanilla tab 3)

### Cold Sorceress (Blizzard/Frozen Orb)
**Skiller GC tab**: Sorc Cold (vanilla tab 5)

## Amazon

### Javazon (Lightning Fury/Charged Strike)
**Most popular Amazon build**
**Skiller GC tab**: Amazon Javelin (vanilla tab 2) — most valuable Amazon skiller
**Key gear**: Titan's Revenge, Thunderstroke

### Bowazon
**Skiller GC tab**: Amazon Bow (vanilla tab 0)

## Necromancer

### Summoner
**Skiller GC tab**: Nec Summoning (vanilla tab 8)

### Poison Nova
**Skiller GC tab**: Nec Poison and Bone (vanilla tab 7)

## Paladin

### Hammerdin (Blessed Hammer)
**Skiller GC tab**: Pal Combat (vanilla tab 9)

### Smiter
**Skiller GC tab**: Pal Combat (vanilla tab 9)

## Barbarian

### Whirlwind
**Skiller GC tab**: Barb Combat (vanilla tab 12)

### Frenzy
**Skiller GC tab**: Barb Combat (vanilla tab 12)

## Druid

### Wind Druid
**Skiller GC tab**: Druid Elemental (vanilla tab 17)

### Summoner Druid
**Best PD2 S12 starter build**
**Skiller GC tab**: Druid Summoning (vanilla tab 15)

## Assassin

### Trapsin
**Skiller GC tab**: Assassin Traps (vanilla tab 18)

### Martial Arts
**Skiller GC tab**: Assassin Martial Arts (vanilla tab 20)

## PD2 Skill Tab ID Detection

To determine which skill tabs a character uses, read their skill point allocations
via D2COMMON_GetUnitStat with stat ID 107 (STAT_SINGLESKILL). The sub_index
of each skill tells you the skill ID, which maps to a skill tree.

### D2 Skill ID Ranges (vanilla)
- Amazon: 0-31
- Sorceress: 36-65
- Necromancer: 66-95
- Paladin: 96-125
- Barbarian: 126-155
- Druid: 221-250
- Assassin: 251-280

### Sorceress Skill IDs (vanilla)
- Fire tree: 36-46 (Fire Bolt, Warmth, Inferno, Blaze, Fire Ball, Fire Wall, Enchant, Meteor, Fire Mastery, Hydra)
- Lightning tree: 47-57 (Charged Bolt, Static Field, Telekinesis, Nova, Lightning, Chain Lightning, Thunder Storm, Energy Shield, Teleport, Lightning Mastery)
- Cold tree: 58-65 (Ice Bolt, Frozen Armor, Frost Nova, Ice Blast, Shiver Armor, Glacial Spike, Blizzard, Chilling Armor, Frozen Orb, Cold Mastery)

Note: PD2 may have different skill IDs due to added/modified skills.
Use the get_skills MCP tool to read actual allocations.
