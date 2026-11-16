# Putting the server on Google Cloud's free e2-micro

The account, its billing card and the project are yours to create; Google asks for a card to verify
you, and the e2-micro below stays inside the Always Free tier. Check the current free-tier terms on
Google's site before you start, and set a small budget alert (Billing > Budgets & alerts, $1) so any
charge is caught at once.

Commands run on your PC need the Google Cloud CLI (`gcloud`), signed in with `gcloud init`.

## 1. Build the image on your PC

From this folder, with Docker Desktop running:

```bash
docker compose build
docker save godzilla-shadnet:local | gzip > godzilla-shadnet.tar.gz
```

## 2. Create the machine and open its ports

The Always Free e2-micro is only free in us-west1, us-central1 and us-east1.

```bash
gcloud compute instances create godzilla-shadnet --zone=us-central1-a --machine-type=e2-micro --image-family=debian-12 --image-project=debian-cloud --boot-disk-size=30GB --boot-disk-type=pd-standard --tags=shadnet
```

```bash
gcloud compute firewall-rules create shadnet --target-tags=shadnet --allow=tcp:31313,udp:31314,udp:31316
```

## 3. Copy the image and start it

```bash
gcloud compute scp godzilla-shadnet.tar.gz relay.py godzilla-shadnet:. --zone=us-central1-a --strict-host-key-checking=no
```

```bash
gcloud compute ssh godzilla-shadnet --zone=us-central1-a --command="sudo apt-get update && sudo apt-get install -y docker.io && gunzip -c godzilla-shadnet.tar.gz | sudo docker load && sudo docker run -d --name shadnet --restart always --network host -v shadnet-data:/data --log-opt max-size=10m --log-opt max-file=3 godzilla-shadnet:local"
```

Then the relay, for players who cannot reach each other directly:

```bash
gcloud compute ssh godzilla-shadnet --zone=us-central1-a --command="sudo docker run -d --name relay --restart always --network host -v \$HOME/relay.py:/relay.py:ro python:3.12-slim python -u /relay.py"
```

Both containers restart by themselves after a crash or a reboot of the machine. Host networking lets the
server see players' real addresses, which hole punching needs. (`docker-compose.cloud.yml` describes
the same container for machines with a current Docker Compose; Debian's packaged one is too old for it.)

## 4. Give players the address

```bash
gcloud compute instances describe godzilla-shadnet --zone=us-central1-a --format="get(networkInterfaces[0].accessConfigs[0].natIP)"
```

Players put that address in the launcher's Online page. Without a static IP the address can change
when the machine is stopped and started; a free dynamic DNS name (DuckDNS, for example) pointed at
it gives players one name that keeps working.

## Updating the server

Repeat steps 1 and 3 (`sudo docker rm -f shadnet` first). Accounts and leaderboards live in the
`shadnet-data` volume and survive updates.

## Who can register

By default anyone can create an account. To keep the server invite-only, set
`RegistrationSecretKey=...` in `shadnet.cfg` inside the data volume and restart the container (`sudo docker restart shadnet`);
players then enter that key as the Registration key on the launcher's Online page. The server writes every
setting into that file on its first start, the key empty: change that line rather than adding a second one.
After a restart the server may refuse to start for about 30 seconds (its lock on `worlds.cfg` looks taken),
then it comes up by itself.

The launcher's **Copy invite code** (Online page, with the address and key filled in) makes one line for
players to paste into **Invite code**, which fills in both.

## Checking the server

From your PC, with Python:

```bash
python smoke_test.py <address> --key <registration key> --accounts smoketest-accounts.json
```

Two test players go through Random VS Play's matchmaking: sign-in, a VS room with the port's match key,
the search that finds it, joining it, the host hearing the join, the peer address lookup, and the relay.
`--accounts` keeps two test accounts (`smoketest-host`, `smoketest-join`) with random passwords in that
file and signs in to them again on every run; leave out `--key` on a server without one.

## Upkeep

- About once a month: check the machine is up and the disk is not full.
- Back up the accounts now and then: copy `db/shadnet.db` out of the volume while the container
  is stopped.
- The game connection is not encrypted. Players should use a password they use nowhere else.
