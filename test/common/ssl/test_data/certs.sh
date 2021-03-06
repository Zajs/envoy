#!/bin/bash

set -e

# Uncomment the following lines if you want to regenerate the private keys.
# openssl genrsa -out ca_key.pem 1024
# openssl genrsa -out no_san_key.pem 1024
# openssl genrsa -out san_dns_key.pem 1024
# openssl genrsa -out san_uri_key.pem 1024

# Generate ca_cert.pem.
openssl req -new -key ca_key.pem -out ca_cert.csr -config ca_cert.cfg -batch -sha256
openssl x509 -req -days 3650 -in ca_cert.csr -signkey ca_key.pem -out ca_cert.pem -extensions v3_ca -extfile ca_cert.cfg

# Generate no_san_cert.pem.
openssl req -new -key no_san_key.pem -out no_san_cert.csr -config no_san_cert.cfg -batch -sha256
openssl x509 -req -days 730 -in no_san_cert.csr -sha256 -CA ca_cert.pem -CAkey ca_key.pem -CAcreateserial -out no_san_cert.pem -extensions v3_ca -extfile no_san_cert.cfg

# Generate san_dns_cert.pem.
openssl req -new -key san_dns_key.pem -out san_dns_cert.csr -config san_dns_cert.cfg -batch -sha256
openssl x509 -req -days 730 -in san_dns_cert.csr -sha256 -CA ca_cert.pem -CAkey ca_key.pem -CAcreateserial -out san_dns_cert.pem -extensions v3_ca -extfile san_dns_cert.cfg

# Generate san_uri_cert.pem.
openssl req -new -key san_uri_key.pem -out san_uri_cert.csr -config san_uri_cert.cfg -batch -sha256
openssl x509 -req -days 730 -in san_uri_cert.csr -sha256 -CA ca_cert.pem -CAkey ca_key.pem -CAcreateserial -out san_uri_cert.pem -extensions v3_ca -extfile san_uri_cert.cfg

rm *csr
rm *srl
