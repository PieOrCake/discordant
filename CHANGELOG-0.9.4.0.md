# Discordant 0.9.4.0

## Crash fixes
- Errors in the overlay or settings panel are now logged instead of closing the game
- Fixed a rare memory corruption when signing in to Discord
- Disconnecting or closing the game can no longer freeze if Discord stops responding

## Bug fixes
- Overlay position is now remembered between game sessions (#1)
- The Discord connection recovers on its own after an error; "Connect to Discord" always works
- Your Discord sign-in token is no longer written to the Nexus log
- Custom Rich Presence Application ID is applied at startup
- Discord status is cleared when you close the game
- Failed avatar downloads are no longer retried every frame; changed avatars update
- Clearer log message when Discord and the game run with different permissions
- One invalid setting no longer resets the rest; settings are saved safely

## New options (#2)
- Hide the overlay when not in a voice channel
- Hide names (avatars only)
- Only show people who are speaking
- Avatar size (16–64 px)
- Background colour, with transparency down to 0%
- The overlay stays visible while the settings panel is open, so you can position it
