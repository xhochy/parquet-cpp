// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gtest/gtest.h>

#include "parquet/file/reader-internal.h"
#include "parquet/file/writer-internal.h"
#include "parquet/column/reader.h"
#include "parquet/column/writer.h"
#include "parquet/util/input.h"
#include "parquet/util/output.h"
#include "parquet/types.h"

namespace parquet {

using schema::NodePtr;
using schema::PrimitiveNode;

namespace test {

class TestPrimitiveWriter : public ::testing::Test {
 public:
  void SetUpSchemaRequiredNonRepeated() {
    node = PrimitiveNode::Make("int64", Repetition::REQUIRED, Type::INT64);
    schema = std::make_shared<ColumnDescriptor>(node, 0, 0);
  }

  void SetUpSchemaOptionalNonRepeated() {
    node = PrimitiveNode::Make("int64", Repetition::REQUIRED, Type::INT64);
    schema = std::make_shared<ColumnDescriptor>(node, 1, 0);
  }

  void SetUpSchemaOptionalRepeated() {
    node = PrimitiveNode::Make("int64", Repetition::REPEATED, Type::INT64);
    schema = std::make_shared<ColumnDescriptor>(node, 1, 1);
  }

  void SetUp() {
    values_out.resize(100);
    definition_levels_out.resize(100);
    repetition_levels_out.resize(100);

    SetUpSchemaRequiredNonRepeated();
  }

  std::unique_ptr<Int64Reader> BuildReader() {
    auto buffer = sink_->GetBuffer();
    std::unique_ptr<InMemoryInputStream> source(new InMemoryInputStream(buffer));
    std::unique_ptr<SerializedPageReader> page_reader(
        new SerializedPageReader(std::move(source), Compression::UNCOMPRESSED));
    return std::unique_ptr<Int64Reader>(
        new Int64Reader(schema.get(), std::move(page_reader)));
  }

  std::unique_ptr<Int64Writer> BuildWriter() {
    sink_.reset(new InMemoryOutputStream());
    std::unique_ptr<SerializedPageWriter> pager(
        new SerializedPageWriter(sink_.get(), Compression::UNCOMPRESSED));
    return std::unique_ptr<Int64Writer>(new Int64Writer(schema.get(), std::move(pager)));
  }

  void ReadColumn() {
    auto reader = BuildReader();
    reader->ReadBatch(values_out.size(), definition_levels_out.data(),
        repetition_levels_out.data(), values_out.data(), &values_read);
  }

 protected:
  int64_t values_read;
  
  // Output buffers
  std::vector<int64_t> values_out;
  std::vector<int16_t> definition_levels_out;
  std::vector<int16_t> repetition_levels_out;

 private:
  NodePtr node;
  std::shared_ptr<ColumnDescriptor> schema;
  std::unique_ptr<InMemoryOutputStream> sink_;
};

TEST_F(TestPrimitiveWriter, RequiredNonRepeated) {
  std::vector<int64_t> values(100, 128);

  // Test case 1: required and non-repeated, so no definition or repetition levels
  std::unique_ptr<Int64Writer> writer = BuildWriter();
  writer->WriteBatch(values.size(), nullptr, nullptr, values.data());
  writer->Close();

  ReadColumn();
  ASSERT_EQ(values_read, 100);
  ASSERT_EQ(values_out, values);
}

TEST_F(TestPrimitiveWriter, OptionalNonRepeated) {
  // Optional and non-repeated, with definition levels
  // but no repetition levels
  SetUpSchemaOptionalNonRepeated();
  
  std::vector<int64_t> values(100, 128);
  std::vector<int16_t> definition_levels(100, 1);
  definition_levels[1] = 0;
  std::vector<int64_t> values_expected(99, 128);
  
  auto writer = BuildWriter();
  writer->WriteBatch(values.size(), definition_levels.data(), nullptr, values.data());
  writer->Close();

  ReadColumn();
  ASSERT_EQ(values_read, 99);
  values_out.resize(99);
  ASSERT_EQ(values_out, values_expected);
}

TEST_F(TestPrimitiveWriter, OptionalRepeated) {
  // Optional and repeated, so definition and repetition levels
  SetUpSchemaOptionalRepeated();

  std::vector<int64_t> values(100, 128);
  std::vector<int16_t> definition_levels(100, 1);
  definition_levels[1] = 0;
  std::vector<int16_t> repetition_levels(100, 0);
  std::vector<int64_t> values_expected(99, 128);

  auto writer = BuildWriter();
  writer->WriteBatch(values.size(), definition_levels.data(),
      repetition_levels.data(), values.data());
  writer->Close();

  ReadColumn();
  ASSERT_EQ(values_read, 99);
  values_out.resize(99);
  ASSERT_EQ(values_out, values_expected);
}

} // namespace test
} // namespace parquet


