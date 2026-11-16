# Online VS Mode through shadNet

GODZILLA's online VS Mode (Random VS Play, 2 or 3 players) between copies of the port, through a
self-hosted [shadNet](https://github.com/shadps4-emu/shadNet) server: an open-source stand-in for the
PSN services the game uses (GPL-2.0-or-later, like the port). Nothing connects to Sony, and shadPS4's
own servers are not used: whoever wants online play runs a server, and players point the launcher at it.

The server only signs players in, matches them and keeps the leaderboards. The match itself runs
directly between the players' PCs, so a server costs next to nothing to run (a free-tier cloud machine
or a spare PC is plenty).

Without a server set, nothing changes: VS Mode stays offline as before, and `MT_ONLINE=1` (the direct
connection between two copies) still works.

## Playing

On the launcher's **Online** page:

| Field | Meaning |
|---|---|
| Invite code | Paste the one line a server's host sent you (`minustwo:…`): it fills in the server address and the registration key |
| Server address | The server everyone plays on (`host` or `host:port`, port 31313 by default) |
| Player name, Password | The account, also the name other players see (3-16 letters, digits, `-`, `_`) |
| Email (first time only) | With it, the first sign-in creates the account when the name is free |
| Registration key | Only for a private server whose host set one |
| Match group | Empty: anyone on the server. A name: only players with the same name (friends, a mod set) |

In the game: VS Mode, Random VS Play, 2 or 3 Players; one player's search creates the room and the
others find it. The game signs in when VS Mode opens. If Windows Firewall asks about `minustwo.exe`,
allow it: the match runs directly between the PCs.

No damage and Infinite atomic breath pause while the player is in an online room (lobby and match)
and come back after it. Unlock all kaiju stays on (it only changes the save).

Mods load online as offline, and nothing checks that players have the same ones. Visual mods
(reskins) are safe: a match only exchanges game state. Gameplay mods need the same set on every
player; a shared Match group keeps such players together.

## Hosting a server

`server/` builds shadNet in a container set up for GODZILLA (`worlds.cfg` puts CUSA02046 in its own
matchmaking group) and runs it:

```bash
cd online/server
docker compose up -d --build
python smoke_test.py
```

The smoke test takes two test players through Random VS Play's matchmaking: sign-in, a VS room with
the match key, the search that finds it (and not with another group's key), joining, the room event,
the peer address lookup and the relay. For a server people use, see `server/DEPLOY.md` (Checking the
server): `--key` and `--accounts`. The compose file also starts `relay.py` (Python standard library only), which
carries the matches of players who cannot reach each other directly. Players need TCP 31313 and
UDP 31314 and 31316. On a Linux cloud machine run it with host
networking (the server must see players' real addresses): `server/DEPLOY.md` walks through Google
Cloud's free e2-micro. Leaderboard boards are created with default settings by their first score;
`scoreboards.cfg` can set their rank limits and sort orders.

To invite players, fill in your server's address and registration key on the Online page and click
**Copy invite code**: it copies one line, `minustwo:<address>/<registration key>` (just
`minustwo:<address>` for a server without a key), to send them. They paste it into **Invite code**.
The key in it is what lets them create an account, so send it only to people you want on the server.

## Settings (environment)

| Variable | Meaning |
|---|---|
| `MT_SHADNET_SERVER` | `host[:port]` of the server; set: online play through shadNet |
| `MT_SHADNET_USER`, `MT_SHADNET_PASSWORD` | the account |
| `MT_SHADNET_EMAIL` | first sign-in registers the account when the name is free |
| `MT_SHADNET_SECRET` | the server's registration key |
| `MT_SHADNET_GROUP` | match group |
| `MT_SHADNET_UDP_PORT` | the server's signaling port (31314) |
| `MT_SHADNET_COMID` | the NP communication ID for leaderboards (`NPWR08361_00`) |
| `MT_P2P_PORT` | this player's UDP port for matches (any free one by default) |
| `MT_SHADNET_RELAY_PORT` | the relay's UDP port on the server (31316) |
| `MT_P2P_FORCE_RELAY=1` | send every match through the relay (testing) |
| `MT_ONLINE_DEBUG=1` | detailed log: room data, events handed to the game, first Rudp traffic |
| `MT_ISA_REWRITE=0` | no instruction rewrite, so no exact math either (testing: such a copy only meets its like) |

## How it works

- `src/runtime_shadnet.c`: the client. One TCP connection (shadNet's framing, a small protobuf
  encoder/decoder), a reader thread matching replies to requests and handing room notifications on,
  sign-in with first-time registration, a keep-alive every 20 s (idle connections are dropped; a
  server that does not answer it within 15 s counts as gone).
- `src/runtime_np.c`: NpMatching2 calls become shadNet commands, sent from a job thread; the room the
  player is in is mirrored from replies and room events, and the existing builders lay it out in the
  PS4 structures. A match key (searchable int attribute 0x53, which GODZILLA leaves free) carries the
  online protocol (`MT_ONLINE_PROTOCOL`), the game's version (APP_VER of the folder the game runs,
  with its update) and the match group: rooms carry it, searches filter on it,
  and the game never sees it. When the connection to the server is lost, the server has taken the
  player out of the room: the game hears its room destroyed by a connection error and the other
  players' signaling goes dead, as when the direct link to a host closes. The next request signs in
  again, and the matching contexts the game still has are started again on the server (shadNet takes
  a player out of a room on a disconnect only for a session that started one).
- `src/runtime_p2p.c`: the P2P sockets and Rudp. One real UDP socket per player; it learns its public
  address from the server's STUN service, players announce their local addresses in shadNet room
  messages, and probes punch through home routers (or go straight across a shared LAN). An opponent
  that has not answered after 5 seconds (strict NAT on both sides) is reached through the server's
  relay instead (`server/relay.py`): both players register a token derived from their room and member
  numbers, and the relay passes their packets to each other, a pair at a time. The game
  sees member N at 10.77.0.N. Once connected, players keep probing each other every second, which
  also keeps home routers' port mappings open while the lobby sends nothing; one silent for 10 s
  (game closed, PC or network gone) is reported gone: its signaling goes dead and its Rudp
  connections fail, as on a PS4. Rudp contexts are reliable ordered message streams (sequence
  numbers, acknowledgements, resends) with the context events and the SOCKET_RELEASED event the game
  uses; a receiver keeps at most 256 messages ahead of the next one it expects and 1024 the game has
  not read yet, and anything beyond waits for the sender's resend.
- `src/runtime_score.c`: NpScore (leaderboards) through shadNet's score commands.
- Exact math (`scripts/isa_rewrite.py`, `src/isa_rewrite.c`): each copy in a match computes the whole
  fight from both players' input, so both must compute it alike. The game normalizes its vectors with
  reciprocal estimates (`vrsqrtps`, 538 places, and `vrcpps`, 106), whose last bits differ between CPU
  makers and under Rosetta 2: every PS4 has the same CPU, but a Ryzen and a Mac drift apart until one
  copy ends the match while the other still shows both players' health. With a server set (or
  `MT_ONLINE=1`), the loader rewrites each one to the exact value (square root and division, correctly
  rounded on every CPU) for the whole session, offline modes included. The scan needs `iced-x86`:
  packaged builds bring it, and `build.sh` installs it into `out/pydeps` for a launcher started from
  the repository. A copy that cannot rewrite (no `iced-x86`, or a changed game) says so in its log and
  gets its own match key, so it only meets copies like it.

What GODZILLA needed beyond a plain shadNet client:

- The player who changes room or member data also gets the update event (the lobby shows each
  player's state from these events, and the host starts the match from its own room data update).
- When the host updates its own member data, it hears the other players' data again: the host starts
  a match when it sees another player ready after becoming ready itself, so a player who was ready
  first would otherwise not be seen.
- Rudp reports FLUSHED only when written data was delivered (an early one leaves the joiner stuck
  on the loading screen) and SOCKET_RELEASED after a terminate (the game waits up to 30 s for it
  before leaving the match).
- A copy that closes its last P2P socket tells the others, whose games then see the signaling
  connection go dead, as on a PS4 after a match.
- Socket addresses carry the P2P virtual port in network byte order.
- Joining a room starts P2P without the last room's players. When the host leaves as the match
  loads, GODZILLA searches again without leaving the room it was in, and member ids repeat from room
  to room (17, 34, 51 in join order): a player marked gone there was taken for the newcomer with its
  id, never probed, and the next 3-player match timed out.

## Tested

Two copies on one PC, against a local server and against a Google Cloud e2-micro: sign-in and
registration, Random VS Play for 2 players, the lobby in either ready order, matches to the result
screen and back, the leaderboard screen. The relay was tested on its own over the internet (two
players' packets passed both ways, a third party refused). Between a Windows PC (AMD Ryzen) and a Mac
(Apple silicon), before exact math: matches drifted apart (one copy ended the match while the other
still showed both players with health). With exact math, a 3-player match between a Steam Deck, a
Windows PC and a Mac on one home network (all on GODZILLA 1.01, the Deck hosting) played through to
the end. Not tested yet: a full match through the relay, Friend VS Play.

Unit tests (`ninja -C out/gpu shadnet-test p2p-test np-shadnet-test` on Windows and macOS,
`build.sh --test` on Linux): the client against a scripted server (a server that closes the
connection or stops answering, signing in again, a refused sign-in), P2P against a scripted player
(keep-alive, a player going silent, the Rudp receive limits, a player gone in the last room joining
the next one with the same member id) and the matchmaking side of a lost
server. Losing an opponent or the server has not been tried with the game yet.

## Limits

- The connection to the server is not encrypted (shadNet's own port): players should use a
  password unique to the server.
- Relayed matches (strict NAT on both sides) add the round trip to the relay: with one relay, players
  far from it feel it. Direct matches are unaffected. Relays in more regions are a later step.
- Every player runs this port: PS4 players and shadPS4 users are not matched.
- Players need builds with the same online protocol (`MT_ONLINE_PROTOCOL`, 3 since the game version is in
  the match key): older copies do not find newer ones' rooms.
- Players only meet players with the same game version: everyone with the 1.01 update (a game folder
  that includes it, or `MT_PATCH_DIR`) plays together, and a 1.00 copy only finds 1.00 rooms. Copies of
  different versions cannot play each other, and no setting can change that: each copy computes the
  whole fight, and 1.01 changed the game's code and its gameplay data (`game_*.pac`, `system.pac`) and
  added kaiju 8, Godzilla (1964), whose data (`ch08a_00.pac`) a 1.00 copy does not have (before the
  version was in the match key, a 1.00 copy that joined a 1.01 3-player room crashed loading it). The launcher's
  Online page says which version the game folder is, and the log says it when VS Mode opens
  (`Online: match group ... game version 01.01`).
