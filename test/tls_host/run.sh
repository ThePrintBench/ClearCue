#!/usr/bin/env bash
# Host test for the firmware's TLS policy (Linux, needs gcc/g++ + openssl).
#   test/tls_host/run.sh <wolfssl source root (v5.9.4-stable)> [real-watchtower-ca.pem]
# Without the real CA (e.g. in CI) the generated replica CA is used for the anchor-load case.
# Builds wolfSSL with include/user_settings.h, generates a Watchtower-like CA (RSA-4096,
# critical Name Constraints) + leaves, serves them with openssl s_server, and runs
# tls_host_test against them.
set -euo pipefail
W="$(realpath "$1")"; REAL_CA="${2:+$(realpath "$2")}"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/bb_tls_host"; rm -rf "$OUT"; mkdir -p "$OUT/obj"; cd "$OUT"

CFLAGS=(-O1 -w -DWOLFSSL_USER_SETTINGS -iquote "$ROOT/include" -I"$W")
ls "$W"/src/*.c "$W"/wolfcrypt/src/*.c | grep -v sniffer | while read -r f; do
  gcc -c "${CFLAGS[@]}" "$f" -o "obj/$(echo "$f" | md5sum | cut -c1-12).o"
done
g++ -O1 -w "${CFLAGS[@]}" "$HERE/tls_host_test.cpp" "$ROOT/src/clearcue/tls_policy.cpp" obj/*.o -lm -o tls_host_test

# --- certificates -----------------------------------------------------------
cat > ca.cnf <<'X'
[req]
distinguished_name=dn
prompt=no
[dn]
O=Watchtower
CN=Watchtower Local CA (replica)
[ca]
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
nameConstraints=critical,permitted;DNS:local,permitted;DNS:localhost,permitted;IP:10.0.0.0/255.0.0.0,permitted;IP:172.16.0.0/255.240.0.0,permitted;IP:192.168.0.0/255.255.0.0,permitted;IP:169.254.0.0/255.255.0.0,permitted;IP:127.0.0.0/255.0.0.0,permitted;IP:::1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff
[leaf]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
subjectAltName=DNS:watchtower.local,DNS:localhost,IP:127.0.0.1,IP:::1,IP:192.168.20.14,IP:192.168.25.14
[evil]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:evil.example.com,IP:8.8.8.8,IP:127.0.0.1
X
cat > ca2.cnf <<'X'
[ca]
default_ca=c
[c]
dir=cadb
database=cadb/index.txt
serial=cadb/serial
new_certs_dir=cadb
default_md=sha256
policy=p
copy_extensions=none
[p]
commonName=supplied
organizationName=optional
X
mkdir -p cadb; : > cadb/index.txt; echo 01 > cadb/serial
openssl req -x509 -newkey rsa:4096 -nodes -keyout ca.key -out ca.pem -days 10950 -config ca.cnf -extensions ca 2>/dev/null
for n in leaf evil; do
  openssl req -new -newkey rsa:2048 -nodes -keyout $n.key -subj "/O=Watchtower/CN=watchtower.local" -out $n.csr 2>/dev/null
  openssl x509 -req -in $n.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 800 -extfile ca.cnf -extensions $n -out $n.pem 2>/dev/null
done
openssl ca -batch -config ca2.cnf -cert ca.pem -keyfile ca.key -in leaf.csr -startdate 20200101000000Z \
  -enddate 20210101000000Z -extfile ca.cnf -extensions leaf -out expired.pem >/dev/null 2>&1
openssl req -x509 -newkey rsa:2048 -nodes -keyout other.key -out other.pem -days 365 -subj "/CN=Other CA" 2>/dev/null
REAL_CA="${REAL_CA:-$OUT/ca.pem}"
echo "replica CA sha256 (openssl): $(openssl x509 -in ca.pem -noout -fingerprint -sha256 | cut -d= -f2)"

# --- servers (each serves leaf + CA, like Watchtower; one serves leaf only) --
pids=()
serve() { openssl s_server -quiet -accept "$1" -cert "$2" -key "$3" ${4:+-cert_chain "$4"} ${5:-} -www >/dev/null 2>&1 & pids+=($!); }
serve 18441 leaf.pem leaf.key ca.pem
serve 18442 evil.pem evil.key ca.pem
serve 18443 expired.pem leaf.key ca.pem
serve 18444 leaf.pem leaf.key
serve 18445 leaf.pem leaf.key ca.pem -tls1_2
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT
sleep 1
./tls_host_test 18441 18442 18443 18444 "$REAL_CA" 18445
