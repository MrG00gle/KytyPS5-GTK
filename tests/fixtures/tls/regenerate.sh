#!/bin/bash
# Writes the TLS test fixtures of tests/launcher_gtk/SoupHttpClientTests.cpp (phase 7 plan D12)
# into this directory with openssl, valid for 100 years from the day they are made:
#   ca-cert.pem      a test certificate authority (its private key is thrown away)
#   server-cert.pem  a server certificate for 127.0.0.1 and localhost, signed by that authority
#   server-key.pem   the server certificate's private key (test only, unencrypted)
# Nothing trusts the authority except the test, which loads ca-cert.pem as its own GTlsDatabase.
# Run it again only when the fixtures must change; the committed files are its output.
set -u
here=$(cd "$(dirname "$0")" && pwd) || exit 1
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
days=36500

cat > "$work/ca.cnf" <<'EOF'
[req]
distinguished_name = dn
prompt = no
x509_extensions = ca
[dn]
CN = KytyPS5-GTK test CA
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
EOF
cat > "$work/server.cnf" <<'EOF'
[req]
distinguished_name = dn
prompt = no
[dn]
CN = 127.0.0.1
[server]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = IP:127.0.0.1, DNS:localhost
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
EOF

openssl req -x509 -new -newkey rsa:2048 -nodes -sha256 -days "$days" -config "$work/ca.cnf" \
	-keyout "$work/ca-key.pem" -out "$work/ca-cert.pem" &&
	openssl req -new -newkey rsa:2048 -nodes -sha256 -config "$work/server.cnf" \
		-keyout "$work/server-key.pem" -out "$work/server.csr" &&
	openssl x509 -req -sha256 -days "$days" -in "$work/server.csr" -CA "$work/ca-cert.pem" \
		-CAkey "$work/ca-key.pem" -CAcreateserial -extfile "$work/server.cnf" -extensions server \
		-out "$work/server-cert.pem" &&
	openssl verify -CAfile "$work/ca-cert.pem" "$work/server-cert.pem" || exit 1

cp "$work/ca-cert.pem" "$work/server-cert.pem" "$work/server-key.pem" "$here/" || exit 1
chmod 0644 "$here/ca-cert.pem" "$here/server-cert.pem" "$here/server-key.pem"
echo "regenerate.sh: wrote ca-cert.pem, server-cert.pem, server-key.pem in $here"
