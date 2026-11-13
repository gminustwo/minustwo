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

The smoke test registers two throwaway accounts and checks sign-in, room creation, search and the
peer address lookup. Players need TCP 31313 and UDP 31314. On a Linux cloud machine run it with host
networking (the server must see players' real addresses): `server/DEPLOY.md` walks through Google
Cloud's free e2-micro. Leaderboard boards are created with default settings by their first score;
`scoreboards.cfg` can set their rank limits and sort orders.

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
| `MT_ONLINE_DEBUG=1` | detailed log: room data, events handed to the game, first Rudp traffic |

## How it works

- `src/runtime_shadnet.c`: the client. One TCP connection (shadNet's framing, a small protobuf
  encoder/decoder), a reader thread matching replies to requests and handing room notifications on,
  sign-in with first-time registration, a keep-alive every 20 s (idle connections are dropped).
- `src/runtime_np.c`: NpMatching2 calls become shadNet commands, sent from a job thread; the room the
  player is in is mirrored from replies and room events, and the existing builders lay it out in the
  PS4 structures. A match key (searchable int attribute 0x53, which GODZILLA leaves free) carries the
  online protocol (`MT_ONLINE_PROTOCOL`) and the match group: rooms carry it, searches filter on it,
  and the game never sees it.
- `src/runtime_p2p.c`: the P2P sockets and Rudp. One real UDP socket per player; it learns its public
  address from the server's STUN service, players announce their local addresses in shadNet room
  messages, and probes punch through home routers (or go straight across a shared LAN). The game
  sees member N at 10.77.0.N. Rudp contexts are reliable ordered message streams (sequence numbers,
  acknowledgements, resends) with the context events and the SOCKET_RELEASED event the game uses.
- `src/runtime_score.c`: NpScore (leaderboards) through shadNet's score commands.

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

## Tested

Two copies on one PC, against a local server and against a Google Cloud e2-micro: sign-in and
registration, Random VS Play for 2 players, the lobby in either ready order, matches to the result
screen and back, the leaderboard screen. Not tested yet: 3-player rooms, players behind routers that
need a relay (strict NAT on both sides; there is no relay), Friend VS Play.

## Limits

- The connection to the server is not encrypted (shadNet's own port): players should use a
  password unique to the server.
- No relay: two players who both sit behind strict NAT cannot reach each other.
- Every player runs this port: PS4 players and shadPS4 users are not matched.
