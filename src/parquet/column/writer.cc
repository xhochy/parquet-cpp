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

#include "parquet/column/writer.h"

#include "parquet/column/properties.h"
#include "parquet/encodings/dictionary-encoding.h"
#include "parquet/encodings/plain-encoding.h"

namespace parquet {

// ----------------------------------------------------------------------
// ColumnWriter

std::shared_ptr<WriterProperties> default_writer_properties() {
  static std::shared_ptr<WriterProperties> default_writer_properties =
      WriterProperties::Builder().build();
  return default_writer_properties;
}

ColumnWriter::ColumnWriter(const ColumnDescriptor* descr,
    std::unique_ptr<PageWriter> pager, int64_t expected_rows, bool has_dictionary,
    MemoryAllocator* allocator)
    : descr_(descr),
      pager_(std::move(pager)),
      expected_rows_(expected_rows),
      has_dictionary_(has_dictionary),
      allocator_(allocator),
      pool_(allocator),
      num_buffered_values_(0),
      num_buffered_encoded_values_(0),
      num_rows_(0),
      total_bytes_written_(0) {
  InitSinks();
}

void ColumnWriter::InitSinks() {
  definition_levels_sink_.reset(new InMemoryOutputStream());
  repetition_levels_sink_.reset(new InMemoryOutputStream());
}

void ColumnWriter::WriteDefinitionLevels(int64_t num_levels, const int16_t* levels) {
  definition_levels_sink_->Write(
      reinterpret_cast<const uint8_t*>(levels), sizeof(int16_t) * num_levels);
}

void ColumnWriter::WriteRepetitionLevels(int64_t num_levels, const int16_t* levels) {
  repetition_levels_sink_->Write(
      reinterpret_cast<const uint8_t*>(levels), sizeof(int16_t) * num_levels);
}

std::shared_ptr<Buffer> ColumnWriter::RleEncodeLevels(
    const std::shared_ptr<Buffer>& buffer, int16_t max_level) {
  // TODO: This only works with due to some RLE specifics
  int64_t rle_size =
      LevelEncoder::MaxBufferSize(Encoding::RLE, max_level, num_buffered_values_) +
      sizeof(uint32_t);
  auto buffer_rle = std::make_shared<OwnedMutableBuffer>(rle_size, allocator_);
  level_encoder_.Init(Encoding::RLE, max_level, num_buffered_values_,
      buffer_rle->mutable_data() + sizeof(uint32_t),
      buffer_rle->size() - sizeof(uint32_t));
  int encoded = level_encoder_.Encode(
      num_buffered_values_, reinterpret_cast<const int16_t*>(buffer->data()));
  DCHECK_EQ(encoded, num_buffered_values_);
  reinterpret_cast<uint32_t*>(buffer_rle->mutable_data())[0] = level_encoder_.len();
  int64_t encoded_size = level_encoder_.len() + sizeof(uint32_t);
  DCHECK(rle_size >= encoded_size);
  buffer_rle->Resize(encoded_size);
  return std::static_pointer_cast<Buffer>(buffer_rle);
}

void ColumnWriter::AddDataPage() {
  DataPageBuffers buffers = {num_buffered_values_, num_buffered_encoded_values_,
      definition_levels_sink_->GetBuffer(), repetition_levels_sink_->GetBuffer(),
      GetValuesBuffer()};

  if (descr_->max_definition_level() > 0) {
    buffers.definition_levels =
        RleEncodeLevels(buffers.definition_levels, descr_->max_definition_level());
  }

  if (descr_->max_repetition_level() > 0) {
    buffers.repetition_levels =
        RleEncodeLevels(buffers.repetition_levels, descr_->max_repetition_level());
  }
  data_page_buffers_.push_back(std::move(buffers));

  // Re-initialize the sinks as GetBuffer made them invalid.
  InitSinks();
  num_buffered_values_ = 0;
  num_buffered_encoded_values_ = 0;
}

void ColumnWriter::WriteNewPage(const DataPageBuffers& buffers) {
  // TODO(PARQUET-590): Encodings are hard-coded
  Encoding::type encoding = Encoding::PLAIN;
  if (has_dictionary_) { encoding = Encoding::PLAIN_DICTIONARY; }
  int64_t bytes_written = pager_->WriteDataPage(buffers.num_buffered_values,
      buffers.num_buffered_encoded_values, buffers.definition_levels, Encoding::RLE,
      buffers.repetition_levels, Encoding::RLE, buffers.values, encoding);
  total_bytes_written_ += bytes_written;
}

int64_t ColumnWriter::Close() {
  if (has_dictionary_) { WriteDictionaryPage(); }
  // Write all outstanding data to a new page
  if (num_buffered_values_ > 0) { AddDataPage(); }

  for (int i = 0; i < data_page_buffers_.size(); i++) {
    WriteNewPage(data_page_buffers_[i]);
  }

  if (num_rows_ != expected_rows_) {
    throw ParquetException(
        "Less then the number of expected rows written in"
        " the current column chunk");
  }

  pager_->Close();

  return total_bytes_written_;
}

// ----------------------------------------------------------------------
// TypedColumnWriter

template <typename Type>
TypedColumnWriter<Type>::TypedColumnWriter(const ColumnDescriptor* schema,
    std::unique_ptr<PageWriter> pager, int64_t expected_rows, Encoding::type encoding,
    MemoryAllocator* allocator)
    : ColumnWriter(schema, std::move(pager), expected_rows,
          (encoding == Encoding::PLAIN_DICTIONARY ||
                       encoding == Encoding::RLE_DICTIONARY),
          allocator) {
  switch (encoding) {
    case Encoding::PLAIN:
      current_encoder_ =
          std::unique_ptr<EncoderType>(new PlainEncoder<Type>(schema, allocator));
      break;
    case Encoding::PLAIN_DICTIONARY:
    case Encoding::RLE_DICTIONARY:
      current_encoder_ =
          std::unique_ptr<EncoderType>(new DictEncoder<Type>(schema, &pool_, allocator));
      break;
    default:
      ParquetException::NYI("Selected encoding is not supported");
  }
}

template <typename Type>
void TypedColumnWriter<Type>::WriteDictionaryPage() {
  auto dict_encoder = static_cast<DictEncoder<Type>*>(current_encoder_.get());
  auto buffer = std::make_shared<OwnedMutableBuffer>(dict_encoder->dict_encoded_size());
  dict_encoder->WriteDict(buffer->mutable_data());
  // TODO Get rid of this deep call
  dict_encoder->mem_pool()->FreeAll();

  // TODO: Encodings are hard-coded
  int64_t bytes_written = pager_->WriteDictionaryPage(
      dict_encoder->num_entries(), buffer, Encoding::PLAIN_DICTIONARY);
  total_bytes_written_ += bytes_written;
}

// ----------------------------------------------------------------------
// Dynamic column writer constructor

std::shared_ptr<ColumnWriter> ColumnWriter::Make(const ColumnDescriptor* descr,
    std::unique_ptr<PageWriter> pager, int64_t expected_rows,
    const WriterProperties* properties) {
  Encoding::type encoding = properties->encoding(descr->path());
  switch (descr->physical_type()) {
    case Type::BOOLEAN:
      return std::make_shared<BoolWriter>(
          descr, std::move(pager), expected_rows, encoding, properties->allocator());
    case Type::INT32:
      return std::make_shared<Int32Writer>(
          descr, std::move(pager), expected_rows, encoding, properties->allocator());
    case Type::INT64:
      return std::make_shared<Int64Writer>(
          descr, std::move(pager), expected_rows, encoding, properties->allocator());
    case Type::INT96:
      return std::make_shared<Int96Writer>(
          descr, std::move(pager), expected_rows, encoding, properties->allocator());
    case Type::FLOAT:
      return std::make_shared<FloatWriter>(
          descr, std::move(pager), expected_rows, encoding, properties->allocator());
    case Type::DOUBLE:
      return std::make_shared<DoubleWriter>(
          descr, std::move(pager), expected_rows, encoding, properties->allocator());
    case Type::BYTE_ARRAY:
      return std::make_shared<ByteArrayWriter>(
          descr, std::move(pager), expected_rows, encoding, properties->allocator());
    case Type::FIXED_LEN_BYTE_ARRAY:
      return std::make_shared<FixedLenByteArrayWriter>(
          descr, std::move(pager), expected_rows, encoding, properties->allocator());
    default:
      ParquetException::NYI("type reader not implemented");
  }
  // Unreachable code, but supress compiler warning
  return std::shared_ptr<ColumnWriter>(nullptr);
}

// ----------------------------------------------------------------------
// Instantiate templated classes

template class TypedColumnWriter<BooleanType>;
template class TypedColumnWriter<Int32Type>;
template class TypedColumnWriter<Int64Type>;
template class TypedColumnWriter<Int96Type>;
template class TypedColumnWriter<FloatType>;
template class TypedColumnWriter<DoubleType>;
template class TypedColumnWriter<ByteArrayType>;
template class TypedColumnWriter<FLBAType>;

}  // namespace parquet
