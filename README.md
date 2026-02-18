# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore

## mod-creature-capture

### This is a module for [AzerothCore](http://www.azerothcore.org)

Capture NPCs and turn them into guardian companions that follow you, fight alongside you, and persist across sessions.

## Features

- **Capture creatures** - Target a creature and use `.capture` or the Tesseract item to make it your guardian
- **Tesseract item** - Use the Tesseract to summon, dismiss, view info, or release your guardian
- **Persistent guardians** - Your captured guardian is saved to the database and persists across logouts
- **Original AI preserved** - Guardians retain their original combat behavior (SmartAI, scripted abilities, etc.)
- **Cross-map support** - Guardians automatically respawn when you change maps
- **Configurable** - Adjust capture rules, guardian stats, and more via config

## Requirements

- AzerothCore with latest master branch

## Installation

1. Place the module in your `modules` folder:
   ```bash
   cd /path/to/azerothcore/modules
   git clone https://github.com/your-repo/mod-creature-capture.git
   ```

2. Re-run CMake and rebuild:
   ```bash
   cd build
   cmake ..
   make -j$(nproc)
   ```

3. Apply SQL files to your databases:
   - `data/sql/db-world/base/tesseract_item.sql` → world database
   - `data/sql/db-characters/base/character_guardian.sql` → characters database

4. Copy the config file:
   ```bash
   cp modules/mod-creature-capture/conf/mod_creature_capture.conf.dist etc/mod_creature_capture.conf
   ```

5. Restart worldserver

## Commands

| Command | Description |
|---------|-------------|
| `.capture` | Capture your targeted creature |
| `.capture dismiss` | Dismiss your current guardian |
| `.capture info` | Display information about your captured guardian |

## Tesseract Item

Players receive a **Tesseract** item (ID: 44807) when they first capture a creature. This item provides a gossip menu to:

- **Summon Guardian** - Bring out your captured companion
- **Dismiss Guardian** - Send your guardian away temporarily
- **Guardian Info** - View your guardian's stats and status
- **Release Guardian** - Permanently release your guardian (cannot be undone)

GMs can give the Tesseract manually: `.additem 44807`

## Configuration

Edit `mod_creature_capture.conf` to customize the module:

| Option | Default | Description |
|--------|---------|-------------|
| `CreatureCapture.Enable` | 1 | Enable/disable the module |
| `CreatureCapture.Announce` | 1 | Announce module to players on login |
| `CreatureCapture.GuardianDuration` | 0 | Guardian duration in seconds (0 = permanent) |
| `CreatureCapture.AllowElite` | 0 | Allow capturing elite creatures |
| `CreatureCapture.AllowRare` | 1 | Allow capturing rare creatures |
| `CreatureCapture.MaxLevelDiff` | 5 | Max level difference for capture |
| `CreatureCapture.MinCreatureLevel` | 1 | Minimum creature level that can be captured |
| `CreatureCapture.HealthPct` | 100 | Guardian health % of original creature |
| `CreatureCapture.DamagePct` | 100 | Guardian damage % of original creature |

## How It Works

1. **Find a creature** you want to capture
2. **Target it** and use `.capture` or the Tesseract item
3. The creature becomes your **guardian companion**
4. Use the **Tesseract** item to manage your guardian
5. Your guardian **fights with you**, using its original abilities
6. Guardian **persists** across logouts and map changes

## Capture Restrictions

- Creature must be alive
- Cannot capture pets, guardians, or summons
- Cannot capture bosses or world bosses
- Cannot capture creatures in combat with other players
- Cannot capture critters
- Elite/rare restrictions configurable
- Level difference restrictions configurable
- Must be within 30 yards of target

## Tips: NPCs That Work as Healers

Captured guardians retain their original SmartAI scripts. Not all NPCs with healing spells will heal **you** — many only heal themselves (e.g. Soulguard Adept 36620). The NPCs listed below have SmartAI scripts that trigger heals, buffs, shields, or dispels on **friendly targets**, meaning they will actively support you in combat.

### Classic Dungeons & World

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 1717 | Hamhock | Bloodlust |
| 1895 | Pyrewood Elder | Lesser Heal |
| 2170 | Blackwood Ursa | Rejuvenation |
| 2171 | Blackwood Shaman | Healing Wave |
| 2640 | Vilebranch Witch Doctor | Healing Wave |
| 2647 | Vilebranch Soul Eater | Dark Offering |
| 4427 | Ward Guardian | Healing Wave |
| 4467 | Vilebranch Soothsayer | Healing Wave |
| 4809 | Twilight Acolyte | Heal, Renew |
| 4820 | Blindlight Oracle | Heal, Renew |
| 4847 | Shadowforge Relic Hunter | Heal |
| 4852 | Stonevault Oracle | Healing Wave |
| 5650 | Sandfury Witch Doctor | Flash Heal |
| 5717 | Mijan | Renew |
| 7247 | Sandfury Soul Eater | Dark Offering |
| 7354 | Ragglesnout | Heal |
| 7795 | Hydromancer Velratha | Healing Wave |
| 7995 | Vile Priestess Hexx | Heal |
| 7996 | Qiaga the Keeper | Renew |

### Wailing Caverns

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 3669 | Lord Cobrahn | Healing Touch |
| 3670 | Lord Pythas | Healing Touch |
| 3671 | Lady Anacondra | Healing Touch |
| 3673 | Lord Serpentis | Healing Touch |
| 3840 | Druid of the Fang | Healing Touch |

### Scarlet Monastery

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 4292 | Scarlet Protector | Heal |
| 4296 | Scarlet Adept | Heal |
| 4299 | Scarlet Chaplain | PW:Shield, Renew |
| 4303 | Scarlet Abbot | Renew, Heal |

### Blackrock Depths / Spire

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 7608 | Murta Grimgut | Renew, PW:Shield, Heal |
| 8894 | Anvilrage Medic | PW:Fortitude, Heal |
| 8895 | Anvilrage Officer | Holy Light |
| 8896 | Shadowforge Peasant | Heal |
| 8898 | Anvilrage Marshal | Holy Light |
| 8902 | Shadowforge Citizen | Heal |
| 8904 | Shadowforge Senator | Fire Shield III |
| 8913 | Twilight Emissary | Fury of Ragnaros |
| 8914 | Twilight Bodyguard | Seal of Sacrifice |
| 8915 | TH Ambassador | Bloodlust |
| 9217 | Spirestone Lord Magus | Enlarge, Bloodlust |
| 11880 | Twilight Avenger | Vengeance |

### Stratholme / Scholomance

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 10420 | Crimson Initiate | Flash Heal, Renew |
| 10421 | Crimson Defender | Holy Light |
| 10423 | Crimson Priest | Heal |
| 10471 | Scholomance Acolyte | Dark Mending |
| 10917 | Aurius | Holy Light |
| 10949 | Silver Hand Disciple | Holy Light |

### Dire Maul

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 11461 | Warpwood Guardian | Regrowth |
| 11473 | Eldreth Spectre | Dark Offering |
| 14303 | Petrified Guardian | Regrowth |
| 14398 | Eldreth Darter | PW:Shield |

### Karazhan

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 16406 | Phantom Attendant | Shadow Rejuvenation |
| 16409 | Phantom Guest | Heal |
| 17007 | Lady Keira Berrybuck | Holy Light, Cleanse |

### TBC Dungeons & Raids

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 16053 | Korv | Heal |
| 16146 | Death Knight | Death Coil (heal) |
| 17084 | Avruu | Dark Mending |
| 17833 | Durnholde Warden | Heal, Dispel |
| 18093 | Tarren Mill Protector | Flash Heal, Cleanse |
| 18500 | Unliving Cleric | Heal, Renew |
| 18702 | Auchenai Necromancer | Shadow Mend |
| 20036 | Bloodwarder Squire | Flash of Light, Cleanse |
| 20766 | Bladespire Mystic | Healing Wave |
| 21224 | Tidewalker Depth-Seer | Healing Touch, Rejuvenation |
| 21270 | Cosmic Infuser | Heal |
| 21350 | Gronn-Priest | Heal, Renew |
| 22305 | Vekh | Heal |
| 22873 | Coilskar General | Free Friend |
| 22876 | Coilskar Soothsayer | Restoration |
| 22964 | Sister of Pleasure | Heal, PW:Shield |
| 23524 | Ashtongue Spiritbinder | Chain-Heal, Spirit Mend |

### Sunwell / Magister's Terrace

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 24684 | Sunblade Blood Knight | Holy Light |
| 25371 | Sunblade Dawn Priest | Renew |

### WotLK Dungeons

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 26639 | Drakkari Shaman | Chain Heal |
| 26728 | Mage Hunter Initiate | Renew |
| 26735 | Azure Scale-Binder | Heal |

### Battlegrounds

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 26803 | Horde Cleric | PW:Shield, Flash Heal |
| 26805 | Alliance Cleric | PW:Shield, Flash Heal |

### Ulduar

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 33355 | Misguided Nymph | Bind Life |
| 33525 | Mangrove Ent | Nourish, Tranquility |
| 33818 | Twilight Adherent | Greater Heal |
| 34198 | Iron Mender | Fuse Metal |
| 34267 | Parts Recovery Technician | Defense Matrix |

### Icecrown Citadel

| Entry | NPC Name | Abilities |
|-------|----------|-----------|
| 37571 | Darkfallen Advisor | Shroud of Protection, Shroud of Spell Warding |

## Credits

- AzerothCore: [Repository](https://github.com/azerothcore) | [Website](https://azerothcore.org/) | [Discord](https://discord.gg/gkt4y2x)

## License

This module is released under the [GNU AGPL v3](https://www.gnu.org/licenses/agpl-3.0.en.html) license.
