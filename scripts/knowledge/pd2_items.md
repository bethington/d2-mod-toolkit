# PD2 Item Knowledge Base

## Charm Values

### Small Charms (1x1)
**Max values**: +20 life, +11 single resist, +5 all resist
**Best in slot**: +20 life / +11 resist (worth Ber rune), +20 life / +5 all res (worth 3-4 Ber)
**Keep thresholds**:
- Life >= 17 (any)
- Single resist >= 9
- Life >= 12 AND resist >= 5 (dual mod)
- FRW >= 3
- MF >= 5 (if also has life)
- Max damage >= 3 AND AR >= 15 (physical builds)
**Vendor**: life < 10 with no other useful stat, plain with no stats

### Large Charms (1x2)
**Generally vendor** — space-inefficient compared to small charms
**Keep only if**: life >= 25, resist >= 10, or exceptional dual mod
**PD2 change**: Can roll +2-3% elemental damage

### Grand Charms (1x3)
**Skillers (+1 to skill tree) = ALWAYS KEEP** — high trade value
- Skiller + life >= 20 = premium value (Mal-Sur+ rune)
- Skiller + life >= 30 = GG roll
- Plain skiller = still valuable for trading
**Non-skiller GCs**: Generally vendor unless +30 life or exceptional
**Unique GCs (Gheed's Fortune)**: Keep — MF/GF/vendor price reduction

### Skill Tab IDs (stat 188 sub_index)
Standard D2: 0=Amazon Bow, 1=Amazon Passive, 2=Amazon Jav, 3=Sorc Fire,
4=Sorc Lightning, 5=Sorc Cold, 6=Nec Curses, 7=Nec PnB, 8=Nec Summon,
9=Pal Combat, 10=Pal Offense, 11=Pal Defense, 12=Barb Combat,
13=Barb Mastery, 14=Barb Warcries, 15=Druid Summon, 16=Druid Shape,
17=Druid Elem, 18=Assassin Traps, 19=Assassin Shadow, 20=Assassin MA

**PD2 NOTE**: Sub_indices observed go beyond 20 (seen: 2, 8, 26, 32, 34, 42, 49).
PD2 may have remapped or extended skill tab IDs. Need to build detection tool
to map sub_indices to actual skill trees by reading player skill allocations.

## Jewels

### Rainbow Facets (Unique Jewels)
**ALWAYS KEEP** — valuable for all elemental builds
- Fire/Cold/Lightning/Poison variants
- Die or Level-up trigger types
- -3 to -5% enemy resist = the valuable stat
- Lightning facets = most valuable (for Lightning Sorc/Java)
- Check: min_fire_dmg/min_cold_dmg/min_light_dmg/min_poison_dmg + skill_on_hit/skill_on_get_hit

### Magic/Rare Jewels
**Keep if**: IAS >= 15, ED >= 30 + another mod, resist >= 15, +skills
**Vendor if**: Only AR, only ED < 30, only energy, only stamina

### PD2 Jewel Changes
- Rare jewels always get 4 affixes
- Jewel Fragments can be stacked and used in recipes

## PD2 Special Items — KEEP ALL

### Uber Access Items
- **Voidstone**: Access Rathma (endgame boss)
- **Vision of Terror**: Access Diablo-Clone
- **Pandemonium Talisman**: Access Uber Tristram
- **Relic of the Ancients**: Access Uber Ancients
ALL ARE VALUABLE — required for endgame content

### Utility Items
- **Skeleton Key**: Unlimited key — keep
- **Larzuk's Puzzlebox**: Adds sockets to uniques/sets — very valuable
- **Worldstone Shard**: Corrupts items/maps — valuable
- **Demonic Cube**: Rerolls unique/set stats — valuable
- **Token of Absolution**: Resets skills — keep 2-3, vendor extras

### Tokens
- Keep 2-3 Tokens of Absolution for skill resets
- Vendor extras (they're relatively common)

## Equipment Quality Guidelines

### Auto-Keep
- **Unique**: Always keep (evaluate later for corruption potential)
- **Set**: Always keep
- **Runeword bases**: Superior with ED or good base type

### Auto-Vendor
- **Inferior**: Always vendor
- **Normal**: Vendor unless elite base with good stats for runewords

### Review (manual or rule-based)
- **Rare**: Keep if FCR + resist, +skills, or life + multiple resists
- **Craft**: Keep if good rolls (treat like rares)
- **Magic**: Generally vendor unless exceptional (e.g., +6 skill javelin)
- **Superior**: Keep if elite base suitable for runewords (4os, high ED)

## Corruption System
- **Worldstone Shard** corrupts items
- **Outcomes**: Add sockets, add corruption modifier, or brick to random rare
- **Best corruptions**: +1 All Skills, Indestructible, max resist
- **Strategy**: Corrupt BEFORE socketing with Puzzlebox (save Puzzlebox for known good corruptions)
- **Corrupted items cannot be corrupted again** but can use other recipes

## Lightning Sorceress Gear Priorities
- **FCR breakpoints**: 117% (key breakpoint for Lightning/Chain Lightning)
- **FHR breakpoint**: 86%
- **Key items**: Eschuta's Temper, Griffon's Eye, Infinity (merc)
- **Rainbow Facets**: Lightning = most valuable variant
- **Skiller GCs**: Sorc Lightning tree = highest priority to keep
- **Resistances**: Stack all to 75 (max) for Hell difficulty
