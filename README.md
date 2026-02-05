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

## Credits

- AzerothCore: [Repository](https://github.com/azerothcore) | [Website](https://azerothcore.org/) | [Discord](https://discord.gg/gkt4y2x)

## License

This module is released under the [GNU AGPL v3](https://www.gnu.org/licenses/agpl-3.0.en.html) license.
