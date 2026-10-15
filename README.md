# Saiga-12K for Geronimo (personal use)

A [GML](https://github.com/RemArrow/geronimo-mod-loader) plugin that adds a **Saiga-12K** to the Shotgun spawner:
a full-auto 12 gauge firing slugs, with the 5-round Sb.5 magazine. It lives in the gun station's
**Shotguns** category, which the game ships hidden and the plugin shows. It replaces no game asset.

**Not for distribution.** The model and textures are the Saiga-12K from Escape from Tarkov (Battlestate
Games), exported from the user's own copy of the game. They live only in `Assets\` (gitignored) and,
encrypted, inside `Saiga12.dll`. Don't commit, upload or share either.

## How it works

- **Base gun: the game's AK-74M.** Same action as a Saiga: side charging handle, rock-in magazine, and a
  safe/auto/semi lever. Handling, grip points and the bolt logic stay the AK-74M's.
- **Spawner entry:** a `GunAsset` built at runtime with the AK-74M's receiver, an icon rendered from the
  Saiga model, and category Shotgun.
- **Looks:** an AK-74M spawned from the entry, or rebuilt from a saved loadout that holds it, gets these as
  DynamicMeshComponents, and the AK-74M mesh is hidden:
  - the Saiga body;
  - its muzzle brake (hidden while a muzzle device is mounted);
  - its own moving parts (below), with the AK-74M's moving parts hidden.

  The model is fitted by the pistol grip. Both guns use the same AK-74 grip, and the fit is a 1.2 mm RMS
  ICP on the cooked AK-74M mesh, done in `tools\prep`. The muzzle point and the side-rail mount move onto
  the Saiga's.
- **The Saiga's own action:** its moving parts are its own meshes. The plugin poses them every frame about
  the Saiga's own pivots (the Tarkov rig's, fitted), from the state the game keeps on the receiver:

  | Part | Motion |
  |---|---|
  | Bolt carrier with the charging handle | slides as far as the game moves the AK-74M's carrier bone (firing cycles, a pulled handle, the safety's stop), 12 cm at most. That is the room in the Saiga's receiver: the carrier's tail stops a millimetre short of the recoil spring guide's rear block, and the bolt face ends 1.3 cm behind the magazine, so a shell can come up. The AK-74M's 15 would push the carrier out the back |
  | Bolt | stays for the carrier's first 0.55 cm, turns 35° to unlock over the next 0.35 cm, then travels with it; the reverse going home |
  | Fire selector | the Saiga's lever on its own axle. **Safe** is all the way up (−20°): its top edge closes the slot under the dust cover, and its front end sits in the charging handle's path, as on an AK. On safe the handle stops against it, 6.1 cm back (a real AK's stops about 6.3 cm back), not far enough to chamber a shell. **Auto** is −8°, the highest the lever can be while clearing the handle's full stroke. **Semi** is down at the "F" (0°, where Tarkov models it). It swings between them at 240°/s. The AK-74M's angles (11.5° and 20.5° down) put the lever across the Saiga's magazine |
  | Trigger | back 10° on its pin with the pull (`CurrentTrigger`) |
  | Magazine release | the paddle swings forward 12° while the release is pressed |

  The recoil spring assembly stays put under the dust cover. If the engine's transform layout ever differs,
  the parts ride the AK-74M's bones instead, and the log says so. The travel, the safe angle and its stop
  come from `tools\prep`'s action study, which renders the action at each travel and lever angle
  (`build\preview\travel_*.png`, `safe_*.png`) and prints where the handle meets the lever.
- **Charging handle grab point:** the box you grab to rack the gun (the receiver's `ChargingHandle`, a
  `GunHandleComponent` with the hand poses for racking attached under it) sits on the Saiga's knob. On
  the AK-74M it is 3.3 mm away, on the AK-74M's knob. The box keeps its size (3 × 6 × 3 cm).
  - The handle keeps two unreflected copies of its own:
    - where its travel starts, at +0x730. The plugin moves it with the box, or the box would go back to the
      AK-74M's spot on the first pull.
    - the receiver's handle settings, at +0x65C, with its travel at +0x660 and its safe stop at +0x670.
      The plugin sets them to the carrier's 12 cm and the lever's 6.1 cm.
  - Without those, the box and your hand would travel 15 cm while the knob travels 12, so at a full pull
    your hand would be 3 cm behind the knob.
  - The plugin finds both copies by their values (the AK-74M's spot, and the settings' numbers), not by
    offset. If the game ever puts the box back on the AK-74M's line, the plugin moves it onto the
    Saiga's again and logs it.
- **Magazines:** magazines in a Saiga, and the spare magazines you carry while you have one, become the
  Sb.5: its mesh, 5 rounds, the 12 gauge round.
- **Magazine grip:** the hand you take a magazine by (its `Magazine Hand Socket`) is posed for the
  AK-74M's slim, curved magazine: a fist it passes through. The Sb.5 is too deep for that fist, and its
  fingers sank 2 cm in. On an Sb.5 the plugin re-poses the socket:
  - the fingers at 80% of their curl, wrapped round the Sb.5's front;
  - the hand rolled 22° about its palm, so the magazine sits in your hand close to how the AK-74M's does;
  - the palm, thumb and middle to little fingers 3–4 mm off it, and the thumb tip 1.5 cm under its top.

  The game mirrors the same socket for the other hand, which fits the same way. The game puts your hand
  on its own IK target from the socket, with the player's `ProxyHandSocketOffset`, so the fit was measured
  on the player's own body hand. `tests\SaigaDev` grips a spare Sb.5 from a pouch the way the game does
  (`[Test] GripOnly = true`), and `[Test] MagGripSearch = true` measures each hand at several curls for a
  new search.
- **12 gauge:** the game's own 12 gauge round, so 12 gauge shells sit in the chamber and come out of the
  ejection port. Fire rate (450 rpm), recoil and shot sound come from the game's Benelli M4.
- **Slugs:** the game only has 9-pellet buckshot. `AGunReceiver::FireRoundLocal` (native, hooked by byte
  signature) spawns the round from its class defaults. While a Saiga's FireRoundLocal runs, those defaults
  become a slug:
  - one 28 g, 18.5 mm projectile at 430 m/s;
  - a blunt-slug drag factor;
  - its own damage curve (250 at the muzzle, falling with speed like the game's curves);
  - never recycled from the bullet pool.

  The defaults are put back right after the shot, so every other 12 gauge gun keeps its
  buckshot. If the signature isn't found (a game update), the Saiga fires buckshot and the log says so.
  The Barrett M107A1 plugin hooks the same function: whichever loads second follows GML's jump to the
  other's detour and hooks that, so both run in turn.
- **Loadouts:** `SaveGunSetup` names a saved gun by the first `GunAsset` with the receiver's class, so a
  Saiga is saved as the AK-74M, with the Saiga's colour name (`Saiga12K`, set on every Saiga's
  `ColorComponent`). A rebuilt gun whose slot carries that name is dressed as the Saiga. A slot saved with
  the runtime asset is re-saved with the AK-74M and recorded by hash, as the Noveske does.
- **The Shotguns category:** the station menu (`WB_GunCustomization`) has a `CategoryBtn_Shotguns` under
  Assault Rifles, SMGs, Pistols and Snipers. It is wired to `ShowWeapons(Shotgun)` like the others but set
  to Hidden in the menu's layout, and nothing in the menu's script shows it. The plugin makes it Visible
  on every station menu. The game's own list for it holds its unreleased Benelli M4 (`DA_M1014`,
  `BenelliM4_C`); the plugin takes that out, so the category holds only the Saiga.
- **Multiplayer:** that colour name replicates, so every machine with the plugin dresses the gun, whoever
  picked it. The host fires every round in the game (a client's shot reaches it as `FireRoundServer`), and
  the slug hook runs there for anyone's Saiga, so the round that hits, and its damage, is a slug. Every
  other machine gets a copy of that round with only its speed, which would fly and strike as a buckshot
  pellet: the plugin gives a round of the Saiga's class fired by a player holding a Saiga the slug's mass,
  size, drag, penetration and damage curve as it arrives. A machine without the plugin sees an AK-74M
  firing buckshot. Verified in a two-player test (`Utils\MPDev`, 2026-10-13): the client's and the host's
  Saiga dressed on both machines, the host's round for the client's shot a slug (28 g, 18.5 mm, 430 m/s,
  the slug curve), and the copy on the client a slug too.

## Build and install

There is no download: the model comes from your own Escape from Tarkov export and can't be shared. Build it
from yours. `prep` also needs [retoc](https://github.com/trumank/retoc) and `UAssetParser` from the
[modding toolkit](https://github.com/RemArrow/geronimo-modding-toolkit) in a `Utils` folder next to this one (see `build.bat`).

```bat
rem 1. Assets\ from your Tarkov export (EFT_EXPORT; default Documents\Escape from Tarkov\ExportedGLTF,
rem    then E:\TARKOVE EXPORTS, then E:\)
build.bat prep
rem 2. the plugin (close the game first)
build.bat install
```

`prep` extracts the AK-74M mesh with retoc (`Utils\retoc`) and reads its points with
`Utils\UAssetParser uexppos`. It then converts the glTF with `tools\prep`, which writes:
- OBJs in the game's space;
- `<Part>_{Diffuse,Normal,ORM}.png` for `M_DefaultShader`: albedo times the material's tint, normal map
  green flipped to DirectX, and ORM with steel parts metallic as on the game's AK-74M;
- `Icon.png`;
- preview renders in `build\preview`, with the action study.

`build.bat` packs `Assets\` encrypted into the DLL (`tools\pack`, a new key each build) and compiles the
plugin and the test harness. `install` copies `Saiga12.dll` and `CREDITS.txt` to
`...\Binaries\Win64\GML\plugins\Saiga12\`. To remove the plugin, delete that folder.

## Settings: `GML\config\com.remarrow.saiga12.cfg`

| Key | Default | |
|---|---|---|
| `[Gun] Capacity` | 5 | rounds per magazine |
| `[Gun] FireRate` | 450 | rounds per minute |
| `[Gun] RecoilScale` | 1 | recoil per shot, as a share of the Benelli M4's |
| `[Slug] Enabled` | true | false = the game's 9-pellet buckshot |
| `[Slug] Damage` | 250 | at the muzzle (7.62x39 does 95, one buckshot pellet 35) |
| `[Slug] Velocity` | 430 | m/s |
| `[Slug] Mass` | 28 | grams |
| `[Visuals] OffsetX/Y/Z` | 0 | cm, a nudge on top of the fitted position |
| `[Visuals] FitAttachments`, `OwnMovingParts`, `OwnMagazines` | true | |
| `[Spawner] ShotgunCategory` | true | show the station's Shotguns category, with the Saiga in it |

## Testing

`tests\SaigaDev` (built, never installed) loads the team room and runs these checks:
1. Presses the Saiga's spawner item through the game's own button handler.
2. Holds the gun in the air and logs its components, magazine, rate, recoil and sound.
3. Fires it through `FireRoundLocal` and logs every round that spawns, plus the 12 gauge defaults before
   and after.
4. Pulls the charging handle through its own `SetProgress` (half, full, home), and logs where the carrier,
   the bolt, the grab box and its hand poses end up against the Saiga's knob.
5. Puts it on safe through the game's own `CheckSafeLock`, pulls the handle all the way, and logs where it
   stops.
6. Puts the game's XR hand (`SKM_MannyXR_right`) on each hand socket the gun and its magazine have:
   - the four on the charging handle;
   - the magazine's;
   - the primary and support grips.

   Each hand is placed and posed as VRExpansion does it (`GetMeshRelativeTransform`,
   `GetBlendedPoseSnapShot`), with a left hand mirrored as the game mirrors it. The harness then measures
   how far each finger (to its tip) and the palm reach into the Sb.5, and renders each hand from four sides.
   This XR hand isn't the hand the game shows you, so its magazine numbers ("HAND mag") say little;
   step 9 measures the real one.
7. Saves the gun with `SaveGunSetup` and rebuilds it with `LoadGunSetups`, as an operation does.
8. Renders the gun with its own scene capture, showing only the gun.
9. Takes a spare Sb.5 from a pouch with the player's left hand, then the right, as a VR grip does
   (`GripObjectByInterface` with the socket). It measures the body's own hand bones against the Sb.5 and
   renders the hand from around itself and from the magazine. The controller is held 25 cm forward,
   30 cm down and 15 cm out from the head (`[Test] GripHold`). Further out, the body's arm can't reach
   its IK target and the hand lands centimetres off where it sits in VR. The log's `REACH` lines show
   the hand bone 0.0 cm from its target.

Verified in game (2026-10-09):
- **Spawning:** the spawner item spawns an AK-74M that is dressed with all 4 moving parts.
- **Gun:** 12 gauge class, 450 rpm, the Benelli's recoil (14, 7, 1) / (15, 0.1, 1) and shot sound, and
  12 gauge shells for the chamber proxy.
- **Magazine:** the Sb.5 with capacity 5, ammo 5, 12 gauge.
- **Slugs:** each shot is one round, 0.028 kg, 1.85 cm, 430 m/s, with the slug curve and no pooling. The
  defaults are buckshot again afterwards (Shotgun, 9 pellets, 3.5 g, `Dmg_12G_Buck`, pooling on).
- **Own action:** all 5 parts are posed by the plugin.
  - Selector (2026-10-10): safe, auto and semi at 20°, 8° and 0° (the logged pitch; up is positive). In
    close-ups, on safe the lever closes the slot under the dust cover; on auto the slot is open.
  - Safe stop (2026-10-10): on safe the game holds the handle at 0.511, so the carrier stops at −6.13 cm,
    against the lever.
  - Trigger: −10° when pulled.
  - Mag release: 12° while pressed.
  - Bolt: at 0.84 cm of carrier travel it is 29° into its turn and hasn't moved back. With the carrier at
    12 cm (2026-10-10), the bolt is at 11.1 cm and turned 35°.
  - Everything returns home afterwards.
  - Charging handle grab point (2026-10-10): the box is on the Saiga's knob spot (0.0 mm off) in all of
    these states: at rest, half back (carrier −6), at the safe stop (−6.13), fully back (−12), back home,
    and on a gun rebuilt from a loadout. The hand poses under it move with it.
- **Hands (2026-10-10):** every hand socket clears the Sb.5.
  - The magazine's grip on the player's own hand clears it by 3 mm, the left and right hands alike
    (index finger 12 mm). The game's own grip was 2 cm in. The first fit, measured with an arm that couldn't
    reach, sat 2 cm too far forward: its fingers were 2–2.5 cm in front of the Sb.5.
  - The charging handle's four sockets clear it by 9–18 mm.
  - The primary grip clears it by 9 mm, the support grip by 55 mm.
- **Loadouts:** a saved Saiga comes back as a Saiga.
- **Looks:** the in-game renders show the Saiga's geometry and textures.
- **Shotguns category:** visible on all 6 team-room station menus, labelled "SHOTGUNS" (the game's own
  text). Its page lists only the Saiga-12K; the Benelli M4 is gone from it.

Not checked by hand in VR: how the Saiga handles and fires in your hands, racking it with a real hand
(the test pulls the handle through the game's `SetProgress`), the magazine rock-in (the Sb.5 rides the
AK-74M magazine's insertion path), muzzle devices and side mounts on it, and multiplayer.

## Known limitations

- It's an AK-74M underneath: your spare AK-74M magazines become 5-round 12 gauge magazines while you carry a
  Saiga.
- Repainting the Saiga at the station doesn't stick, because the colour name marks it as a Saiga.
- The Sb.5 is solid, so the rounds inside aren't drawn.
