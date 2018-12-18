/**
 * @file rp_dt_context_helper.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief RP datatree context helper functions.
 *
 * @copyright
 * Copyright 2016 Cisco Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <setjmp.h>
#include <cmocka.h>

#include "test_data.h"
#include "request_processor.h"
#include "rp_internal.h"

#include "notification_processor.h"
#include "persistence_manager.h"

void
test_rp_ctx_create(cm_connection_mode_t conn_mode, rp_ctx_t **rp_ctx_p)
{
    int rc = SR_ERR_OK;
    rp_ctx_t *ctx = NULL;

    ctx = calloc(1, sizeof(*ctx));
    assert_non_null(ctx);

    ctx->do_not_generate_config_change = true;

    rc = ac_init(TEST_DATA_SEARCH_DIR, &ctx->ac_ctx);
    assert_int_equal(SR_ERR_OK, rc);

    rc = np_init(ctx, TEST_INTERNAL_SCHEMA_SEARCH_DIR, TEST_DATA_SEARCH_DIR, &ctx->np_ctx);
    assert_int_equal(SR_ERR_OK, rc);

    rc = pm_init(ctx, TEST_INTERNAL_SCHEMA_SEARCH_DIR, TEST_DATA_SEARCH_DIR, &ctx->pm_ctx);
    assert_int_equal(SR_ERR_OK, rc);

    rc = dm_init(ctx->ac_ctx, ctx->np_ctx, ctx->pm_ctx, conn_mode, TEST_SCHEMA_SEARCH_DIR, TEST_DATA_SEARCH_DIR, &ctx->dm_ctx);
    assert_int_equal(SR_ERR_OK, rc);

    *rp_ctx_p = ctx;
}

void
test_rp_ctx_cleanup(rp_ctx_t *ctx)
{
    pm_cleanup(ctx->pm_ctx);
    np_cleanup(ctx->np_ctx);
    ac_cleanup(ctx->ac_ctx);
    dm_cleanup(ctx->dm_ctx);
    free(ctx);
}

void
test_rp_session_create(rp_ctx_t *rp_ctx, sr_datastore_t datastore, rp_session_t **rp_session_p)
{
    rp_session_t *session = NULL;
    ac_ucred_t *credentials = NULL;
    char *user = getenv("USER");
    int rc = SR_ERR_OK;

    credentials = calloc(1, sizeof(*credentials));

    credentials->r_username = user ? strdup(user) : NULL;
    credentials->r_uid = getuid();
    credentials->r_gid = getgid();

    rc = rp_session_start(rp_ctx, 123456, credentials, datastore, SR_SESS_DEFAULT, 0, &session);
    assert_int_equal(SR_ERR_OK, rc);

    *rp_session_p = session;
}

void
test_rp_session_create_with_options(rp_ctx_t *rp_ctx, sr_datastore_t datastore,
        uint32_t options, rp_session_t **rp_session_p)
{
    rp_session_t *session = NULL;
    ac_ucred_t *credentials = NULL;
    char *user = getenv("USER");
    int rc = SR_ERR_OK;

    credentials = calloc(1, sizeof(*credentials));

    credentials->r_username = user ? strdup(user) : NULL;
    credentials->r_uid = getuid();
    credentials->r_gid = getgid();

    rc = rp_session_start(rp_ctx, 123456, credentials, datastore, options, 0, &session);
    assert_int_equal(SR_ERR_OK, rc);

    *rp_session_p = session;
}

void
test_rp_session_create_user(rp_ctx_t *rp_ctx, sr_datastore_t datastore, const ac_ucred_t user_credentials,
        uint32_t options, rp_session_t **rp_session_p)
{
    rp_session_t *session = NULL;
    ac_ucred_t *credentials = NULL;
    int rc = SR_ERR_OK;

    credentials = calloc(1, sizeof(*credentials));

    credentials->r_username = strdup(user_credentials.r_username);
    credentials->r_uid = user_credentials.r_uid;
    credentials->r_gid = user_credentials.r_gid;
    credentials->e_username = user_credentials.e_username ? strdup(user_credentials.e_username) : NULL;
    credentials->e_uid = user_credentials.e_uid;
    credentials->e_gid = user_credentials.e_gid;

    rc = rp_session_start(rp_ctx, 123456, credentials, datastore, options, 0, &session);
    assert_int_equal(SR_ERR_OK, rc);

    *rp_session_p = session;
}

void
test_rp_session_cleanup(rp_ctx_t *ctx, rp_session_t *session)
{
    if (NULL != session) {
        free((void *)session->user_credentials->r_username);
        free((void *)session->user_credentials->e_username);
        free((void*)session->user_credentials);
        rp_session_stop(ctx, session);
    }
}
