/**
 * @file test_module_helper.h
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief
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
#ifndef TEST_MODULE_HELPER_H
#define TEST_MODULE_HELPER_H

#include <libyang/libyang.h>
#include "test_data.h"
#include "data_manager.h"

#define TEST_MODULE_DATA_FILE_NAME TEST_DATA_SEARCH_DIR "test-module" SR_STARTUP_FILE_EXT
#define EXAMPLE_MODULE_DATA_FILE_NAME TEST_DATA_SEARCH_DIR "example-module" SR_STARTUP_FILE_EXT
#define REFERENCED_MODULE_DATA_FILE_NAME TEST_DATA_SEARCH_DIR "referenced-data" SR_STARTUP_FILE_EXT
#define STATE_MODULE_DATA_FILE_NAME TEST_DATA_SEARCH_DIR "state-module" SR_STARTUP_FILE_EXT

/**
 * Creates test-module data tree and writes it into a file.
 */
void createDataTreeTestModule();

/**
 * Creates ietf-interfaces data tree and writes it into a file
 */
void createDataTreeIETFinterfacesModule();

/**
 * Creates ietf-interfaces data tree and writes it into a file for merging test
 */

void createDataTreeIETFinterfacesModuleMerge();

/**
 * Creates example-module data tree and writes it into a file.
 */
void createDataTreeExampleModule();

/**
 * Creates example-module data tree with specified number list instances.
 */
void createDataTreeLargeExampleModule(int list_count);

/**
 * Creates ietf-interfaces module configuration for specified number of interfaces
 */
void createDataTreeLargeIETFinterfacesModule(size_t if_count);

/**
 * Creates referenced-data module configuration.
 */
void createDataTreeReferencedModule(int8_t magic_number);

/**
 * Creates state-data module configuration.
 */
void createDataTreeStateModule();

/**
 * Skips the testcase if sysrepod is running.
 */
void skip_if_daemon_running();

#define XP_TEST_MODULE_ENUM "/test-module:main/enum"
#define XP_TEST_MODULE_ENUM_VALUE "maybe"

#define XP_TEST_MODULE_RAW "/test-module:main/raw"
/*Hello world!  (base64 encoded)*/
#define XP_TEST_MODULE_RAW_VALUE "SGVsbG8gd29ybGQh"

#define XP_TEST_MODULE_BITS "/test-module:main/options"
#define XP_TEST_MODULE_BITS_VALUE "strict recursive"

#define XP_TEST_MODULE_BOOL "/test-module:main/boolean"
#define XP_TEST_MODULE_BOOL_VALUE "true"
#define XP_TEST_MODULE_BOOL_VALUE_T true

#define XP_TEST_MODULE_DEC64 "/test-module:main/dec64"
#define XP_TEST_MODULE_DEC64_VALUE "9.85"
#define XP_TEST_MODULE_DEC64_VALUE_T 9.85

#define XP_TEST_MODULE_EMPTY "/test-module:main/empty"
#define XP_TEST_MODULE_EMPTY_VALUE ""

#define XP_TEST_MODULE_IDREF "/test-module:main/id_ref"
#define XP_TEST_MODULE_IDREF_VALUE "test-module:id_1"

#define XP_TEST_MODULE_INT8 "/test-module:main/i8"
#define XP_TEST_MODULE_INT8_VALUE "8"
#define XP_TEST_MODULE_INT8_VALUE_T 8

#define XP_TEST_MODULE_INT16 "/test-module:main/i16"
#define XP_TEST_MODULE_INT16_VALUE "16"
#define XP_TEST_MODULE_INT16_VALUE_T 16

#define XP_TEST_MODULE_INT32 "/test-module:main/i32"
#define XP_TEST_MODULE_INT32_VALUE "32"
#define XP_TEST_MODULE_INT32_VALUE_T 32

#define XP_TEST_MODULE_INT64 "/test-module:main/i64"
#define XP_TEST_MODULE_INT64_VALUE "64"
#define XP_TEST_MODULE_INT64_VALUE_T 64

#define XP_TEST_MODULE_STRING "/test-module:main/string"
#define XP_TEST_MODULE_STRING_VALUE "str"

#define XP_TEST_MODULE_UINT8 "/test-module:main/ui8"
#define XP_TEST_MODULE_UINT8_VALUE "8"
#define XP_TEST_MODULE_UINT8_VALUE_T 8

#define XP_TEST_MODULE_UINT16 "/test-module:main/ui16"
#define XP_TEST_MODULE_UINT16_VALUE "16"
#define XP_TEST_MODULE_UINT16_VALUE_T 16

#define XP_TEST_MODULE_UINT32 "/test-module:main/ui32"
#define XP_TEST_MODULE_UINT32_VALUE "32"
#define XP_TEST_MODULE_UINT32_VALUE_T 32

#define XP_TEST_MODULE_UINT64 "/test-module:main/ui64"
#define XP_TEST_MODULE_UINT64_VALUE "64"
#define XP_TEST_MODULE_UINT64_VALUE_T 64

#define XP_TEST_MODULE_ANYXML "/test-module:main/xml-data"
#define XP_TEST_MODULE_ANYXML_VALUE "<p xmlns=\"http://www.w3.org/1999/xhtml\">This is <em>very</em> cool.</p>"

#define XP_TEST_MODULE_ANYDATA "/test-module:main/any-data"
#define XP_TEST_MODULE_ANYDATA_VALUE "<container><leaf1>value1</leaf1><leaf2>value2</leaf2></container>"

#define XP_TEST_MODULE_INSTANCE_ID "/test-module:main/instance_id"
#define XP_TEST_MODULE_INSTANCE_ID_VALUE "/test-module:main/i64"

#define XP_TEST_MODULE_DEC64_IN_UNION "/test-module:dec64-in-union"
#define XP_TEST_MODULE_DEC64_IN_UNION_VALUE "-11.17"
#define XP_TEST_MODULE_DEC64_IN_UNION_VALUE_T (-11.17)


#endif /* TEST_MODULE_HELPER_H */

