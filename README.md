# Exalted Bases

A D2RLoader plugin for Diablo II: Resurrected that adds a fourth tier of weapons and armor
above elite, called Exalted, and the Restoration Crystal that upgrades and restores them.

- Every elite base gets an exalted version with higher defense or damage. Exalted bases drop
  from the game's own loot tables, and can drop as uniques and set items of their family.
- The Restoration Crystal drops from bosses in Hell. In the cube it turns an elite item into
  the same item on its exalted base, or "restores" an exalted item with extra attributes.
- The plugin works on top of the mod you already play. It never changes the mod's files: it
  builds adjusted copies at game start and hands those to the game.
- No game art or data is included. Pictures are made from the installed game when it starts.

In the game and in its file names the plugin is called **Exalted Items**
(`d2rl-exalted-items.dll`, `exalted-items.toml`, console command `exalted`).

## Documents

- [User guide](Exalted-Items-User-Guide.md): what it adds, the settings, what to be careful with.
- [Change log](CHANGELOG.md)
- [How it works](plugin/README.md): the generated files, the hooks, the tests.

## Install

1. Put `d2rl-exalted-items.dll` in `<game>/d2rloader/plugins/` (every mod) or
   `<game>/mods/<mod>/d2rloader/plugins/` (one mod).
2. Put `exalted-items.toml` in the `config` folder beside that `plugins` folder.
3. Start the game through D2RLoader with your mod, as usual.

Characters that hold exalted items or crystals need the plugin from then on. See the user
guide before removing it.

## Build

Needs mingw-w64 and the [D2RLoader Plugin SDK](https://github.com/D2RLoader/PluginSDK) headers.

```
cd plugin
SDK=/path/to/PluginSDK/include ./build.sh
```

The DLL is written to `plugin/build/`.

## Tests

`plugin/test/run_tests.sh [mod data dir] [vanilla data dir] [game image dump]` runs every check
that does not need the game. It needs your own mod folder and data extracted from your own
copy of the game; none of that is in this repository. The part that loads the real DLL needs
Wine.

## Third-party code

`plugin/third_party/CascLib` is [CascLib](https://github.com/ladislav-zezula/CascLib) by
Ladislav Zezula, under its own license (see the `LICENSE` file there).
