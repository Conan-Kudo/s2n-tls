/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "api/s2n.h"

#include "error/s2n_errno.h"

#include "tls/s2n_connection.h"
#include "tls/s2n_config.h"
#include "tls/s2n_signature_algorithms.h"
#include "tls/s2n_tls.h"

#include "stuffer/s2n_stuffer.h"

#include "utils/s2n_safety.h"
#include "tls/s2n_async_pkey.h"

static int s2n_client_cert_verify_send_complete(struct s2n_connection *conn, struct s2n_blob *signature);

int s2n_client_cert_verify_recv(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->handshake.hashes);

    struct s2n_stuffer *in = &conn->handshake.io;
    struct s2n_signature_scheme *chosen_sig_scheme = &conn->handshake_params.client_cert_sig_scheme;

    if (conn->actual_protocol_version < S2N_TLS12) {
        POSIX_GUARD(s2n_choose_default_sig_scheme(conn, chosen_sig_scheme, S2N_CLIENT));
    } else {
        /* Verify the SigScheme picked by the Client was in the preference list we sent (or is the default SigScheme) */
        POSIX_GUARD(s2n_get_and_validate_negotiated_signature_scheme(conn, in, chosen_sig_scheme));
    }

    uint16_t signature_size;
    struct s2n_blob signature = {0};
    POSIX_GUARD(s2n_stuffer_read_uint16(in, &signature_size));
    signature.size = signature_size;
    signature.data = s2n_stuffer_raw_read(in, signature.size);
    POSIX_ENSURE_REF(signature.data);

    /* Use a copy of the hash state since the verify digest computation may modify the running hash state we need later. */
    struct s2n_hash_state hash_state = {0};
    POSIX_GUARD(s2n_handshake_get_hash_state(conn, chosen_sig_scheme->hash_alg, &hash_state));
    POSIX_GUARD(s2n_hash_copy(&conn->handshake.hashes->hash_workspace, &hash_state));

    /* Verify the signature */
    POSIX_GUARD(s2n_pkey_verify(&conn->handshake_params.client_public_key, chosen_sig_scheme->sig_alg, &conn->handshake.hashes->hash_workspace, &signature));

    /* Client certificate has been verified. Minimize required handshake hash algs */
    POSIX_GUARD(s2n_conn_update_required_handshake_hashes(conn));

    return S2N_SUCCESS;
}

int s2n_client_cert_verify_send(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->handshake.hashes);

    S2N_ASYNC_PKEY_GUARD(conn);
    struct s2n_stuffer *out = &conn->handshake.io;

    struct s2n_signature_scheme *chosen_sig_scheme = &conn->handshake_params.client_cert_sig_scheme;
    if (conn->actual_protocol_version < S2N_TLS12) {
        POSIX_GUARD(s2n_choose_default_sig_scheme(conn, chosen_sig_scheme, S2N_CLIENT));
    } else {
        POSIX_GUARD(s2n_stuffer_write_uint16(out, conn->handshake_params.client_cert_sig_scheme.iana_value));
    }

    /* Use a copy of the hash state since the verify digest computation may modify the running hash state we need later. */
    struct s2n_hash_state hash_state = {0};
    POSIX_GUARD(s2n_handshake_get_hash_state(conn, chosen_sig_scheme->hash_alg, &hash_state));
    POSIX_GUARD(s2n_hash_copy(&conn->handshake.hashes->hash_workspace, &hash_state));

    S2N_ASYNC_PKEY_SIGN(conn, chosen_sig_scheme->sig_alg, &conn->handshake.hashes->hash_workspace, s2n_client_cert_verify_send_complete);
}

static int s2n_client_cert_verify_send_complete(struct s2n_connection *conn, struct s2n_blob *signature)
{
    struct s2n_stuffer *out = &conn->handshake.io;

    POSIX_GUARD(s2n_stuffer_write_uint16(out, signature->size));
    POSIX_GUARD(s2n_stuffer_write(out, signature));

    /* Client certificate has been verified. Minimize required handshake hash algs */
    POSIX_GUARD(s2n_conn_update_required_handshake_hashes(conn));

    return S2N_SUCCESS;
}
