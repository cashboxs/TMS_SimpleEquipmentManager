Player Simple Equipment Manager

Skyrim SE/AE SKSE plugin: player-only Combat/Town/Home outfit switching. No ESP or Papyrus. SKSE Menu Framework integration.

Behavior

- Outfit category is evaluated on valid LoadingMenu transitions; ordinary CELL/Location changes are *not* equipment-switch triggers.
- Confirmed player combat entry requests Combat equipment, including a new fight while Combat is the active group.
- Combat ending **does not** switch back. The existing outfit remains until the next valid loading transition.
- A verified combat true→false observation is latched until consumed by the game-thread state machine. This closes a window where a new fight could begin before the previous combat ended gate was cleared. No timer or repeating polling was added.
- Repeated same-category loading does not reapply its configuration, even if a user edited that category in the menu; the user has chosen to defer that UX change.
- Target group explicitly configured slots win on cross-group transitions; unspecified slots preserve manual replacements, while SEM-owned previous-group items may be removed.
- Equipment is drawn from the player's actual inventory; uniquely identifiable armor copies and enchantment/temper instances are tracked. Protected slots 50/52 are excluded, slot 41 is eligible.
- JSON profile is per character *display name*. Saves with identical display names share an outfit JSON. Configuration writes are immediate, but equip changes wait for qualifying transitions.
- Unavailable item rows remain visible for manual removal in the menu (appearance intentionally unchanged).

Packaging / Configuration

Build from CMakePresets.json with MSVC x64, CommonLibSSE NG, nlohmann_json and SKSE Menu Framework headers.