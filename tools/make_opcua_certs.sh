#!/usr/bin/env bash
#
# Generate the two application instance certificates the OPC UA server needs,
# and emit them as C arrays.
#
# Two, not one: Basic256Sha256 is an RSA policy and ECC_nistP256 is an elliptic
# curve one, and a policy can only use a certificate whose key matches it. OPC
# UA Part 12 names them RsaSha256ApplicationCertificateType and
# EccNistP256ApplicationCertificateType for exactly this reason.
#
# The part that breaks connections if it is wrong is the subjectAltName URI. A
# client compares it against the server's ApplicationUri and rejects the
# certificate with BadCertificateUriInvalid when they differ - not a warning, a
# refusal. So the URI here is pinned to the server's OPCUA_APPLICATION_URI
# (examples/opcua_node/inc/opcua_server.h) and the two must be changed together.
#
# These are test certificates for bring-up and the interoperability matrix. They
# are self-signed, they carry a key that lives in the firmware image, and the
# generated files are git-ignored so no private key is committed. A product
# needs provisioned keys in the reserved settings slot
# (opcua_settings.h:144 sec_cert_slot_off), not these.
#
# Usage:  tools/make_opcua_certs.sh [output-dir]
#         default output: examples/opcua_node/src (+ inc) in this repository

set -eu

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${1:-$REPO/examples/opcua_node/src}"
INC_DIR="$REPO/examples/opcua_node/inc"

# Must match OPCUA_APPLICATION_URI and the static address in wiznet_network.c.
APP_URI="urn:WIZnet:opcua-node"
HOST_DNS="wiznet-opcua-node"
HOST_IP="192.168.11.2"
# Subject comes from the [dn] section of the generated config. Passing -subj on
# the command line is not an option here: MSYS rewrites a leading-slash argument
# into a Windows path before openssl ever sees it.
DAYS=3650

command -v openssl >/dev/null || { echo "openssl not found" >&2; exit 1; }
command -v python3 >/dev/null || PY=python || PY=python3
PY="${PY:-python3}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# The extension block. keyUsage differs between the two: an RSA key transports
# a secret (keyEncipherment/dataEncipherment), an EC key agrees one
# (keyAgreement). Offering the wrong usage is a rejection reason for strict
# clients, so they are not merged into one list.
# ---------------------------------------------------------------------------
common_ext() {
    cat <<EXT
basicConstraints = critical, CA:FALSE
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
extendedKeyUsage = critical, serverAuth, clientAuth
subjectAltName = URI:${APP_URI}, DNS:${HOST_DNS}, IP:${HOST_IP}
EXT
}

cat > "$WORK/rsa.ext" <<EXT
$(common_ext)
keyUsage = critical, digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment, keyCertSign
EXT

cat > "$WORK/ecc.ext" <<EXT
$(common_ext)
keyUsage = critical, digitalSignature, nonRepudiation, keyAgreement, keyCertSign
EXT

# openssl here is a native Windows binary and cannot open the /dev/fd paths that
# process substitution produces, so the config goes through a real file.
write_cfg() {
    cat > "$2" <<CFG
[req]
distinguished_name = dn
prompt = no
[dn]
C = KR
O = WIZnet
CN = WIZnet OPC UA Node
[v3_req]
$(cat "$1")
CFG
}

write_cfg "$WORK/rsa.ext" "$WORK/rsa.cnf"
write_cfg "$WORK/ecc.ext" "$WORK/ecc.cnf"

echo "== RSA-2048 (Basic256Sha256, Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss)"
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$DAYS" \
    -extensions v3_req -config "$WORK/rsa.cnf" \
    -keyout "$WORK/rsa_key.pem" -out "$WORK/rsa_cert.pem"

echo "== EC P-256 (ECC_nistP256)"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -sha256 \
    -days "$DAYS" -extensions v3_req -config "$WORK/ecc.cnf" \
    -keyout "$WORK/ecc_key.pem" -out "$WORK/ecc_cert.pem"

# open62541 takes DER, and the private key as PKCS#8 DER.
openssl x509 -in "$WORK/rsa_cert.pem" -outform DER -out "$WORK/rsa_cert.der"
openssl pkcs8 -topk8 -nocrypt -in "$WORK/rsa_key.pem" -outform DER -out "$WORK/rsa_key.der"
openssl x509 -in "$WORK/ecc_cert.pem" -outform DER -out "$WORK/ecc_cert.der"
openssl pkcs8 -topk8 -nocrypt -in "$WORK/ecc_key.pem" -outform DER -out "$WORK/ecc_key.der"

echo
echo "== subjectAltName check (must contain the ApplicationUri)"
openssl x509 -in "$WORK/rsa_cert.pem" -noout -text | grep -A1 "Subject Alternative Name"
openssl x509 -in "$WORK/ecc_cert.pem" -noout -text | grep -A1 "Subject Alternative Name"

"$PY" - "$WORK" "$SRC_DIR/opcua_certs.c" "$INC_DIR/opcua_certs.h" "$APP_URI" <<'PYEOF'
import sys, os, datetime

work, out_c, out_h, app_uri = sys.argv[1:5]

blobs = [
    ("opcua_cert_rsa",     "rsa_cert.der", "RSA-2048 certificate (Basic256Sha256 and the other RSA policies)"),
    ("opcua_key_rsa",      "rsa_key.der",  "RSA-2048 private key, PKCS#8 DER"),
    ("opcua_cert_ecc",     "ecc_cert.der", "EC P-256 certificate (ECC_nistP256)"),
    ("opcua_key_ecc",      "ecc_key.der",  "EC P-256 private key, PKCS#8 DER"),
]

banner = (
    "/*\n"
    " * GENERATED FILE - do not edit, and do not commit.\n"
    " *\n"
    " * tools/make_opcua_certs.sh produced this on %s.\n"
    " * It contains PRIVATE KEYS. They are self-signed test material for\n"
    " * bring-up and the interoperability matrix, never for a shipped device:\n"
    " * anything holding this image holds the key.\n"
    " *\n"
    " * The subjectAltName URI is pinned to %s and must stay\n"
    " * equal to OPCUA_APPLICATION_URI, or clients reject the certificate with\n"
    " * BadCertificateUriInvalid.\n"
    " */\n\n" % (datetime.date.today().isoformat(), app_uri)
)

with open(out_c, "w", newline="\n") as c:
    c.write(banner)
    c.write('#include "opcua_certs.h"\n\n')
    for name, fn, desc in blobs:
        data = open(os.path.join(work, fn), "rb").read()
        c.write("/* %s - %d bytes */\n" % (desc, len(data)))
        c.write("const unsigned char %s[] = {" % name)
        for i, b in enumerate(data):
            c.write("\n    " if i % 12 == 0 else " ")
            c.write("0x%02x," % b)
        c.write("\n};\n")
        c.write("const unsigned int %s_len = sizeof(%s);\n\n" % (name, name))

with open(out_h, "w", newline="\n") as h:
    h.write(banner)
    h.write("#ifndef OPCUA_CERTS_H\n#define OPCUA_CERTS_H\n\n")
    h.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
    for name, _, desc in blobs:
        h.write("/* %s */\n" % desc)
        h.write("extern const unsigned char %s[];\n" % name)
        h.write("extern const unsigned int  %s_len;\n\n" % name)
    h.write("#ifdef __cplusplus\n}\n#endif\n\n#endif /* OPCUA_CERTS_H */\n")

for name, fn, _ in blobs:
    print("  %-18s %6d bytes" % (name, os.path.getsize(os.path.join(work, fn))))
print("\nwrote %s" % out_c)
print("wrote %s" % out_h)
PYEOF

echo
echo "These files are git-ignored. Re-run this script after cloning, or after"
echo "changing OPCUA_APPLICATION_URI or the static IP."
