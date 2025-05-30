# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# The following only applies to changes made to this file as part of YugaByte development.
#
# Portions Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

set(YB_PCH_PREFIX client)

set(CLIENT_SRCS
  async_rpc.cc
  batcher.cc
  client.cc
  client_builder-internal.cc
  client_error.cc
  client_master_rpc.cc
  client-internal.cc
  client_utils.cc
  error.cc
  error_collector.cc
  in_flight_op.cc
  meta_cache.cc
  meta_data_cache.cc
  namespace_alterer.cc
  permissions.cc
  session.cc
  schema.cc
  stateful_services/stateful_service_client_base.cc
  table.cc
  table_alterer.cc
  table_creator.cc
  table_handle.cc
  tablet_rpc.cc
  transaction.cc
  transaction_cleanup.cc
  transaction_manager.cc
  transaction_pool.cc
  transaction_rpc.cc
  universe_key_client.cc
  value.cc
  xcluster_client.cc
  yb_op.cc
  yb_table_name.cc
)

set(CLIENT_LIBS
  yb_common
  cdc_service_proto
  master_proto
  master_rpc
  master_util
  tserver_proto
  tserver_service_proto
  pg_auto_analyze_service_proto
  pg_cron_leader_service_proto
  gutil
  test_echo_service_proto
  tserver_shared
  yb_ql_expr
  yb_ash
  yb_util
  yb_util_shmem
  yrpc
  yb_dockv)

# We customize the output name/directory of the exported library so that we can
# call it "yb_client" without it colliding with the regular library.
ADD_YB_LIBRARY(yb_client
  SRCS ${CLIENT_SRCS}
  DEPS ${CLIENT_LIBS})

set(CLIENT_MOCK_SRCS
  xcluster_client_mock.cc
)

set(CLIENT_MOCK_LIBS
  yb_client
  gmock
  gtest)

ADD_YB_LIBRARY(yb_client_mock
  SRCS ${CLIENT_MOCK_SRCS}
  DEPS ${CLIENT_MOCK_LIBS})

if(NOT APPLE)
  # Localize thirdparty symbols using a linker version script. This hides them
  # from the client application. The OS X linker does not support the
  # version-script option.
  set(LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/symbols.map")
endif()

# Test utility library

# This code is useful for other tests which use the client, but isn't
# part of the client itself (ie we don't want to ship it to customers,
# and therefore don't need to worry about export strictness)
ADD_YB_LIBRARY(yb_client_test_util
               SRCS client-test-util.cc snapshot_test_util.cc
               DEPS gmock gtest yb_client yb_test_util)

ADD_YB_LIBRARY(ql-dml-test-base
               SRCS ql-dml-test-base.cc txn-test-base.cc
               DEPS integration-tests)

# Tests

set(YB_TEST_LINK_LIBS
    yb_docdb_test_common integration-tests rpc_test_util ql-dml-test-base ${YB_MIN_TEST_LIBS})
ADD_YB_TEST(backup-txn-test)
ADD_YB_TEST(client-test)
ADD_YB_TEST(client-unittest)
ADD_YB_TEST(clone_namespace-test)
ADD_YB_TEST(ql-dml-test)
ADD_YB_TEST(ql-dml-ttl-test)
ADD_YB_TEST(ql-list-test)
ADD_YB_TEST(ql-map-test)
ADD_YB_TEST(ql-tablet-test)
ADD_YB_TEST(ql-transaction-test)
ADD_YB_TEST(ql-stress-test)
ADD_YB_TEST(seal-txn-test)
ADD_YB_TEST(snapshot-txn-test)
ADD_YB_TEST(snapshot_schedule-test)
ADD_YB_TEST(serializable-txn-test)
ADD_YB_TEST(tablet_rpc-test)
ADD_YB_TEST(advisory_lock-test)
