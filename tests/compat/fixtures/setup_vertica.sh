#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Create the `vertica` matrix entry's database inside a running vertica container.
#
#     tests/compat/fixtures/setup_vertica.sh [container] [db-name]
#
# defaults: container compat-vertica-1 (what the compose service is named), database
# VMart -- pass the name if the container was started by hand.  Idempotent in the sense that
# re-running it on a container that already has the database is a no-op that reports so;
# to start over, remove the container and bring it up again.
#
# Why this is not just an entrypoint on the compose service: opentext/vertica-k8s is
# built for the VerticaDB Kubernetes operator, which does all of the below from outside
# the pod, so the image ships a server with no database and no way to make one on its
# own.  Standing one up by hand takes four steps:
#
#   1. The image's /etc/passwd has no `dbadmin` even though every file it ships is owned
#      by uid 997 / gid 995 and the server refuses to run as anyone else, so the account
#      has to be recreated.  (This is why the compose service runs as root: it is the
#      only user that can add one.  The server itself then runs as dbadmin.)
#   2. The node management agent -- the local agent `vcluster` drives, in place of the
#      admintools/SSH of the pre-24.x images -- will not start without a TLS key pair
#      and a CA to chain it to, which the operator would normally mount in.  A
#      self-signed set generated in place is all it wants; nothing outside the container
#      ever presents these, and the SQL port (5433) does not use them.
#   3. `vcluster create_db` builds the catalog and starts the node.
#   4. It then polls the server's *HTTPS* service (8443) to confirm the node is up, and
#      that service has no certificate of its own -- the image's httpstls.json ships with
#      an empty key and certificate -- so the poll fails the TLS handshake ("no shared
#      cipher") and create_db reports failure after the database is already running.  The
#      wait below watches the SQL port instead, which is the only one the matrix uses,
#      and --startup-timeout 30 keeps create_db from spending its default 300 seconds on
#      a poll that cannot succeed.
#
# The CE licence step is a real version ceiling, not a workaround: Vertica 26.1 dropped
# Community Edition ("CE license is deprecated and no longer supported"), and bootstrap
# fails outright on 26.x with the licence this image ships.  Hence the 25.3 tag pinned in
# docker-compose.yml.
set -euo pipefail

C=${1:-compat-vertica-1}
DB=${2:-VMart}

if docker exec -u dbadmin "$C" /opt/vertica/bin/vsql -h 127.0.0.1 -U dbadmin -d "$DB" \
      -c 'SELECT 1' >/dev/null 2>&1; then
  echo "$C: database $DB is already up"
  exit 0
fi

# 1. dbadmin (uid 997 / gid 995), the owner of everything the image ships.
docker exec "$C" bash -c '
set -e
groupadd -g 995 verticadba 2>/dev/null || true
useradd -u 997 -g 995 -d /home/dbadmin -s /bin/bash dbadmin 2>/dev/null || true
chown -R dbadmin:verticadba /home/dbadmin /opt/vertica/config /opt/vertica/log
install -d -o dbadmin -g verticadba /home/dbadmin/data'

# 2. A self-signed CA plus the key pair the node management agent looks for, then the
#    agent itself (it listens on 5554, inside the container only).
docker exec -u dbadmin "$C" bash -c '
set -e
IP=$(grep -v "^127\|^::\|^fe00\|^ff0" /etc/hosts | awk "NF{print \$1}" | head -1)
HN=$(awk -v ip="$IP" "\$1==ip{print \$2}" /etc/hosts | head -1)
cd /opt/vertica/config/https_certs
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout rootca.key -out rootca.pem -subj "/CN=adbc-vertica-ca" 2>/dev/null
printf "[req]\ndistinguished_name=dn\n[dn]\n[ext]\nsubjectAltName=DNS:%s,DNS:localhost,IP:%s,IP:127.0.0.1\nextendedKeyUsage=serverAuth,clientAuth\n" \
  "$HN" "$IP" > san.cnf
for n in vertica_https dbadmin; do
  openssl req -newkey rsa:2048 -nodes -keyout $n.key -out $n.csr -subj "/CN=$n" 2>/dev/null
  openssl x509 -req -in $n.csr -CA rootca.pem -CAkey rootca.key -CAcreateserial \
    -out $n.pem -days 3650 -sha256 -extfile san.cnf -extensions ext 2>/dev/null
done
chmod 600 *.key
/opt/vertica/bin/manage_node_agent.sh start node_management_agent >/dev/null'

# The agent needs a moment to bind before create_db's first health check.
for _ in $(seq 30); do
  docker exec "$C" bash -c 'ss -ltn 2>/dev/null | grep -q :5554' && break
  sleep 1
done

# 3. Build the catalog and start the node.  create_db addresses the node by the address
#    it will bind, so pass the container's own IP rather than 127.0.0.1.
IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$C")
echo "$C: creating database $DB on $IP (a few minutes)"
docker exec -u dbadmin "$C" /opt/vertica/bin/vcluster create_db \
  --db-name "$DB" --hosts "$IP" \
  --catalog-path /home/dbadmin/data --data-path /home/dbadmin/data \
  --license /opt/vertica/config/licensing/vertica_community_edition.license.key \
  --password '' --skip-package-install --startup-timeout 30 2>&1 | grep -v 'in progress' || true

# 4. create_db's own "node is up" poll goes over HTTPS and cannot succeed here, so wait
#    on the SQL port -- which is what the matrix connects to -- instead.
echo "$C: waiting for SQL on 5433"
for _ in $(seq 300); do
  if docker exec -u dbadmin "$C" /opt/vertica/bin/vsql -h 127.0.0.1 -U dbadmin -d "$DB" \
        -c 'SELECT version()' 2>/dev/null | grep -q Vertica; then
    docker exec -u dbadmin "$C" /opt/vertica/bin/vsql -h 127.0.0.1 -U dbadmin -d "$DB" \
      -tAc 'SELECT version()'
    echo "$C: ready"
    exit 0
  fi
  sleep 1
done
echo "$C: database $DB did not come up" >&2
exit 1
