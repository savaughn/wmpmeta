#!/usr/bin/env python3
"""Turn a PEM bundle into a C source file: embed_certs.py roots.pem out.c"""
import sys

pem = open(sys.argv[1]).read()
# Drop comment lines; mbedTLS only wants the PEM blocks.
body = "".join(l + "\n" for l in pem.splitlines() if not l.startswith("#") and l.strip())
with open(sys.argv[2], "w") as f:
    f.write("/* Generated from %s by tools/embed_certs.py. Do not edit. */\n" % sys.argv[1])
    f.write("const char tp_ca_pem[] =\n")
    for line in body.splitlines():
        f.write('    "%s\\n"\n' % line)
    f.write(";\n")
    # mbedtls_x509_crt_parse wants the length including the NUL for PEM.
    f.write("const unsigned tp_ca_pem_len = sizeof tp_ca_pem;\n")
