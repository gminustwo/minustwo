#!/bin/sh
# shadNet reads its settings and database from the folder its program sits in.
# Run it from /data so settings, accounts and scores live on the persistent volume.
set -e
cd /data
cp -f /opt/shadnet/shadnet /data/shadnet
mkdir -p db
for f in shadnet.cfg worlds.cfg scoreboards.cfg clientversions.cfg; do
    [ -f "$f" ] || cp "/opt/shadnet/defaults/$f" "$f"
done
exec /data/shadnet "$@"
