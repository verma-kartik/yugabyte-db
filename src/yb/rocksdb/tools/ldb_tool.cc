//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#include "yb/rocksdb/ldb_tool.h"
#include "yb/rocksdb/tools/ldb_cmd.h"

#include "yb/util/flags.h"

using std::string;

DEFINE_test_flag(bool, exit_on_finish, true, "Exit the process on finishing.");

namespace rocksdb {

LDBOptions::LDBOptions() {}

class LDBCommandRunner {
 public:

  static void PrintHelp(const char* exec_name) {
    string ret;

    ret.append("ldb - LevelDB Tool");
    ret.append("\n\n");
    ret.append("commands MUST specify --" + LDBCommand::ARG_DB +
        "=<full_path_to_db_directory> when necessary\n");
    ret.append("\n");
    ret.append("The following optional parameters control if keys/values are "
        "input/output as hex or as plain strings:\n");
    ret.append("  --" + LDBCommand::ARG_KEY_HEX +
        " : Keys are input/output as hex\n");
    ret.append("  --" + LDBCommand::ARG_VALUE_HEX +
        " : Values are input/output as hex\n");
    ret.append("  --" + LDBCommand::ARG_HEX +
        " : Both keys and values are input/output as hex\n");
    ret.append(
        "  --" + LDBCommand::ARG_CF_NAME +
        " : name of the column family to operate on. default: default column "
        "family\n");
    ret.append("\n");

    ret.append("The following optional parameters control the database "
        "internals:\n");
    ret.append("  --" + LDBCommand::ARG_TTL +
        " with 'put','get','scan','dump','query','batchput'"
        " : DB supports ttl and value is internally timestamp-suffixed\n");
    ret.append("  --" + LDBCommand::ARG_BLOOM_BITS + "=<int,e.g.:14>\n");
    ret.append("  --" + LDBCommand::ARG_FIX_PREFIX_LEN + "=<int,e.g.:14>\n");
    ret.append("  --" + LDBCommand::ARG_COMPRESSION_TYPE +
        "=<no|snappy|zlib|bzip2>\n");
    ret.append("  --" + LDBCommand::ARG_BLOCK_SIZE +
        "=<block_size_in_bytes>\n");
    ret.append("  --" + LDBCommand::ARG_AUTO_COMPACTION + "=<true|false>\n");
    ret.append("  --" + LDBCommand::ARG_DB_WRITE_BUFFER_SIZE +
        "=<int,e.g.:16777216>\n");
    ret.append("  --" + LDBCommand::ARG_WRITE_BUFFER_SIZE +
        "=<int,e.g.:4194304>\n");
    ret.append("  --" + LDBCommand::ARG_FILE_SIZE + "=<int,e.g.:2097152>\n");

    ret.append("\n\n");
    ret.append("Data Access Commands:\n");
    PutCommand::Help(ret);
    GetCommand::Help(ret);
    BatchPutCommand::Help(ret);
    ScanCommand::Help(ret);
    DeleteCommand::Help(ret);
    DBQuerierCommand::Help(ret);
    ApproxSizeCommand::Help(ret);
    CheckConsistencyCommand::Help(ret);

    ret.append("\n\n");
    ret.append("Admin Commands:\n");
    WALDumperCommand::Help(ret);
    CompactorCommand::Help(ret);
    ReduceDBLevelsCommand::Help(ret);
    ChangeCompactionStyleCommand::Help(ret);
    DBDumperCommand::Help(ret);
    DBLoaderCommand::Help(ret);
    ManifestDumpCommand::Help(ret);
    ListColumnFamiliesCommand::Help(ret);
    DBFileDumperCommand::Help(ret);
    InternalDumpCommand::Help(ret);

    fprintf(stderr, "%s\n", ret.c_str());
  }

  static void RunCommand(
      int argc, char** argv, Options options, const LDBOptions& ldb_options,
      const std::vector<ColumnFamilyDescriptor>* column_families) {
    if (argc <= 2) {
      PrintHelp(argv[0]);
      exit(1);
    }

    LDBCommand* cmdObj = LDBCommand::InitFromCmdLineArgs(
        argc, argv, options, ldb_options, column_families);
    if (cmdObj == nullptr) {
      fprintf(stderr, "Unknown command\n");
      PrintHelp(argv[0]);
      exit(1);
    }

    if (!cmdObj->ValidateCmdLineOptions()) {
      exit(1);
    }

    cmdObj->Run();
    LDBCommandExecuteResult ret = cmdObj->GetExecuteState();
    fprintf(stderr, "%s\n", ret.ToString().c_str());
    delete cmdObj;

    if (FLAGS_TEST_exit_on_finish) {
      exit(ret.IsFailed());
    }
  }

};

void LDBTool::Run(int argc, char** argv, Options options,
                  const LDBOptions& ldb_options,
                  const std::vector<ColumnFamilyDescriptor>* column_families) {
  LDBCommandRunner::RunCommand(argc, argv, options, ldb_options,
                               column_families);
}
} // namespace rocksdb
