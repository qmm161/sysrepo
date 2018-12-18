/**
 * @file sysrepocfg_test.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>,
 *         Milan Lenco <milan.lenco@pantheon.tech>
 * @brief sysrepocfg tool unit tests.
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
#include <setjmp.h>
#include <cmocka.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <libyang/libyang.h>

#include "sysrepo.h"
#include "sr_common.h"
#include "test_data.h"
#include "system_helper.h"
#include "client_library.h"
#include "test_module_helper.h"
#include "module_dependencies.h"

#define FILENAME_NEW_CONFIG   "sysrepocfg_test-new_config.txt"
#define FILENAME_USER_INPUT   "sysrepocfg_test-user_input.txt"
#define MAX_SUBS              16

struct ly_ctx *srcfg_test_libyang_ctx = NULL;
static char *srcfg_test_datastore = NULL;
static sr_conn_ctx_t *srcfg_test_connection = NULL;
static sr_session_ctx_t *srcfg_test_session = NULL;

typedef struct srcfg_test_subscription_e {
    sr_subscription_ctx_t *subscription;
    char *module_name;
} srcfg_test_subscription_t;

srcfg_test_subscription_t srcfg_test_subscriptions[MAX_SUBS];


/**
 * @brief Compare data file content against a string using libyang's lyd_diff.
 */
static int
srcfg_test_cmp_data_file_content(const char *file_path, LYD_FORMAT file_format, const char *exp, LYD_FORMAT exp_format)
{
    int fd = -1;
    struct lyd_node *file_data = NULL, *exp_data = NULL;
    struct lyd_difflist *diff = NULL;
    size_t count = 0, skip_differences = 0;

    fd = open(file_path, O_RDONLY);
    assert_true_bt(fd >= 0);

    ly_errno = LY_SUCCESS;
    file_data = lyd_parse_fd(srcfg_test_libyang_ctx, fd, file_format, LYD_OPT_TRUSTED | LYD_OPT_CONFIG);
    if (!file_data && LY_SUCCESS != ly_errno) {
        fprintf(stderr, "lyd_parse_fd error: %s (%s)", ly_errmsg(srcfg_test_libyang_ctx), ly_errpath(srcfg_test_libyang_ctx));
    }
    assert_true_bt(file_data || LY_SUCCESS == ly_errno);
    if (NULL != exp) {
        ly_errno = LY_SUCCESS;
        exp_data = lyd_parse_mem(srcfg_test_libyang_ctx, exp, exp_format, LYD_OPT_TRUSTED | LYD_OPT_CONFIG);
        if (!exp_data && LY_SUCCESS != ly_errno) {
            fprintf(stderr, "lyd_parse_mem error: %s (%s)", ly_errmsg(srcfg_test_libyang_ctx), ly_errpath(srcfg_test_libyang_ctx));
        }
        assert_true_bt(exp_data || LY_SUCCESS == ly_errno);
    }

    diff = lyd_diff(file_data, exp_data, 0);
    assert_non_null_bt(diff);

    while (diff->type && LYD_DIFF_END != diff->type[count]) {
        if (NULL != diff->first[count] && LYS_ANYDATA == diff->first[count]->schema->nodetype) {
            /* LYS_ANYDATA not supported by libyang JSON printer */
            ++skip_differences;
        } else {
            char *first = lyd_path(diff->first[count]);
            char *second = lyd_path(diff->second[count]);
            printf("first: %s; second: %s\n", first, second);
            free(first);
            free(second);
        }
        ++count;
    }

    if ((count - skip_differences) > 0) {
        fprintf(stderr, "file data:\n");
        lyd_print_fd(STDERR_FILENO, file_data, LYD_XML, LYP_WITHSIBLINGS | LYP_FORMAT);
        fprintf(stderr, "exp data:\n");
        lyd_print_fd(STDERR_FILENO, exp_data, LYD_XML, LYP_WITHSIBLINGS | LYP_FORMAT);
    }

    lyd_free_diff(diff);
    if (NULL != file_data) {
        lyd_free_withsiblings(file_data);
    }
    if (NULL != exp_data) {
        lyd_free_withsiblings(exp_data);
    }

    close(fd);
    return (count - skip_differences);
}

/**
 * @brief Compare two data files using libyang's lyd_diff.
 */
static int
srcfg_test_cmp_data_files(const char *file1_path, LYD_FORMAT file1_format, const char *file2_path, LYD_FORMAT file2_format)
{
    int rc = -1, fd = -1;
    struct stat file_info = {0};
    char *file2_content = NULL;

    fd = open(file2_path, O_RDONLY);
    assert_true_bt(fd >= 0);

    assert_int_equal_bt(0, fstat(fd, &file_info));
    if (0 < file_info.st_size) {
        file2_content = mmap(0, file_info.st_size, PROT_READ, MAP_SHARED, fd, 0);
        assert_true_bt(file2_content && MAP_FAILED != file2_content);
    }

    rc = srcfg_test_cmp_data_file_content(file1_path, file1_format, file2_content, file2_format);

    if (0 < file_info.st_size) {
        assert_int_equal_bt(0, munmap(file2_content, file_info.st_size));
    }
    close(fd);
    return rc;
}

static int
srcfg_test_module_change_cb(sr_session_ctx_t *session, const char *module_name, sr_notif_event_t event,
                            void *private_ctx)
{
    return SR_ERR_OK;
}

static int
srcfg_test_subscribe(const char *module_name)
{
    int rc = SR_ERR_OK;
    unsigned i = 0;

    /* already subscribed? */
    for (i = 0; i < MAX_SUBS; ++i) {
        if (NULL != srcfg_test_subscriptions[i].subscription &&
            0 == strcmp(module_name, srcfg_test_subscriptions[i].module_name)) {
            return rc;
        }
    }

    /* find first free slot */
    i = 0;
    while (MAX_SUBS > i && NULL != srcfg_test_subscriptions[i].subscription) {
        ++i;
    }
    if (MAX_SUBS == i) {
        return SR_ERR_INTERNAL;
    }

    /* subscribe */
    rc = sr_module_change_subscribe(srcfg_test_session, module_name, srcfg_test_module_change_cb, NULL, 0,
                                    SR_SUBSCR_DEFAULT, &(srcfg_test_subscriptions[i].subscription));
    if (SR_ERR_OK == rc) {
        srcfg_test_subscriptions[i].module_name = strdup(module_name);
    }
    return rc;
}

static int
srcfg_test_unsubscribe(const char *module_name)
{
    for (unsigned i = 0; i < MAX_SUBS; ++i) {
        if (NULL != srcfg_test_subscriptions[i].subscription &&
            0 == strcmp(module_name, srcfg_test_subscriptions[i].module_name)) {
            free(srcfg_test_subscriptions[i].module_name);
            srcfg_test_subscriptions[i].module_name = NULL;
            int rc = sr_unsubscribe(srcfg_test_session, srcfg_test_subscriptions[i].subscription);
            srcfg_test_subscriptions[i].subscription = NULL;
            return rc;
        }
    }
    return SR_ERR_OK;
}

static int
srcfg_test_init_datastore_content()
{
    createDataTreeIETFinterfacesModule();
    return 0;
}

static int
srcfg_test_set_startup_datastore(void **state)
{
    createDataTreeIETFinterfacesModule();
    srcfg_test_datastore = strdup("startup");
    assert_non_null_bt(srcfg_test_datastore);
    return 0;
}

static int
srcfg_test_set_running_datastore(void **state)
{
    createDataTreeIETFinterfacesModule();
    srcfg_test_datastore = strdup("running");
    assert_non_null_bt(srcfg_test_datastore);
    return 0;
}

static int
srcfg_test_set_running_datastore_merge(void **state)
{
    createDataTreeIETFinterfacesModuleMerge();
    srcfg_test_datastore = strdup("running");
    assert_non_null_bt(srcfg_test_datastore);
    return 0;
}

static int
srcfg_test_teardown(void **state)
{
    free(srcfg_test_datastore);
    srcfg_test_datastore = NULL;
    return 0;
}

static void
srcfg_test_version(void **state)
{
    exec_shell_command("../src/sysrepocfg -v",
                       "^sysrepocfg - sysrepo configuration tool, version [0-9]\\.[0-9]\\.[0-9][0-9]*[[:space:]]*$", true, 0);
}

static void
srcfg_test_help(void **state)
{
    exec_shell_command("../src/sysrepocfg -h", "Usage:", true, 0);
}

static void
srcfg_test_export(void **state)
{
    /* invalid arguments */
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=txt ietf-interfaces > /tmp/ietf-interfaces.startup.xml", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --export=/tmp/module.startup.xml --datastore=startup --format=json", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=txt ietf-interfaces > /tmp/ietf-interfaces.running.xml", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --export=/tmp/module.running.xml --datastore=running --format=json", ".*", true, 1);

    /* export ietf-interfaces, test-module, example-module, cross-module and referenced-data in both xml and json formats */

    /* ietf-interfaces */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml ietf-interfaces > /tmp/ietf-interfaces.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "ietf-interfaces.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/ietf-interfaces.startup.json --datastore=startup --format=json ietf-interfaces", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "ietf-interfaces.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml ietf-interfaces", "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("ietf-interfaces"));
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml ietf-interfaces > /tmp/ietf-interfaces.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "ietf-interfaces.running", SR_FILE_FORMAT_LY));
    /*  running, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/ietf-interfaces.running.json --datastore=running --format=json ietf-interfaces", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "ietf-interfaces.running", SR_FILE_FORMAT_LY));

    /* test-module */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml test-module > /tmp/test-module.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "test-module.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/test-module.startup.json --datastore=startup --format=json test-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "test-module.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml test-module", "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("test-module"));
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml test-module > /tmp/test-module.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "test-module.running", SR_FILE_FORMAT_LY));
    /*  running, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/test-module.running.json --datastore=running --format=json test-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "test-module.running", SR_FILE_FORMAT_LY));

    /* example-module */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml example-module > /tmp/example-module.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "example-module.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/example-module.startup.json --datastore=startup --format=json example-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "example-module.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml example-module", "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("example-module"));
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml example-module > /tmp/example-module.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "example-module.running", SR_FILE_FORMAT_LY));
    /*  running, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/example-module.running.json --datastore=running --format=json example-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "example-module.running", SR_FILE_FORMAT_LY));

    /* cross-module */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml cross-module > /tmp/cross-module.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "cross-module.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/cross-module.startup.json --datastore=startup --format=json cross-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "cross-module.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml cross-module", "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("cross-module"));
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml cross-module > /tmp/cross-module.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "cross-module.running", SR_FILE_FORMAT_LY));
    /*  running, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/cross-module.running.json --datastore=running --format=json cross-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "cross-module.running", SR_FILE_FORMAT_LY));

    /* referenced-data */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml referenced-data > /tmp/referenced-data.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "referenced-data.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/referenced-data.startup.json --datastore=startup --format=json referenced-data", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "referenced-data.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml referenced-data", "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("referenced-data"));
    exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml referenced-data > /tmp/referenced-data.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "referenced-data.running", SR_FILE_FORMAT_LY));
    /*  running, json */
    exec_shell_command("../src/sysrepocfg --export=/tmp/referenced-data.running.json --datastore=running --format=json referenced-data", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "referenced-data.running", SR_FILE_FORMAT_LY));

    /* restore pre-test state */
    assert_int_equal(0, srcfg_test_unsubscribe("ietf-interfaces"));
    assert_int_equal(0, srcfg_test_unsubscribe("test-module"));
    assert_int_equal(0, srcfg_test_unsubscribe("example-module"));
    assert_int_equal(0, srcfg_test_unsubscribe("cross-module"));
    assert_int_equal(0, srcfg_test_unsubscribe("referenced-data"));
}

static void
srcfg_test_xpath(void **state)
{
    sr_val_t *rvalue = { 0 };
    int rc = 0;

    /* export ietf-interfaces, in both xml and json formats */

    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg -d startup -f xml -g /ietf-interfaces:*//* > /tmp/ietf-interfaces.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "ietf-interfaces.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg -d startup -f json -g /ietf-interfaces:*//* > /tmp/ietf-interfaces.startup.json", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "ietf-interfaces.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg -d running -f xml -g /ietf-interfaces:*//*", "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("ietf-interfaces"));
    exec_shell_command("../src/sysrepocfg -d running -f xml -g /ietf-interfaces:*//* ietf-interfaces > /tmp/ietf-interfaces.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "ietf-interfaces.running", SR_FILE_FORMAT_LY));
    /*  running, json */
    exec_shell_command("../src/sysrepocfg -d running -f json -g /ietf-interfaces:*//* ietf-interfaces > /tmp/ietf-interfaces.running.json", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "ietf-interfaces.running", SR_FILE_FORMAT_LY));

    /* set a string value */
    exec_shell_command("../src/sysrepocfg -s \"/ietf-interfaces:interfaces/interface[name='eth0']/description\" -w 'description eth0' --datastore=running", ".*", true, 0);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    }
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth0']/description", &rvalue);
    assert_int_equal(rc, SR_ERR_OK);
    assert_int_equal(SR_STRING_T, rvalue->type);
    assert_string_equal("/ietf-interfaces:interfaces/interface[name='eth0']/description", rvalue->xpath);
    assert_string_equal("description eth0", rvalue->data.string_val);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* set a boolean value */
    exec_shell_command("../src/sysrepocfg -s \"/ietf-interfaces:interfaces/interface[name='eth0']/enabled\" -w false --datastore=running", ".*", true, 0);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    }
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth0']/enabled", &rvalue);
    assert_int_equal(rc, SR_ERR_OK);
    assert_int_equal(SR_BOOL_T, rvalue->type);
    assert_string_equal("/ietf-interfaces:interfaces/interface[name='eth0']/enabled", rvalue->xpath);
    assert_false(rvalue->data.bool_val);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* set a leaf value */
    exec_shell_command("../src/sysrepocfg -d running -s \"/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu\" -w 1600", ".*", true, 0);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    }
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu", &rvalue);
    assert_int_equal(rc, SR_ERR_OK);
    assert_int_equal(SR_UINT16_T, rvalue->type);
    assert_string_equal("/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu", rvalue->xpath);
    assert_int_equal(1600, rvalue->data.uint16_val);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* set a not existing leaf */
    exec_shell_command("../src/sysrepocfg -d running -s \"/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/fakeleaf\" -w 'not existing leaf'", ".*", true, 1);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    };
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/fakeleaf", &rvalue);
    assert_int_equal(rc, SR_ERR_BAD_ELEMENT);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* set a leaf without a value */
    exec_shell_command("../src/sysrepocfg -d running -s \"/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu\"", ".*", true, 1);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    }
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu", &rvalue);
    assert_int_equal(rc, SR_ERR_OK);
    assert_int_equal(SR_UINT16_T, rvalue->type);
    assert_string_equal("/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu", rvalue->xpath);
    assert_int_equal(1600, rvalue->data.uint16_val);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* remove a leaf */
    exec_shell_command("../src/sysrepocfg -d running -r \"/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu\" ietf-interfaces", ".*", true, 0);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    }
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu", &rvalue);
    assert_int_equal(rc, SR_ERR_NOT_FOUND);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* remove multiple leaves in one shot */
    exec_shell_command("../src/sysrepocfg -d running -r \"/ietf-interfaces:interfaces/interface[name='eth1']/ietf-ip:ipv4/mtu\" -r \"/ietf-interfaces:interfaces/interface[name='eth1']/description\" ietf-interfaces", ".*", true, 0);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    }
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth1']/ietf-ip:ipv4/mtu", &rvalue);
    assert_int_equal(rc, SR_ERR_NOT_FOUND);
    sr_free_val(rvalue);
    rvalue = NULL;

    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth1']/description", &rvalue);
    assert_int_equal(rc, SR_ERR_NOT_FOUND);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* create a new list entry */
    exec_shell_command("../src/sysrepocfg -d running -s \"/ietf-interfaces:interfaces/interface[name='eth6']/type\" -w 'iana-if-type:ethernetCsmacd'", ".*", true, 0);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    }
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth6']/type", &rvalue);
    assert_int_equal(rc, SR_ERR_OK);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* remove a list entry */
    exec_shell_command("../src/sysrepocfg -d running -r \"/ietf-interfaces:interfaces/interface[name='eth6']\" ietf-interfaces", ".*", true, 0);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
    }
    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth6']/type", &rvalue);
    assert_int_equal(rc, SR_ERR_NOT_FOUND);
    sr_free_val(rvalue);
    rvalue = NULL;

    /* restore pre-test state */
    assert_int_equal(0, srcfg_test_unsubscribe("ietf-interfaces"));
}

static void
srcfg_test_merge(void **state)
{
    sr_val_t *rvalue = { 0 };
    int rc = 0;

    exec_shell_command("../src/sysrepocfg -d running -f " SR_FILE_FORMAT_EXT " -g /ietf-interfaces:*//*", "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("ietf-interfaces"));
    exec_shell_command("../src/sysrepocfg -m " TEST_DATA_SEARCH_DIR "ietf-interfaces.merge." SR_FILE_FORMAT_EXT " -d running ietf-interfaces", ".*", true, 0);
    assert_int_equal(rc, SR_ERR_OK);
    rc = sr_session_refresh(srcfg_test_session);
    if (rc != SR_ERR_OK) {
        printf("Error by sr_session_refresh %s\n", sr_strerror(rc));
        goto cleanup;
    }

    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth0']/description", &rvalue);
    assert_int_equal(rc, SR_ERR_OK);
    assert_int_equal(SR_STRING_T, rvalue->type);
    assert_string_equal("/ietf-interfaces:interfaces/interface[name='eth0']/description", rvalue->xpath);
    assert_string_equal("Ethernet 0 for Merging", rvalue->data.string_val);
    sr_free_val(rvalue);
    rvalue = NULL;

    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu", &rvalue);
    assert_int_equal(rc, SR_ERR_OK);
    assert_int_equal(SR_UINT16_T, rvalue->type);
    assert_string_equal("/ietf-interfaces:interfaces/interface[name='eth0']/ietf-ip:ipv4/mtu", rvalue->xpath);
    assert_int_equal(1600, rvalue->data.uint16_val);
    sr_free_val(rvalue);
    rvalue = NULL;

    rc = sr_get_item(srcfg_test_session, "/ietf-interfaces:interfaces/interface[name='vdsl0']/description", &rvalue);
    assert_int_equal(rc, SR_ERR_OK);
    assert_int_equal(SR_STRING_T, rvalue->type);
    assert_string_equal("/ietf-interfaces:interfaces/interface[name='vdsl0']/description", rvalue->xpath);
    assert_string_equal("Vdsl 0 for Merging", rvalue->data.string_val);
    sr_free_val(rvalue);
    rvalue = NULL;

cleanup:
    /* restore pre-test state */
    assert_int_equal(0, srcfg_test_unsubscribe("ietf-interfaces"));
}

static void
srcfg_test_import(void **state)
{
    int rc = 0;
    md_ctx_t *md_ctx = NULL;
    md_module_t *module = NULL;

    /* invalid arguments */
    exec_shell_command("../src/sysrepocfg --import --datastore=startup --format=txt ietf-interfaces < /tmp/ietf-interfaces.startup.xml", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --import=/tmp/ietf-interfaces.startup.xml --datastore=startup --format=xml", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=txt ietf-interfaces < /tmp/ietf-interfaces.running.xml", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --import=/tmp/ietf-interfaces.running.xml --datastore=running --format=xml", ".*", true, 1);

    /* import ietf-interfaces, test-module, example-module, cross-module and referenced-data configuration from temporary files */

    /* ietf-interfaces */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=startup --format=xml ietf-interfaces < /tmp/ietf-interfaces.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "ietf-interfaces.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --import=/tmp/ietf-interfaces.startup.json --datastore=startup --format=json ietf-interfaces", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "ietf-interfaces.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml ietf-interfaces < /tmp/ietf-interfaces.running.xml",
                       "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("ietf-interfaces"));
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml ietf-interfaces < /tmp/ietf-interfaces.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "ietf-interfaces.running", SR_FILE_FORMAT_LY));
    /*  running, json, permanent */
    exec_shell_command("../src/sysrepocfg --permanent --import=/tmp/ietf-interfaces.running.json --datastore=running --format=json ietf-interfaces", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "ietf-interfaces.running", SR_FILE_FORMAT_LY));
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/ietf-interfaces.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "ietf-interfaces.startup", SR_FILE_FORMAT_LY));
    /* check the internal data file with module dependencies (just in case) */
    rc = md_init(TEST_SCHEMA_SEARCH_DIR, TEST_SCHEMA_SEARCH_DIR "internal/", TEST_DATA_SEARCH_DIR "internal/",
                 false, &md_ctx);
    assert_int_equal(0, rc);
    rc = md_get_module_info(md_ctx, "ietf-interfaces", "2014-05-08", NULL, &module);
    assert_int_equal(SR_ERR_OK, rc);
    md_destroy(md_ctx);

    /* test-module */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=startup --format=xml test-module < /tmp/test-module.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "test-module.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --import=/tmp/test-module.startup.json --datastore=startup --format=json test-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "test-module.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml test-module < /tmp/test-module.running.xml",
                       "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("test-module"));
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml test-module < /tmp/test-module.running.xml",
                       "Cannot read data from module 'referenced-data' .* no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("referenced-data"));
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml test-module < /tmp/test-module.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "test-module.running", SR_FILE_FORMAT_LY));
    /*  running, json, permanent */
    exec_shell_command("../src/sysrepocfg --permanent --import=/tmp/test-module.running.json --datastore=running --format=json test-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "test-module.running", SR_FILE_FORMAT_LY));
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/test-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "test-module.startup", SR_FILE_FORMAT_LY));
    /* check the internal data file with module dependencies (just in case) */
    rc = md_init(TEST_SCHEMA_SEARCH_DIR, TEST_SCHEMA_SEARCH_DIR "internal/", TEST_DATA_SEARCH_DIR "internal/",
                 false, &md_ctx);
    assert_int_equal(0, rc);
    rc = md_get_module_info(md_ctx, "test-module", "", NULL, &module);
    assert_int_equal(SR_ERR_OK, rc);
    md_destroy(md_ctx);
    assert_int_equal(0, srcfg_test_unsubscribe("referenced-data"));

    /* example-module */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=startup --format=xml example-module < /tmp/example-module.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "example-module.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --import=/tmp/example-module.startup.json --datastore=startup --format=json example-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "example-module.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml example-module < /tmp/example-module.running.xml",
                       "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("example-module"));
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml example-module < /tmp/example-module.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "example-module.running", SR_FILE_FORMAT_LY));
    /*  running, json, permanent */
    exec_shell_command("../src/sysrepocfg --permanent --import=/tmp/example-module.running.json --datastore=running --format=json example-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "example-module.running", SR_FILE_FORMAT_LY));
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/example-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "example-module.startup", SR_FILE_FORMAT_LY));
    /* check the internal data file with module dependencies (just in case) */
    rc = md_init(TEST_SCHEMA_SEARCH_DIR, TEST_SCHEMA_SEARCH_DIR "internal/", TEST_DATA_SEARCH_DIR "internal/",
                 false, &md_ctx);
    assert_int_equal(0, rc);
    rc = md_get_module_info(md_ctx, "example-module", "", NULL, &module);
    assert_int_equal(SR_ERR_OK, rc);
    md_destroy(md_ctx);

    /* cross-module */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=startup --format=xml cross-module < /tmp/cross-module.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "cross-module.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --import=/tmp/cross-module.startup.json --datastore=startup --format=json cross-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "cross-module.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml cross-module < /tmp/cross-module.running.xml",
                       "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("cross-module"));
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml cross-module < /tmp/cross-module.running.xml",
                       "Cannot read data from module 'referenced-data' .* no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("referenced-data"));
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml cross-module < /tmp/cross-module.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "cross-module.running", SR_FILE_FORMAT_LY));
    /*  running, json, permanent */
    exec_shell_command("../src/sysrepocfg --permanent --import=/tmp/cross-module.running.json --datastore=running --format=json cross-module", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "cross-module.running", SR_FILE_FORMAT_LY));
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/cross-module.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "cross-module.startup", SR_FILE_FORMAT_LY));
    /* check the internal data file with module dependencies (just in case) */
    rc = md_init(TEST_SCHEMA_SEARCH_DIR, TEST_SCHEMA_SEARCH_DIR "internal/", TEST_DATA_SEARCH_DIR "internal/",
                 false, &md_ctx);
    assert_int_equal(0, rc);
    rc = md_get_module_info(md_ctx, "cross-module", "", NULL, &module);
    assert_int_equal(SR_ERR_OK, rc);
    md_destroy(md_ctx);
    assert_int_equal(0, srcfg_test_unsubscribe("referenced-data"));

    /* referenced-data */
    /*  startup, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=startup --format=xml referenced-data < /tmp/referenced-data.startup.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.startup.xml", LYD_XML, TEST_DATA_SEARCH_DIR "referenced-data.startup", SR_FILE_FORMAT_LY));
    /*  startup, json */
    exec_shell_command("../src/sysrepocfg --import=/tmp/referenced-data.startup.json --datastore=startup --format=json referenced-data", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.startup.json", LYD_JSON, TEST_DATA_SEARCH_DIR "referenced-data.startup", SR_FILE_FORMAT_LY));
    /*  running, xml */
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml referenced-data < /tmp/referenced-data.running.xml",
                       "no active subscriptions", true, 1);
    assert_int_equal(0, srcfg_test_subscribe("referenced-data"));
    exec_shell_command("../src/sysrepocfg --import --datastore=running --format=xml referenced-data < /tmp/referenced-data.running.xml", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.running.xml", LYD_XML, TEST_DATA_SEARCH_DIR "referenced-data.running", SR_FILE_FORMAT_LY));
    /*  running, json, permanent */
    exec_shell_command("../src/sysrepocfg --permanent --import=/tmp/referenced-data.running.json --datastore=running --format=json referenced-data", ".*", true, 0);
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "referenced-data.running", SR_FILE_FORMAT_LY));
    assert_int_equal(0, srcfg_test_cmp_data_files("/tmp/referenced-data.running.json", LYD_JSON, TEST_DATA_SEARCH_DIR "referenced-data.startup", SR_FILE_FORMAT_LY));
    /* check the internal data file with module dependencies (just in case) */
    rc = md_init(TEST_SCHEMA_SEARCH_DIR, TEST_SCHEMA_SEARCH_DIR "internal/", TEST_DATA_SEARCH_DIR "internal/",
                 false, &md_ctx);
    assert_int_equal(0, rc);
    rc = md_get_module_info(md_ctx, "referenced-data", "", NULL, &module);
    assert_int_equal(SR_ERR_OK, rc);
    md_destroy(md_ctx);

    /* restore pre-test state */
    assert_int_equal(0, srcfg_test_unsubscribe("ietf-interfaces"));
    assert_int_equal(0, srcfg_test_unsubscribe("test-module"));
    assert_int_equal(0, srcfg_test_unsubscribe("example-module"));
    assert_int_equal(0, srcfg_test_unsubscribe("cross-module"));
    assert_int_equal(0, srcfg_test_unsubscribe("referenced-data"));
}

static void
srcfg_test_prepare_config(const char *config)
{
    int fd = open(FILENAME_NEW_CONFIG, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    assert_true(fd >= 0);
    write(fd, config, strlen(config) * (sizeof *config));
    close(fd);
}

static void
srcfg_test_prepare_user_input(const char *input)
{
    int fd = open(FILENAME_USER_INPUT, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    assert_true(fd >= 0);
    write(fd, input, strlen(input) * (sizeof *input));
    close(fd);
}

static void
srcfg_test_editing(void **state)
{
    char cmd[PATH_MAX] = { 0, };
    char *args = NULL;

    /* invalid arguments */
    exec_shell_command("../src/sysrepocfg --datastore=candidate ietf-interfaces", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --datastore=startup --format=txt ietf-interfaces", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --datastore=startup --format=json", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --datastore=running --format=txt ietf-interfaces", ".*", true, 1);
    exec_shell_command("../src/sysrepocfg --datastore=running --format=json", ".*", true, 1);

    /* prepare command to execute sysrepocfg */
    strcat(cmd, "cat " FILENAME_USER_INPUT " | PATH=");
    assert_non_null(getcwd(cmd + strlen(cmd), PATH_MAX - strlen(cmd)));
    strcat(cmd, ":$PATH ../src/sysrepocfg --editor=sysrepocfg_test_editor.sh --format=xml --datastore=");
    strcat(cmd, srcfg_test_datastore);
    strcat(cmd, " ");
    args = cmd + strlen(cmd);

    /**
     * module: test-module
     * format: default(xml)
     * valid?: yes
     * permanent?: no
     **/
    char *test_module1 = "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameA</name>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameB</name>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameC</name>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameD</name>\n"
        "</user>\n"
        /* newly added list entry */
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameE</name>\n"
        "  <type>typeE</type>\n"
        "</user>\n";
    srcfg_test_prepare_config(test_module1);
    srcfg_test_prepare_user_input("");
    strcpy(args,"test-module");
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command(cmd, "no active subscriptions", true, 1);
        assert_int_equal(0, srcfg_test_subscribe("test-module"));
        assert_int_equal(0, srcfg_test_subscribe("referenced-data"));
    }
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml test-module > /tmp/test-module_edited.xml", ".*", true, 0);
    } else {
        exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml test-module > /tmp/test-module_edited.xml", ".*", true, 0);
    }
    srcfg_test_cmp_data_file_content("/tmp/test-module_edited.xml", LYD_XML, test_module1, LYD_XML);

    /**
     * module: test-module
     * format: default(xml)
     * valid?: yes
     * permanent?: yes
     **/
    char *test_module2 = "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameA</name>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameB</name>\n"
        "  <type>typeB</type>\n" /* added leaf */
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n" /* moved list entry */
        "  <name>nameD</name>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameC</name>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n" /* created (+moved) list entry */
        "  <name>nameX</name>\n"
        "  <type>typeX</type>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameE</name>\n"
        "  <type>typeE2</type>\n" /* changed */
        "</user>\n";
    srcfg_test_prepare_config(test_module2);
    srcfg_test_prepare_user_input("");
    strcpy(args,"--permanent test-module");
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml test-module > /tmp/test-module_edited.xml", ".*", true, 0);
        srcfg_test_cmp_data_file_content("/tmp/test-module_edited.xml", LYD_XML, test_module2, LYD_XML);
    }
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml test-module > /tmp/test-module_edited.xml", ".*", true, 0);
    srcfg_test_cmp_data_file_content("/tmp/test-module_edited.xml", LYD_XML, test_module2, LYD_XML);

    /**
     * module: test-module
     * format: json
     * valid?: yes (reverting to test_module1)
     * permanent?: yes
     **/
    char *test_module3 = "{\n"
            "\"test-module:user\": [\n"
                "{\n"
                    "\"name\": \"nameA\"\n"
                "},\n"
                "{\n"
                    "\"name\": \"nameB\"\n"
                "},\n"
                "{\n"
                    "\"name\": \"nameC\"\n"
                "},\n"
                "{\n"
                    "\"name\": \"nameD\"\n"
                "},\n"
                "{\n"
                    "\"name\": \"nameE\",\n"
                    "\"type\": \"typeE\"\n"
                "}\n"
            "]\n"
        "}\n";
    srcfg_test_prepare_config(test_module3);
    srcfg_test_prepare_user_input("");
    strcpy(args,"--format=json --permanent test-module");
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml test-module > /tmp/test-module_edited.xml", ".*", true, 0);
        srcfg_test_cmp_data_file_content("/tmp/test-module_edited.xml", LYD_XML, test_module3, LYD_JSON);
    }
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml test-module > /tmp/test-module_edited.xml", ".*", true, 0);
    srcfg_test_cmp_data_file_content("/tmp/test-module_edited.xml", LYD_XML, test_module3, LYD_JSON);

    /**
     * module: test-module
     * format: default(xml)
     * valid?: no
     * permanent?: no
     **/
    char *test_module4 = "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameA</name>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameB</name>\n"
        "</user>\n"
        "<user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameC</name>\n"
        "</user>\n"
        /* missing '<' */
        "user xmlns=\"urn:ietf:params:xml:ns:yang:test-module\">\n"
        "  <name>nameD</name>\n"
        "</user>\n";
    srcfg_test_prepare_config(test_module4);
    srcfg_test_prepare_user_input("y\n y\n n\n y\n sysrepocfg_test-dump.txt\n"); /* 3 failed attempts, then save to local file */
    strcpy(args,"test-module");
    exec_shell_command(cmd, "(.*Unable to apply the changes.*){3}"
                            "Your changes have been saved to 'sysrepocfg_test-dump.txt'", true, 1);
    test_file_content("./sysrepocfg_test-dump.txt", test_module4, false);

    /* remove subscription added due to cross-module dependencies */
    assert_int_equal(0, srcfg_test_unsubscribe("referenced-data"));

    /**
     * module: example-module
     * format: json
     * valid?: yes
     * permanent?: yes
     **/
    char *example_module1 = "{\n"
        "  \"example-module:container\": {\n"
        "    \"list\": [\n"
        "      {\n"
        "        \"key1\": \"key1.1\",\n"
        "        \"key2\": \"key2.1\",\n"
        "        \"leaf\": \"Leaf value A\"\n"
        "      },\n"
        "      {\n"
        "        \"key1\": \"key2.1\",\n"
        "        \"key2\": \"key2.2\",\n"
        "        \"leaf\": \"Leaf value B\"\n"
        "      }\n"
        "    ]\n"
        "  }\n"
        "}\n";
    srcfg_test_prepare_config(example_module1);
    srcfg_test_prepare_user_input("");
    strcpy(args,"--format=json --permanent example-module");
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command(cmd, "no active subscriptions", true, 1);
        assert_int_equal(0, srcfg_test_subscribe("example-module"));
    }
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=json example-module > /tmp/example-module_edited.json", ".*", true, 0);
        srcfg_test_cmp_data_file_content("/tmp/example-module_edited.json", LYD_JSON, example_module1, LYD_JSON);
    }
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=json example-module > /tmp/example-module_edited.json", ".*", true, 0);
    srcfg_test_cmp_data_file_content("/tmp/example-module_edited.json", LYD_JSON, example_module1, LYD_JSON);

    /**
     * module: example-module
     * format: json
     * valid?: no
     * permanent?: no
     **/
    char *example_module2 = "{\n"
        "  \"example-module:container\": {\n"
        "    \"list\": [\n"
        "      {\n"
        "        \"key1\": \"key1.1\",\n"
        "        \"key2\": \"key2.1\",\n"
        "        \"leaf\": \"Leaf value A\"\n"
        "      },\n"
        "      {\n"
        "        \"key1\": \"key2.1\",\n"
        "        \"key2\": \"key2.2\",\n"
        "        \"leaf\": \"Leaf value B\"\n"
        /* missing curly bracket */
        "    ]\n"
        "  }\n"
        "}\n";
    srcfg_test_prepare_config(example_module2);
    srcfg_test_prepare_user_input("y\n n\n y\n sysrepocfg_test-dump.txt\n"); /* 2 failed attempts, then save to local file */
    strcpy(args,"--format=json example-module");
    exec_shell_command(cmd, "(.*Unable to apply the changes.*){2}"
                            "Your changes have been saved to 'sysrepocfg_test-dump.txt'", true, 1);
    test_file_content("./sysrepocfg_test-dump.txt", example_module2, false);

    /**
     * module: ietf-interfaces
     * format: xml
     * valid?: yes (empty config)
     * permanent?: no
     **/
    char *ietf_interfaces1 = "";
    srcfg_test_prepare_config(ietf_interfaces1);
    srcfg_test_prepare_user_input("");
    strcpy(args,"ietf-interfaces");
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command(cmd, "no active subscriptions", true, 1);
        assert_int_equal(0, srcfg_test_subscribe("ietf-interfaces"));
    }
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml ietf-interfaces > /tmp/ietf-interfaces_edited.xml", ".*", true, 0);
    } else {
        exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml ietf-interfaces > /tmp/ietf-interfaces_edited.xml", ".*", true, 0);
    }
    srcfg_test_cmp_data_file_content("/tmp/ietf-interfaces_edited.xml", LYD_XML, ietf_interfaces1, LYD_XML);

    /**
     * module: ietf-interfaces
     * format: xml
     * valid?: yes (two added list entries)
     * permanent?: yes
     **/
    char *ietf_interfaces2 = "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">\n"
        "  <interface>\n"
        "    <name>eth1</name>\n"
        "    <description>Ethernet 1</description>\n"
        "    <type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>\n"
        "    <enabled>true</enabled>\n"
        "    <ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">\n"
        "      <enabled>true</enabled>\n"
        "      <mtu>1500</mtu>\n"
        "      <address>\n"
        "        <ip>10.10.1.5</ip>\n"
        "        <prefix-length>16</prefix-length>\n"
        "      </address>\n"
        "    </ipv4>\n"
        "  </interface>\n"
        "  <interface>\n"
        "    <name>gigaeth1</name>\n"
        "    <description>GigabitEthernet 1</description>\n"
        "    <type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>\n"
        "    <enabled>true</enabled>\n"
        "  </interface>\n"
        "</interfaces>\n";
    srcfg_test_prepare_config(ietf_interfaces2);
    srcfg_test_prepare_user_input("");
    strcpy(args,"--permanent ietf-interfaces");
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml ietf-interfaces > /tmp/ietf-interfaces_edited.xml", ".*", true, 0);
    srcfg_test_cmp_data_file_content("/tmp/ietf-interfaces_edited.xml", LYD_XML, ietf_interfaces2, LYD_XML);

    /**
     * module: ietf-interfaces
     * format: xml
     * valid?: no (missing key)
     * permanent?: no
     **/
    char *ietf_interfaces3 = "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">\n"
        "  <interface>\n"
        /* missing key leaf "name" */
        "    <description>GigabitEthernet 2</description>\n"
        "    <type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>\n"
        "    <enabled>false</enabled>\n"
        "  </interface>\n"
        "</interfaces>\n";
    srcfg_test_prepare_config(ietf_interfaces3);
    srcfg_test_prepare_user_input("n\n n\n"); /* 1 failed attempt, don't even save locally */
    strcpy(args,"ietf-interfaces");
    exec_shell_command(cmd, "(.*Unable to apply the changes.*){1}"
                            "Your changes were discarded", true, 1);

    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *session = NULL;
    assert_int_equal(SR_ERR_OK, sr_connect("sysrepocfg_test", SR_CONN_DEFAULT, &conn));
    assert_non_null(conn);

    assert_int_equal(SR_ERR_OK, sr_session_start(conn, SR_DS_STARTUP, SR_SESS_DEFAULT, &session));
    sr_feature_enable(session, "ietf-ip", "ipv4-non-contiguous-netmasks", false);

    /**
     * module: ietf-interfaces
     * format: xml
     * valid?: no (not enabled feature)
     * permanent?: no
     **/
    char *ietf_interfaces4 = "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">\n"
        "  <interface>\n"
        "    <name>eth1</name>\n"
        "    <description>Ethernet 1</description>\n"
        "    <type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>\n"
        "    <enabled>true</enabled>\n"
        "    <ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">\n"
        "      <enabled>true</enabled>\n"
        "      <mtu>1500</mtu>\n"
        "      <address>\n"
        "        <ip>10.10.1.5</ip>\n"
    // node if-feature ipv4-non-contiguous-netmasks
        "        <netmask>255.255.0.0</netmask>\n"
        "      </address>\n"
        "    </ipv4>\n"
        "  </interface>\n"
        "</interfaces>\n";
    srcfg_test_prepare_config(ietf_interfaces4);
    srcfg_test_prepare_user_input("n\n n\n"); /* 1 failed attempt, don't even save locally */
    strcpy(args,"ietf-interfaces");
    exec_shell_command(cmd, "(.*Unable to apply the changes.*){1}"
                            "Your changes were discarded", true, 1);



    sr_feature_enable(session, "ietf-ip", "ipv4-non-contiguous-netmasks", true);
    /**
     * module: ietf-interfaces
     * format: xml
     * valid?: yes
     * permanent?: no
     **/
    char *ietf_interfaces5 = "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">\n"
        "  <interface>\n"
        "    <name>eth1</name>\n"
        "    <description>Ethernet 1</description>\n"
        "    <type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>\n"
        "    <enabled>true</enabled>\n"
        "    <ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">\n"
        "      <enabled>true</enabled>\n"
        "      <mtu>1500</mtu>\n"
        "      <address>\n"
        "        <ip>10.10.1.5</ip>\n"
    // node if-feature ipv4-non-contiguous-netmasks
        "        <netmask>255.255.0.0</netmask>\n"
        "      </address>\n"
        "    </ipv4>\n"
        "  </interface>\n"
        "</interfaces>\n";
    srcfg_test_prepare_config(ietf_interfaces5);
    srcfg_test_prepare_user_input("");
    strcpy(args,"ietf-interfaces");
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml ietf-interfaces > /tmp/ietf-interfaces_edited.xml", ".*", true, 0);
    } else {
        exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml ietf-interfaces > /tmp/ietf-interfaces_edited.xml", ".*", true, 0);
    }
    srcfg_test_cmp_data_file_content("/tmp/ietf-interfaces_edited.xml", LYD_XML, ietf_interfaces5, LYD_XML);

    sr_feature_enable(session, "ietf-ip", "ipv4-non-contiguous-netmasks", false);
    sr_session_stop(session);
    sr_disconnect(conn);

    /**
     * module: cross-module
     * format: xml
     * valid?: yes (empty config)
     * permanent?: no
     **/
    char *cross_module1 = "";
    srcfg_test_prepare_config(cross_module1);
    srcfg_test_prepare_user_input("");
    strcpy(args,"cross-module");
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command(cmd, "no active subscriptions", true, 1);
        assert_int_equal(0, srcfg_test_subscribe("cross-module"));
        exec_shell_command(cmd, "no active subscriptions", true, 1);
        assert_int_equal(0, srcfg_test_subscribe("referenced-data"));
    }
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml cross-module > /tmp/cross-module_edited.xml", ".*", true, 0);
    } else {
        exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml cross-module > /tmp/cross-module_edited.xml", ".*", true, 0);
    }
    srcfg_test_cmp_data_file_content("/tmp/cross-module_edited.xml", LYD_XML, cross_module1, LYD_XML);

    /**
     * module: referenced-data
     * format: xml
     * valid?: yes (empty config)
     * permanent?: no
     **/
    char *referenced_data1 = "";
    srcfg_test_prepare_config(referenced_data1);
    srcfg_test_prepare_user_input("");
    strcpy(args,"referenced-data");
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml referenced-data > /tmp/referenced-data_edited.xml", ".*", true, 0);
    } else {
        exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml referenced-data > /tmp/referenced-data_edited.xml", ".*", true, 0);
    }
    srcfg_test_cmp_data_file_content("/tmp/referenced-data_edited.xml", LYD_XML, referenced_data1, LYD_XML);

    /**
     * module: cross-module
     * format: xml
     * valid?: no (unsatisfied cross-module dependency)
     * permanent?: no
     **/
    char *cross_module2 = "<reference xmlns=\"urn:cm\">abcd</reference>";
    srcfg_test_prepare_config(cross_module2);
    srcfg_test_prepare_user_input("n\n n\n"); /* 1 failed attempt, don't even save locally */
    strcpy(args, "cross-module");
    exec_shell_command(cmd, "(.*Unable to apply the changes.*){1}"
                            "Your changes were discarded", true, 1);
    /**
     * module: referenced-data
     * format: xml
     * valid?: yes
     * permanent?: no
     **/
    char *referenced_data2 = "<list-b xmlns=\"urn:rd\">\n"
          "  <name>abcd</name>\n"
          "</list-b>\n";
    srcfg_test_prepare_config(referenced_data2);
    srcfg_test_prepare_user_input("");
    strcpy(args,"referenced-data");
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml referenced-data > /tmp/referenced-data_edited.xml", ".*", true, 0);
    } else {
        exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml referenced-data > /tmp/referenced-data_edited.xml", ".*", true, 0);
    }
    srcfg_test_cmp_data_file_content("/tmp/referenced-data_edited.xml", LYD_XML, referenced_data2, LYD_XML);

    /**
     * module: cross-module
     * format: xml
     * valid?: yes (satisfied cross-module dependency)
     * permanent?: no
     **/
    char *cross_module3 = "<reference xmlns=\"urn:cm\">abcd</reference>";
    srcfg_test_prepare_config(cross_module3);
    srcfg_test_prepare_user_input("");
    strcpy(args,"cross-module");
    exec_shell_command(cmd, "The new configuration was successfully applied.", true, 0);
    if (0 == strcmp("running", srcfg_test_datastore)) {
        exec_shell_command("../src/sysrepocfg --export --datastore=running --format=xml cross-module > /tmp/cross-module_edited.xml", ".*", true, 0);
    } else {
        exec_shell_command("../src/sysrepocfg --export --datastore=startup --format=xml cross-module > /tmp/cross-module_edited.xml", ".*", true, 0);
    }
    srcfg_test_cmp_data_file_content("/tmp/cross-module_edited.xml", LYD_XML, cross_module3, LYD_XML);

    /* restore pre-test state */
    if (0 == strcmp("running", srcfg_test_datastore)) {
        assert_int_equal(0, srcfg_test_unsubscribe("ietf-interfaces"));
        assert_int_equal(0, srcfg_test_unsubscribe("test-module"));
        assert_int_equal(0, srcfg_test_unsubscribe("example-module"));
        assert_int_equal(0, srcfg_test_unsubscribe("cross-module"));
        assert_int_equal(0, srcfg_test_unsubscribe("referenced-data"));
    }
}

int
main() {
    int ret = -1;
    sr_schema_t *schemas = NULL;
    size_t schema_cnt = 0;
    const char *path = NULL;
    const struct lys_module *module = NULL;
    uint32_t idx = 0;

    const struct CMUnitTest tests[] = {
            cmocka_unit_test_setup_teardown(srcfg_test_version, NULL, NULL),
            cmocka_unit_test_setup_teardown(srcfg_test_help, NULL, NULL),
            cmocka_unit_test_setup_teardown(srcfg_test_export, srcfg_test_init_datastore_content, NULL),
            cmocka_unit_test_setup_teardown(srcfg_test_editing, srcfg_test_set_startup_datastore, srcfg_test_teardown),
            cmocka_unit_test_setup_teardown(srcfg_test_editing, srcfg_test_set_running_datastore, srcfg_test_teardown),
            cmocka_unit_test_setup_teardown(srcfg_test_import, srcfg_test_init_datastore_content, NULL),
            cmocka_unit_test_setup_teardown(srcfg_test_xpath, srcfg_test_set_running_datastore, srcfg_test_teardown),
            cmocka_unit_test_setup_teardown(srcfg_test_merge, srcfg_test_set_running_datastore_merge, NULL)
    };

    /* create libyang context */
    srcfg_test_libyang_ctx = ly_ctx_new(TEST_SCHEMA_SEARCH_DIR, 0);
    if (NULL == srcfg_test_libyang_ctx) {
        fprintf(stderr, "Unable to initialize libyang context");
        goto terminate;
    }

    /* connect to sysrepo */
    ret = sr_connect("sysrepocfg", SR_CONN_DEFAULT, &srcfg_test_connection);
    if (SR_ERR_OK == ret) {
        ret = sr_session_start(srcfg_test_connection, SR_DS_RUNNING, SR_SESS_DEFAULT, &srcfg_test_session);
    }
    if (SR_ERR_OK != ret) {
        fprintf(stderr, "Unable to connect to sysrepo.\n");
        goto terminate;
    }

    char *modules_for_tests[] = {
        "test-module",
        "example-module",
        "iana-if-type",
        "ietf-interfaces",
        "ietf-ip",
        "referenced-data",
        "cross-module"
    };
    /* load module necessary for tests */
    ret = sr_list_schemas(srcfg_test_session, &schemas, &schema_cnt);
    if (SR_ERR_OK == ret) {
        for (size_t i = 0; i < schema_cnt; i++) {
            path = schemas[i].revision.file_path_yang;
            for (int j = 0; j < sizeof(modules_for_tests) / sizeof(*modules_for_tests); j++) {
                if (NULL != path && 0 == strcmp(modules_for_tests[j], schemas[i].module_name)) {
                    if (NULL == lys_parse_path(srcfg_test_libyang_ctx, path, LYS_IN_YANG)) {
                        fprintf(stderr, "Failed to parse schema file '%s': %s (%s)",
                                path, ly_errmsg(srcfg_test_libyang_ctx), ly_errpath(srcfg_test_libyang_ctx));
                        ret = SR_ERR_IO;
                        goto terminate;
                    }
                    break;
                }
            }

        }
        while (NULL != (module = ly_ctx_get_module_iter(srcfg_test_libyang_ctx, &idx))) {
            for (int i = 0; i < module->features_size; i++) {
                lys_features_enable(module, module->features[i].name);
            }
        }
        sr_free_schemas(schemas, schema_cnt);
    }
    if (SR_ERR_OK != ret) {
        fprintf(stderr, "Unable to load all schemas.\n");
        goto terminate;
    }

    /* start with zero subscriptions */
    for (unsigned i = 0; i < MAX_SUBS; ++i) {
        srcfg_test_subscriptions[i].subscription = NULL;
        srcfg_test_subscriptions[i].module_name = NULL;
    }
    truncate(TEST_DATA_SEARCH_DIR "test-module.persist", 0);
    truncate(TEST_DATA_SEARCH_DIR "ietf-interfaces.persist", 0);
    truncate(TEST_DATA_SEARCH_DIR "example-module.persist", 0);
    truncate(TEST_DATA_SEARCH_DIR "referenced-data.persist", 0);
    truncate(TEST_DATA_SEARCH_DIR "cross-module.persist", 0);

    watchdog_start(300);
    ret = cmocka_run_group_tests(tests, NULL, NULL);
    watchdog_stop();

terminate:
    if (NULL != srcfg_test_session) {
        sr_session_stop(srcfg_test_session);
    }
    if (NULL != srcfg_test_connection) {
        sr_disconnect(srcfg_test_connection);
    }
    if (NULL != srcfg_test_libyang_ctx) {
        ly_ctx_destroy(srcfg_test_libyang_ctx, NULL);
    }
    return ret;
}
