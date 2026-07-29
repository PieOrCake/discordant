# Discordant 0.9.2.1

Crash fix release. If 0.9.2.0 crashed your game on startup, this is the one to install.

## Fixed

- **Crash on addon load.** Discordant registered a Quick Access icon that pointed at a keybind it never created, which could take the game down as soon as Nexus drew the icon bar. The icon has been removed entirely — it did nothing when clicked, and the overlay appears on its own when you join a voice channel.
- **Crash for characters with accented or non-Latin names.** Long names were being cut mid-character by the game's own name buffer, and Rich Presence then refused to send them, killing the game instead.
- **Crash on unexpected Discord messages.** Several voice events assumed Discord always replies in one exact shape. Error replies and alternate formats now get handled instead of terminating Guild Wars 2.
- **Hang when closing the game.** A partially delivered Discord message could stall the Rich Presence connection and leave Guild Wars 2 refusing to exit.
- **Crash on a corrupted Discord connection.** Malformed data on the Rich Presence pipe could trigger a huge memory request. The connection now resets instead.
- **Unload safety.** Disabling or reloading the addon from Nexus no longer risks using memory that has already been freed.

If Discordant runs into trouble now, it disconnects and writes to the Nexus log rather than closing the game.

## Notes

There are no new features or settings in this release, and your existing `config.json` carries over untouched.
