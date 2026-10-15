#!/usr/bin/env bash
#
# A client certificate for the interoperability tests.
#
# Separate from make_opcua_certs.sh on purpose: that one produces firmware, this
# one produces test tooling. The server's certificates are compiled into the
# device; this is a PEM pair that lives on the PC running the tests.
#
# The subjectAltName URI has to match the ApplicationUri the test client
# announces, the same rule that applies to the server's own certificate. Both
# are set here and in opcua_concurrent_test.py, and they must stay equal.
#
# Output: tools/interop_certs/{client_cert.pem, client_key.pem} - git-ignored.

set -eu

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO/tools/interop_certs"

APP_URI="urn:wiznet:interop-client"
DAYS=3650

command -v openssl >/dev/null || { echo "openssl not found" >&2; exit 1; }

mkdir -p "$OUT"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Same shape as the server certificate: an application instance certificate,
# self-signed, with the URI in the SAN. clientAuth rather than serverAuth is the
# only functional difference, and both are listed because a test client may end
# up on either side.
cat > "$WORK/client.cnf" <<CFG
[req]
distinguished_name = dn
prompt = no
[dn]
C = KR
O = WIZnet
CN = WIZnet Interop Test Client
[v3_req]
basicConstraints = critical, CA:FALSE
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
keyUsage = critical, digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment, keyCertSign
extendedKeyUsage = critical, clientAuth, serverAuth
subjectAltName = URI:${APP_URI}
CFG

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$DAYS" \
    -extensions v3_req -config "$WORK/client.cnf" \
    -keyout "$OUT/client_key.pem" -out "$OUT/client_cert.pem"

# asyncua reads DER for the certificate and PEM for the key; both are written so
# whichever the library wants is already there.
openssl x509 -in "$OUT/client_cert.pem" -outform DER -out "$OUT/client_cert.der"

echo
openssl x509 -in "$OUT/client_cert.pem" -noout -text | grep -A1 "Subject Alternative Name"
echo
echo "wrote $OUT/client_cert.pem"
echo "      $OUT/client_cert.der"
echo "      $OUT/client_key.pem"
echo
echo "ApplicationUri is $APP_URI - the test client must announce the same."
