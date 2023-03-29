#!/bin/bash

# Copyright (c) 2023 Cisco and/or its affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

mkdir -p /etc/sonic/telemetry
cd /etc/sonic/telemetry

openssl genrsa -out dsmsroot.key

openssl req -x509 -subj "/C=IN/CN=sonic-telemetry-ca" -key dsmsroot.key -out dsmsroot.cer -days 1000

openssl genrsa -out streamingtelemetryserver.key

DNS_NAME=${DNS_NAME:="telemetry-sonic.cisco.com"}

openssl req -new -subj "/C=IN/CN=telemetry-sonic"  -addext "certificatePolicies = 1.2.3.4" -key streamingtelemetryserver.key -out req.pem

cat > ssl.cnf <<EOF
[req]
req_extensions = v3_req
distinguished_name = req_distinguished_name
[req_distinguished_name]
[ v3_req ]
basicConstraints = CA:FALSE
keyUsage = nonRepudiation, digitalSignature, keyEncipherment
extendedKeyUsage=serverAuth,clientAuth
subjectAltName = @alt_names
[alt_names]
EOF
echo "DNS.1 = $DNS_NAME" >> ssl.cnf

openssl x509 -req -in req.pem  -CA dsmsroot.cer  -CAkey dsmsroot.key -CAcreateserial -extfile ./ssl.cnf -extensions v3_req -out streamingtelemetryserver.cer -days 365

