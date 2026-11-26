# W13 1P impact vibration

`--gameplay-rumble` enables impact detection for the craft in slot `00B8C500`. Fighter, GERWALK, and Battroid share the mover and the detector, including falls. The hooks read guest state and pass closing speed to `cellPad_gameplay_impact(0, closing_speed)`. The surface trace is optional. [m28 capture and episode rules](SURFACE-TRACE.md)

## Lifted sites

| Site | Impact input |
| --- | --- |
| `005:0025712C → 00257D88` | Scope one mover update. Keep separate ground and wall latches for the current guest thread, actor, and body. |
| `005:00257488/002574BC → 0025748C/002574C0`, `00258468` | Early swept contacts, incoming velocity, and confirmation that the position correction was stored. |
| `005:0025773C → 00257740`, `00251804/002518B4` | Resolver incoming velocity and result, accepted world sweep, and final object direction at `sp+0xE0`. |
| `005:0025077C/002510D4/00251984/00251FC8` | Actor-backed producer evidence. The final resolver direction requires a successful producer and a nonzero object or support pointer. |
| `007:0035AEE8/0035DFD0` | Confirm the normal from the selected world sweep or the primary stage-model pass at `0035AE80`. |
| `005:00259DBC/0025A444/0025B44C/0025D5F4/0025E014/0025C348` and `0025AC2C → 0025AC9C`, `0025C318` | Paired velocity stores and travel-mode transition. The Fighter pair spans two writes. If several responses ran, use the incoming velocity from the pair with the largest outward change along the contact normal. |
| `005:00258A40` | Read the enabled water plane, old and new position Y, and incoming vertical velocity. Require downward entry across the 0.01-unit deadband. |

On ground and walls, normalize the confirmed normal and use Y ≥ 0.65 for ground. A kind arms after an observed separation. A skipped resolver or an unclassified hit leaves absent contact types unconfirmed. An armed contact needs positive closing speed and a stored position correction, a travel-mode transition, or an outward velocity change. Closing speed is `max(0, -dot(v_in, n))` for solid surfaces and `-v_in.y` on downward water entry.

With `MACROSS_ACTOR_SURFACE_TRACE=1` and a field trace CSV enabled, the mover row includes `["impact_emit",0,c,t,"ground"]` for a qualifying guest request. The final value can also be `"wall"` or `"water"`. Host settings and merge determine whether that request starts or replaces a pulse.

## Host tuning

The starting constants are together in `native/host/gameplay_rumble.h`. Closing speeds below **1.5** are silent. From **1.5** to **8.0**, interpolate the current slider’s existing weak and strong motor levels and durations with `t = clamp((c - 1.5) / (8.0 - 1.5), 0, 1)`. At **100%**, the endpoints are approximately **70% low / 30% high for 225 ms** and **100% low / 100% high for 750 ms**.

The impact channel stores its own level and deadline. A request less than **300 ms** after the last impact start replaces it only when its `t` is larger. The replacement starts a new deadline. The pad backend recomputes the per-motor maximum of the impact, the existing gameplay hit output, and guest actuator levels at the first active deadline. F9 `진동` off and strength **0%** clear pending impact vibration and suppress new impact pulses.

## Replay and checks

The native replay consumed all 6,306 mover rows from `m28-lto/run-surface-20261126-010823`. The 9.116 wall and 9.299 ground impacts started strong pulses. The 7.627 response 19 ms later merged into the running strong pulse. The 4.102 ground impact started a pulse at `t ≈ 0.40`. Ground impacts at 1.371, 0.657, 0.753, 0.253, and 0.121 and the downward water entry at 0.710 were silent. The two upward water crossings were ignored.

The pulse fixture covers the slider at 0%, 50%, 100%, and 200%, both merge directions, 30 and 144 FPS expiry, actuator mixing, and the enable and strength settings. The lifted fixture compares the complete PPU context and guest memory byte-for-byte across the baseline, impact with tracing off, and impact with tracing on. A scripted movement callee supplies a fast downward crossing to the lifted mover. Both impact variants request the same pulse for 1P and ignore the enemy crossing. The trace records `["impact_emit",0,9,1,"water"]`. Both register-local configurations pass syntax checks.

```powershell
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
python -B tools/tests/test_gameplay_rumble.py
python -B tools/tests/test_surface_impact.py
python -B tools/tests/test_surface_trace.py
python -B tools/tests/test_real60_builder.py
```

## Controller playtest

Repeat the contact matrix with free FPS and F9 locked to 30 FPS. Start at 100% strength and enable the surface trace to correlate each request with the controller response.

| Form | Ground | Wall or structure | Water |
| --- | --- | --- | --- |
| Fighter | Low and high speed | Low and high speed | Slow and fast downward entry |
| GERWALK | Low and high speed | Low and high speed | Slow and fast downward entry |
| Battroid | Low and high speed | Low and high speed | Slow and fast downward entry |

- [ ] Include a fall from altitude, a frontal building strike, and an actor-backed obstacle. Check that greater closing speed gives a stronger, longer pulse.
- [ ] Check sliding, stationary support, a shallow skim, and upward water exit. Confirm that sustained contact produces no repeat and clear separation allows a fresh impact.
- [ ] Compare two bounces within 300 ms, first with the stronger hit leading and then with the weaker hit leading. Verify that the weaker follow-up is suppressed and the stronger one upgrades the pulse.
- [ ] Check a hit or knockdown during impact, and repeat an impact with F9 vibration off and with the slider at 0%.

## Live result (2026-11-26, accepted)

Run `build/tests/Macross-real60/m29-lto/run-impact-20261126-205338/`: 1.5x, free rate, strength 80%, field and surface trace on, exit 0, user saves unchanged. The analyzer (`surface-analysis.txt` in the run) found 20,723 mover updates and 51 contact episodes. Fighter and Battroid hit ground, walls, and water; no GERWALK and no F9 30 FPS samples. The guest requested 27 impact pulses (closing 1.69–10.24; 6 at `t = 1`), all 1P, kinds wall 18, ground 5, water 4. Settling contacts below 1.5 and slow Battroid water entries stayed silent. Follow-ups within 300 ms (e.g. 10.24 then 7.08 after 41 ms) are left to the host merge.

The user accepted the behaviour as is ("이대로 간다"). The starting constants 1.5, 8 and 300 ms stay. Unchecked live: GERWALK, F9 30 FPS, slider 0% and vibration off.

