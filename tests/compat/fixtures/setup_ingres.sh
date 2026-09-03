#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Start the open-source Ingres 10.1 instance the `ingres` matrix entry talks to, and
# create its `adbc` database.
#
#     tests/compat/fixtures/setup_ingres.sh [container] [net-user]
#
# defaults: container compat-ingres-1 (what the compose service is named), net-user
# `id -un` -- the local account the matrix will run as.  Idempotent: re-running it on a
# container whose Ingres is already up skips the start and only re-checks the database.
#
# Why this is not just an entrypoint on the compose service: the iidbdb/ingres image
# bakes a *built* Ingres install (products tm,dbms,net,ome,das,odbc from the GPL
# ingres-10.1.0-00-NPTL source kit) at /opt/Ingres-golden/II, but its own entrypoint is a
# one-off data-migration driver that refuses to run without a /restore/<month>/copy.in of
# its owner's making.  Everything below is what that entrypoint would otherwise have
# wrapped, minus the restore:
#
#   1. Put the install back at the path it was *configured* for.  Its files/symbol.tbl
#      names /opt/ingres-src/install/II, so a symlink from there to /opt/Ingres-golden/II
#      is what makes config lookups resolve; nothing has to be rewritten.
#   2. Set `date_alias` to ansidate *before* the DBMS starts.  It is a start-up parameter,
#      and on the image's default (ingresdate) the ANSIDATE the entry's DDL asks for is
#      created as INGRESDATE -- Ingres' own date-and-time type, which the driver describes
#      SQL_TYPE_TIMESTAMP and whose NULL comes back as the "empty date" (year 0) rather
#      than a NULL.
#   3. `ingstart`, which brings up the name server, the DBMS, the recovery/archiver pair
#      and -- the part this entry needs -- the Ingres/Net listener (iigcc) on TCP port
#      "II" = 21064.  The config in the image already has gcc tcp_ip status ON.
#   4. Make `ingvalidpw` setuid root and give the `ingres` account an OS password.  That
#      is what the *network* login in the client's vnode is checked against: Ingres/Net
#      validates the vnode login through ingvalidpw against the server's shadow file, and
#      the image ships that helper mode 0755 owned by `ingres`, so every remote connect
#      would otherwise fail authentication.
#   5. `createdb -n adbc`, plus an Ingres user for the *local* account the client runs as.
#      -n makes it a Unicode-enabled database, which is what lets a VARCHAR hold the
#      workload's "héllo <U+1F680>" (and is required for NCHAR/NVARCHAR at all: without
#      it, "CREATE TABLE ... NVARCHAR" is "national character data types require '-n' or
#      '-i' option with CREATEDB").  The user is needed because the vnode login
#      authenticates the *connection* while the session still runs as the caller's own OS
#      user name, and an Ingres installation rejects a user it has no iiuser row for
#      ("E_US18FF ... Your user identifier was not known to this installation").  That is
#      why the name is an argument here rather than a constant: it belongs to whoever is
#      running the matrix.
#
# Note what is *not* done: `alter user ingres with password=...`.  Setting a DBMS password
# on the installation owner of this build leaves even a local `sql iidbdb` failing E_US18FF
# -- it locks the DBA out of the instance -- and the OS password from step 4 is the one
# Ingres/Net wants anyway.
set -euo pipefail

C=${1:-compat-ingres-1}
NETUSER=${2:-$(id -un)}
II=/opt/Ingres-golden/II
ENV=". /opt/ingres-src/install/II/ingres/.ingIIsh"

if docker exec "$C" pgrep -x iigcc >/dev/null 2>&1; then
  echo "$C: Ingres is already running"
else
  docker exec "$C" bash -c "
set -e
mkdir -p /opt/ingres-src/install
ln -sfn $II /opt/ingres-src/install/II
chown -R ingres:ingres $II /opt/ingres-src
su - ingres -c '$ENV; iisetres ii.\$(hostname).dbms.\*.date_alias ansidate'
su - ingres -c '$ENV; ingstart'"
fi

# Network authentication: setuid ingvalidpw plus an OS password for `ingres`.
docker exec "$C" bash -c "
set -e
chown root $II/ingres/bin/ingvalidpw
chmod 4755 $II/ingres/bin/ingvalidpw
echo 'ingres:adbc' | chpasswd"

# The database, and an Ingres user for the account the client connects from.
docker exec "$C" bash -c "
set -e
su - ingres -c '$ENV; infodb adbc' >/dev/null 2>&1 || su - ingres -c '$ENV; createdb -n adbc'
printf 'create user \"$NETUSER\" with privileges = (all), noexpire_date;\\\\g\n' > /tmp/u.sql
su - ingres -c '$ENV; sql iidbdb' < /tmp/u.sql > /tmp/u.out 2>&1 || true
rm -f /tmp/u.sql"

docker exec "$C" bash -c "su - ingres -c '$ENV; ingstatus'"
echo "$C: ready (database adbc, Ingres/Net on 21064, net user $NETUSER)"
