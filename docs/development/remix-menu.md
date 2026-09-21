# Remix menu in Skyrim

The D3D11 presenter draws Remix's existing ImGui menu into the primary Skyrim
swap-chain image after the game/UI blit and before presentation. It is not a
second rendering window. Alt+X is Remix's existing configurable menu shortcut.

Skyrim uses DirectInput. The plugin feeds its original input events to
`csRemixGuiInput` before dispatching to either Skyrim or the CS menu. The runtime
queues events under a mutex and consumes them on the ImGui render thread before
`NewFrame`. Mouse position comes from the Win32 backend in client coordinates.
The D3D11 path disables Remix's separate raw-input sink before it starts, so it
does not take over process-wide raw-input registration. D3D9 input is unchanged.

The private ABI uses three scalar arguments `(uint32_t type, uint32_t code,
float value)` and returns a `uint32_t` input-blocking flag:

| Type | Code | Value |
| --- | --- | --- |
| 0 | ignored | ignored; query capture only |
| 1 | Windows virtual key | 1 down, 0 up |
| 2 | mouse button 0–4 | 1 down, 0 up |
| 3 | ignored | signed vertical wheel steps |
| 4 | character code point | ignored |
| 5 | ignored | 1 focused, 0 unfocused |

Capture is published atomically when Remix's menu is visible and its existing
`blockInputToGameInUI` option is enabled. The plugin consumes the whole game
input list while captured, including relative mouse movement. It continues
feeding Remix, so closing the menu does not require the game to process Alt+X.
Window activation events clear held keys through the same queue. Events are
bounded to prevent unbounded growth if presentation stalls.

## Testing

Build both the optimized plugin and runtime, then deploy both with the backup
scripts. Verify in a loaded save:

- Alt+X closes and reopens the menu.
- Developer menu buttons, collapsible sections, and scrolling work.
- Camera angles/position stay unchanged while navigating the menu.
- Closing the menu returns input to the game; alt-tabbing does not stick keys.

`test_menu_input_contract.py` checks wiring and queue invariants, not actual
rendering or input behavior. Desktop screenshots are necessary: CS captures are
taken before Present and do not include this last-stage menu overlay.

Known unrelated limitation: changing DLSS resolution/profile live while frame
generation is active has previously crashed this integration. Menu availability
does not establish that every runtime graphics setting is safe to change live.
