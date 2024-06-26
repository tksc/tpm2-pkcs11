#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause

set -xo pipefail

if [ -z "$T" ]; then
    export T="$(cd "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
fi

source "$T/test/integration/scripts/helpers.sh"

EXT_FILE="$TEST_FIXTURES/xpextensions"
CLIENT_CNF="$TEST_FIXTURES/client.cnf"

# details on the PKCS11 URI can be found here: https://tools.ietf.org/html/rfc7512
PKCS11_KEY="pkcs11:model=SW%20%20%20TPM;manufacturer=IBM;serial=0000000000000000;token=label;object=rsa0;type=private"

if [ "$ASAN_ENABLED" = "true" ]; then
  # Skip this test when ASAN is enabled
  exit 77
fi

check_openssl_version

export TPM2OPENSSL_TCTI="$TPM2TOOLS_TCTI"

function cleanup() {
  if [ "$1" != "no-kill" ]; then
      pkill -P $$ || true
  fi
   cleanup_ca
  rm -f server.csr server.crt server.key server.pem client.csr client.crt client_tpm.pem
}
trap cleanup EXIT

onerror() {
  echo "$BASH_COMMAND on line ${BASH_LINENO[0]} failed: $?"
  exit 1
}
trap onerror ERR

cleanup "no-kill"

# setup the CA BEFORE EXPORTING THE CA CONF for the clients
setup_ca

# create and sign standard file-based cert for server
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr -subj "/C=US/ST=Radius/L=Somewhere/O=Example Inc./CN=server.example.com"
openssl ca -batch -keyfile "$CA_KEY" -cert "$CA_PEM" -in server.csr -out server.crt -config "$CLIENT_CNF"
openssl x509 -in server.crt -out server.pem -outform pem

# create and sign PKCS#11 cert for client
if [ "$OSSL3_DETECTED" -eq "0" ]; then
    export OPENSSL_CONF="$TEST_FIXTURES/ossl.cnf"
    openssl req -new -engine pkcs11 -keyform engine -key "$PKCS11_KEY" -out client.csr -subj "/C=US/ST=Radius/L=Somewhere/O=Example Inc./CN=testing/emailAddress=testing@123.com"
else
    pushd $TPM2_PKCS11_STORE
    yaml_rsa0=$(tpm2_ptool export --label=label --userpin=myuserpin --key-label=rsa0 --path=$TPM2_PKCS11_STORE)
    popd

    auth_rsa0=$(echo "$yaml_rsa0" | grep "object-auth" | cut -d' ' -f2-)

    TPM2OPENSSL_PARENT_AUTH="mypobjpin" openssl req -new -provider tpm2 -provider base \
      -key "$TPM2_PKCS11_STORE/rsa0.pem" -passin "pass:$auth_rsa0" \
      -subj "/C=US/ST=Radius/L=Somewhere/O=Example Inc./CN=testing/emailAddress=testing@123.com" \
      -out client.csr
fi

openssl ca -batch -keyfile "$CA_KEY" -cert "$CA_PEM" -in client.csr -out client.crt -extensions xpclient_ext -extfile "$EXT_FILE" -config "$CLIENT_CNF"
openssl x509 -in client.crt -out client_tpm.pem -outform pem

# OpenSSL version 1.0.2g ends up in a state where it tries to read from stdin instead of the ssl connection.
# Feeding it one byte as stdin avoids this condition which is described in more detail here:
# https://github.com/tpm2-software/tpm2-pkcs11/pull/366
openssl s_server -debug -CAfile "$CA_PEM" -cert server.pem -key server.key -Verify 1 <<< '1' &
sleep 1

# default connects to 127.0.0.1:443
if [ "$OSSL3_DETECTED" -eq "0" ]; then
    openssl s_client -engine pkcs11 -keyform engine -key "$PKCS11_KEY" -CAfile "$CA_PEM" -cert client_tpm.pem <<< 'Q'
else
    TPM2OPENSSL_PARENT_AUTH="mypobjpin" openssl s_client -provider tpm2 -provider default \
      -key "$TPM2_PKCS11_STORE/rsa0.pem" -pass "pass:$auth_rsa0" -ignore_unexpected_eof \
      -CAfile "$CA_PEM" -cert client_tpm.pem <<< 'Q'
fi
exit 0
