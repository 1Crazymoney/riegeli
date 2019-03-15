// Copyright 2017 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "riegeli/chunk_encoding/transpose_encoder.h"

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/optimization.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "riegeli/base/base.h"
#include "riegeli/base/canonical_errors.h"
#include "riegeli/base/chain.h"
#include "riegeli/bytes/backward_writer.h"
#include "riegeli/bytes/backward_writer_utils.h"
#include "riegeli/bytes/chain_backward_writer.h"
#include "riegeli/bytes/chain_reader.h"
#include "riegeli/bytes/chain_writer.h"
#include "riegeli/bytes/limiting_reader.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/reader_utils.h"
#include "riegeli/bytes/string_reader.h"
#include "riegeli/bytes/writer.h"
#include "riegeli/bytes/writer_utils.h"
#include "riegeli/chunk_encoding/compressor.h"
#include "riegeli/chunk_encoding/compressor_options.h"
#include "riegeli/chunk_encoding/constants.h"
#include "riegeli/chunk_encoding/transpose_internal.h"

namespace riegeli {

namespace {

constexpr uint32_t kInvalidPos = std::numeric_limits<uint32_t>::max();
// Maximum varint value to encode as varint subtype instead of using the buffer.
constexpr uint8_t kMaxVarintInline = 3;

static_assert(kMaxVarintInline < 0x80,
              "Only one byte is used to store inline varint and its value must "
              "concide with its varint encoding");

// Maximum depth of the nested message we break into columns. Submessages with
// deeper nesting are encoded as strings.
constexpr int kMaxRecursionDepth = 100;

// Returns true if "record" is a valid protocol buffer message in the canonical
// encoding. The purpose of this method is to distinguish string from a
// submessage in the proto wire format and to perform validity checks that are
// asserted later (such as that double proto field is followed by at least 8
// bytes of data).
// Note: Protocol buffer with suboptimal varint encoded tags and values (such as
// 0x87,0x00 instead of 0x07) would parse successfully with the default proto
// parser. This can happen for binary strings in proto. However, we need to
// produce exactly the same bytes in the output so we reject message encoded
// in non-canonical way.
bool IsProtoMessage(Reader* record) {
  // We validate that all started proto groups are closed with endgroup tag.
  std::vector<uint32_t> started_groups;
  while (record->Pull()) {
    uint32_t tag;
    if (!ReadCanonicalVarint32(record, &tag)) return false;
    const uint32_t field = tag >> 3;
    if (field == 0) return false;
    switch (static_cast<internal::WireType>(tag & 7)) {
      case internal::WireType::kVarint: {
        uint64_t value;
        if (!ReadCanonicalVarint64(record, &value)) return false;
      } break;
      case internal::WireType::kFixed32:
        if (!record->Skip(sizeof(uint32_t))) return false;
        break;
      case internal::WireType::kFixed64:
        if (!record->Skip(sizeof(uint64_t))) return false;
        break;
      case internal::WireType::kLengthDelimited: {
        uint32_t length;
        if (!ReadCanonicalVarint32(record, &length)) return false;
        if (!record->Skip(length)) return false;
      } break;
      case internal::WireType::kStartGroup:
        started_groups.push_back(field);
        break;
      case internal::WireType::kEndGroup:
        if (started_groups.empty() || started_groups.back() != field) {
          return false;
        }
        started_groups.pop_back();
        break;
      default:
        return false;
    }
  }
  RIEGELI_ASSERT(record->healthy())
      << "Reading record failed: " << record->status();
  return started_groups.empty();
}

// PriorityQueueEntry is used in priority_queue to order destinations by the
// number of transitions into them.
struct PriorityQueueEntry {
  PriorityQueueEntry() {}

  explicit PriorityQueueEntry(uint32_t dest_index, size_t num_transitions)
      : dest_index(dest_index), num_transitions(num_transitions) {}

  // Index of the destination in "tags_list_".
  uint32_t dest_index = 0;
  // Number of transitions into destination.
  size_t num_transitions = 0;
};

bool operator<(PriorityQueueEntry a, PriorityQueueEntry b) {
  // Sort by num_transitions. Largest first.
  if (a.num_transitions != b.num_transitions) {
    return a.num_transitions > b.num_transitions;
  }
  // Break ties for reproducible ordering.
  return a.dest_index < b.dest_index;
}

}  // namespace

inline TransposeEncoder::MessageNode::MessageNode(
    internal::MessageId message_id)
    : message_id(message_id) {}

inline TransposeEncoder::NodeId::NodeId(internal::MessageId parent_message_id,
                                        uint32_t tag)
    : parent_message_id(parent_message_id), tag(tag) {}

inline TransposeEncoder::StateInfo::StateInfo()
    : etag_index(kInvalidPos),
      base(kInvalidPos),
      canonical_source(kInvalidPos) {}

inline TransposeEncoder::StateInfo::StateInfo(uint32_t etag_index,
                                              uint32_t base)
    : etag_index(etag_index), base(base), canonical_source(kInvalidPos) {}

inline TransposeEncoder::DestInfo::DestInfo() : pos(kInvalidPos) {}

inline TransposeEncoder::EncodedTagInfo::EncodedTagInfo(
    NodeId node_id, internal::Subtype subtype)
    : node_id(node_id),
      subtype(subtype),
      state_machine_pos(kInvalidPos),
      public_list_noop_pos(kInvalidPos),
      base(kInvalidPos) {}

inline TransposeEncoder::BufferWithMetadata::BufferWithMetadata(NodeId node_id)
    : buffer(absl::make_unique<Chain>()), node_id(node_id) {}

TransposeEncoder::TransposeEncoder(CompressorOptions options,
                                   uint64_t bucket_size)
    : compression_type_(options.compression_type()),
      bucket_size_(options.compression_type() == CompressionType::kNone
                       ? std::numeric_limits<uint64_t>::max()
                       : bucket_size),
      compressor_(options),
      nonproto_lengths_writer_(Chain()) {}

TransposeEncoder::~TransposeEncoder() {}

void TransposeEncoder::Reset() {
  ChunkEncoder::Reset();
  compressor_.Reset();
  tags_list_.clear();
  encoded_tags_.clear();
  for (std::vector<BufferWithMetadata>& buffers : data_) buffers.clear();
  group_stack_.clear();
  message_nodes_.clear();
  nonproto_lengths_writer_ = ChainBackwardWriter<Chain>(Chain());
  next_message_id_ = internal::MessageId::kRoot + 1;
}

bool TransposeEncoder::AddRecord(absl::string_view record) {
  StringReader<> reader(record);
  return AddRecordInternal(&reader);
}

bool TransposeEncoder::AddRecord(std::string&& record) {
  if (record.size() <= kMaxBytesToCopy) {
    return AddRecord(absl::string_view(record));
  } else {
    return AddRecord(Chain(std::move(record)));
  }
}

bool TransposeEncoder::AddRecord(const Chain& record) {
  ChainReader<> reader(&record);
  return AddRecordInternal(&reader);
}

bool TransposeEncoder::AddRecords(Chain records, std::vector<size_t> limits) {
  RIEGELI_ASSERT_EQ(limits.empty() ? 0u : limits.back(), records.size())
      << "Failed precondition of ChunkEncoder::AddRecords(): "
         "record end positions do not match concatenated record values";
  LimitingReader<ChainReader<>> record_reader((ChainReader<>(&records)));
  for (const size_t limit : limits) {
    RIEGELI_ASSERT_GE(limit, record_reader.pos())
        << "Failed precondition of ChunkEncoder::AddRecords(): "
           "record end positions not sorted";
    record_reader.set_size_limit(limit);
    if (ABSL_PREDICT_FALSE(!AddRecordInternal(&record_reader))) return false;
    RIEGELI_ASSERT_EQ(record_reader.pos(), limit)
        << "Record was not read up to its end";
  }
  if (!record_reader.Close()) {
    RIEGELI_ASSERT_UNREACHABLE()
        << "Closing records failed: " << record_reader.status();
  }
  return true;
}

inline bool TransposeEncoder::AddRecordInternal(Reader* record) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  RIEGELI_ASSERT(record->healthy())
      << "Failed precondition of TransposeEncoder::AddRecordInternal(): "
      << record->status();
  const Position pos_before = record->pos();
  Position size;
  if (!record->Size(&size)) {
    RIEGELI_ASSERT_UNREACHABLE()
        << "Getting record size failed: " << record->status();
  }
  RIEGELI_ASSERT_LE(pos_before, size)
      << "Current position after the end of record";
  size -= pos_before;
  if (ABSL_PREDICT_FALSE(num_records_ == kMaxNumRecords)) {
    return Fail(ResourceExhaustedError("Too many records"));
  }
  if (ABSL_PREDICT_FALSE(size > std::numeric_limits<uint64_t>::max() -
                                    decoded_data_size_)) {
    return Fail(ResourceExhaustedError("Decoded data size too large"));
  }
  ++num_records_;
  decoded_data_size_ += IntCast<uint64_t>(size);
  const bool is_proto = IsProtoMessage(record);
  if (!record->Seek(pos_before)) {
    RIEGELI_ASSERT_UNREACHABLE()
        << "Seeking reader of a record failed: " << record->status();
  }
  if (is_proto) {
    encoded_tags_.push_back(GetPosInTagsList(
        GetNode(NodeId(internal::MessageId::kStartOfMessage, 0)),
        internal::Subtype::kTrivial));
    LimitingReader<> message(record);
    return AddMessage(&message, internal::MessageId::kRoot, 0);
  } else {
    Node* node = GetNode(NodeId(internal::MessageId::kNonProto, 0));
    encoded_tags_.push_back(
        GetPosInTagsList(node, internal::Subtype::kTrivial));
    BackwardWriter* const buffer = GetBuffer(node, BufferType::kNonProto);
    if (ABSL_PREDICT_FALSE(!record->CopyTo(buffer, IntCast<size_t>(size)))) {
      return Fail(*buffer);
    }
    if (ABSL_PREDICT_FALSE(!WriteVarint64(&nonproto_lengths_writer_,
                                          IntCast<uint64_t>(size)))) {
      return Fail(nonproto_lengths_writer_);
    }
    return true;
  }
}

inline BackwardWriter* TransposeEncoder::GetBuffer(Node* node,
                                                   BufferType type) {
  if (!node->second.writer) {
    std::vector<BufferWithMetadata>& buffers =
        data_[static_cast<uint32_t>(type)];
    buffers.emplace_back(node->first);
    node->second.writer =
        absl::make_unique<ChainBackwardWriter<>>(buffers.back().buffer.get());
  }
  return node->second.writer.get();
}

inline uint32_t TransposeEncoder::GetPosInTagsList(Node* node,
                                                   internal::Subtype subtype) {
  size_t pos = static_cast<size_t>(subtype);
  if (node->second.encoded_tag_pos.size() <= pos) {
    node->second.encoded_tag_pos.resize(pos + 1,
                                        std::numeric_limits<uint32_t>::max());
  }
  uint32_t* ret = &node->second.encoded_tag_pos[pos];
  if (*ret == std::numeric_limits<uint32_t>::max()) {
    *ret = tags_list_.size();
    tags_list_.emplace_back(node->first, subtype);
  }
  return *ret;
}

inline TransposeEncoder::Node* TransposeEncoder::GetNode(NodeId node_id) {
  auto it = message_nodes_.find(node_id);
  if (it == message_nodes_.end()) {
    it = message_nodes_.emplace(node_id, next_message_id_).first;
    ++next_message_id_;
  }
  return &*it;
}

// Precondition: IsProtoMessage returns true for this record.
// Note: EncodedTags are appended into "encoded_tags_" but data is prepended
// into respective buffers. "encoded_tags_" will be reversed later in
// WriteToBuffer call.
inline bool TransposeEncoder::AddMessage(LimitingReaderBase* record,
                                         internal::MessageId parent_message_id,
                                         int depth) {
  while (record->Pull()) {
    uint32_t tag;
    if (!ReadVarint32(record, &tag)) {
      RIEGELI_ASSERT_UNREACHABLE() << "Invalid tag: " << record->status();
    }
    Node* node = GetNode(NodeId(parent_message_id, tag));
    switch (static_cast<internal::WireType>(tag & 7)) {
      case internal::WireType::kVarint: {
        // Storing value as uint64_t[2] instead of uint8_t[10] lets Clang and
        // GCC generate better code for clearing high bit of each byte.
        uint64_t value[2];
        static_assert(sizeof(value) >= kMaxLengthVarint64,
                      "value too small to hold a varint64");
        char* const value_end =
            CopyVarint64(record, reinterpret_cast<char*>(value));
        if (value_end == nullptr) {
          RIEGELI_ASSERT_UNREACHABLE()
              << "Invalid varint: " << record->status();
        }
        const size_t value_length =
            PtrDistance(reinterpret_cast<char*>(value), value_end);
        if (reinterpret_cast<const unsigned char*>(value)[0] <=
            kMaxVarintInline) {
          encoded_tags_.push_back(GetPosInTagsList(
              node, internal::Subtype::kVarintInline0 +
                        reinterpret_cast<const unsigned char*>(value)[0]));
        } else {
          encoded_tags_.push_back(
              GetPosInTagsList(node, internal::Subtype::kVarint1 +
                                         IntCast<uint8_t>(value_length - 1)));
          // Clear high bit of each byte.
          for (uint64_t& word : value) word &= ~uint64_t{0x8080808080808080};
          BackwardWriter* const buffer = GetBuffer(node, BufferType::kVarint);
          if (ABSL_PREDICT_FALSE(!buffer->Write(absl::string_view(
                  reinterpret_cast<const char*>(value), value_length)))) {
            return Fail(*buffer);
          }
        }
      } break;
      case internal::WireType::kFixed32: {
        encoded_tags_.push_back(
            GetPosInTagsList(node, internal::Subtype::kTrivial));
        BackwardWriter* const buffer = GetBuffer(node, BufferType::kFixed32);
        if (ABSL_PREDICT_FALSE(!record->CopyTo(buffer, sizeof(uint32_t)))) {
          return Fail(*buffer);
        }
      } break;
      case internal::WireType::kFixed64: {
        encoded_tags_.push_back(
            GetPosInTagsList(node, internal::Subtype::kTrivial));
        BackwardWriter* const buffer = GetBuffer(node, BufferType::kFixed64);
        if (ABSL_PREDICT_FALSE(!record->CopyTo(buffer, sizeof(uint64_t)))) {
          return Fail(*buffer);
        }
      } break;
      case internal::WireType::kLengthDelimited: {
        uint32_t length;
        const Position length_pos = record->pos();
        if (!ReadVarint32(record, &length)) {
          RIEGELI_ASSERT_UNREACHABLE()
              << "Invalid length: " << record->status();
        }
        const Position value_pos = record->pos();
        SizeLimitSetter size_limiter(record, value_pos + length);
        // Non-toplevel empty strings are treated as strings, not messages.
        // They have a simpler encoding this way (one node instead of two).
        if (depth < kMaxRecursionDepth && length != 0 &&
            IsProtoMessage(record)) {
          encoded_tags_.push_back(GetPosInTagsList(
              node, internal::Subtype::kLengthDelimitedStartOfSubmessage));
          if (!record->Seek(value_pos)) {
            RIEGELI_ASSERT_UNREACHABLE()
                << "Seeking submessage reader failed: " << record->status();
          }
          auto end_of_submessage_pos = GetPosInTagsList(
              node, internal::Subtype::kLengthDelimitedEndOfSubmessage);
          if (ABSL_PREDICT_FALSE(
                  !AddMessage(record, node->second.message_id, depth + 1))) {
            return false;
          }
          // Call to AddMessage invalidates "node"
          node = nullptr;
          encoded_tags_.push_back(end_of_submessage_pos);
        } else {
          encoded_tags_.push_back(GetPosInTagsList(
              node, internal::Subtype::kLengthDelimitedString));
          if (!record->Seek(length_pos)) {
            RIEGELI_ASSERT_UNREACHABLE()
                << "Seeking message reader failed: " << record->status();
          }
          BackwardWriter* const buffer = GetBuffer(node, BufferType::kString);
          if (ABSL_PREDICT_FALSE(!record->CopyTo(
                  buffer, IntCast<size_t>(value_pos - length_pos) + length))) {
            return Fail(*buffer);
          }
        }
      } break;
      case internal::WireType::kStartGroup: {
        encoded_tags_.push_back(
            GetPosInTagsList(node, internal::Subtype::kTrivial));
        group_stack_.push_back(parent_message_id);
        ++depth;
        parent_message_id = node->second.message_id;
      } break;
      case internal::WireType::kEndGroup:
        parent_message_id = group_stack_.back();
        group_stack_.pop_back();
        --depth;
        // Note that "parent_message_id" was updated above so the "node" does
        // not belong to "(parent_message_id, tag)" as in all the other cases.
        // But we don't reload "node" because this still works. All we need is
        // some unique consistent node.
        encoded_tags_.push_back(
            GetPosInTagsList(node, internal::Subtype::kTrivial));
        break;
      default:
        RIEGELI_ASSERT_UNREACHABLE() << "Invalid wire type: " << (tag & 7);
    }
  }
  RIEGELI_ASSERT(record->healthy())
      << "Reading record failed: " << record->status();
  return true;
}

inline bool TransposeEncoder::AddBuffer(bool force_new_bucket,
                                        const Chain& next_chunk,
                                        Writer* data_writer,
                                        std::vector<size_t>* bucket_lengths,
                                        std::vector<size_t>* buffer_lengths) {
  buffer_lengths->push_back(next_chunk.size());
  if (ABSL_PREDICT_FALSE(force_new_bucket ||
                         compressor_.writer()->pos() + next_chunk.size() >
                             bucket_size_) &&
      compressor_.writer()->pos() > 0) {
    const Position pos_before = data_writer->pos();
    if (ABSL_PREDICT_FALSE(!compressor_.EncodeAndClose(data_writer))) {
      return Fail(compressor_);
    }
    RIEGELI_ASSERT_GE(data_writer->pos(), pos_before)
        << "Data writer position decreased";
    bucket_lengths->push_back(IntCast<size_t>(data_writer->pos() - pos_before));
    compressor_.Reset();
  }
  if (ABSL_PREDICT_FALSE(!compressor_.writer()->Write(next_chunk))) {
    return Fail(compressor_);
  }
  return true;
}

inline bool TransposeEncoder::WriteBuffers(
    Writer* header_writer, Writer* data_writer,
    absl::flat_hash_map<NodeId, uint32_t>* buffer_pos) {
  size_t num_buffers = 0;
  for (size_t i = 0; i < kNumBufferTypes; ++i) {
    // Sort data_ by length, largest to smallest.
    std::sort(
        data_[i].begin(), data_[i].end(),
        [](const BufferWithMetadata& a, const BufferWithMetadata& b) {
          if (a.buffer->size() != b.buffer->size()) {
            return a.buffer->size() > b.buffer->size();
          }
          if (a.node_id.parent_message_id != b.node_id.parent_message_id) {
            return a.node_id.parent_message_id < b.node_id.parent_message_id;
          }
          return a.node_id.tag < b.node_id.tag;
        });
    num_buffers += data_[i].size();
  }
  const Chain& nonproto_lengths = nonproto_lengths_writer_.dest();
  if (!nonproto_lengths.empty()) ++num_buffers;

  std::vector<size_t> buffer_lengths;
  buffer_lengths.reserve(num_buffers);
  std::vector<size_t> bucket_lengths;

  // Write all buffer lengths to the header and data to "bucket_buffer".
  for (size_t i = 0; i < kNumBufferTypes; ++i) {
    for (size_t j = 0; j < data_[i].size(); ++j) {
      const BufferWithMetadata& buffer = data_[i][j];
      if (ABSL_PREDICT_FALSE(!AddBuffer(j == 0, *buffer.buffer, data_writer,
                                        &bucket_lengths, &buffer_lengths))) {
        return false;
      }
      const std::pair<absl::flat_hash_map<NodeId, uint32_t>::iterator, bool>
          insert_result = buffer_pos->emplace(
              buffer.node_id, IntCast<uint32_t>(buffer_pos->size()));
      RIEGELI_ASSERT(insert_result.second)
          << "Field already has buffer assigned: "
          << static_cast<uint32_t>(buffer.node_id.parent_message_id) << "/"
          << buffer.node_id.tag;
    }
  }
  if (!nonproto_lengths.empty()) {
    // nonproto_lengths_ is the last buffer if non-empty.
    if (ABSL_PREDICT_FALSE(!AddBuffer(
            /*force_new_bucket=*/true, nonproto_lengths, data_writer,
            &bucket_lengths, &buffer_lengths))) {
      return false;
    }
    // Note: nonproto_lengths needs no buffer_pos.
  }

  if (compressor_.writer()->pos() > 0) {
    // Last bucket.
    const Position pos_before = data_writer->pos();
    if (ABSL_PREDICT_FALSE(!compressor_.EncodeAndClose(data_writer))) {
      return Fail(compressor_);
    }
    RIEGELI_ASSERT_GE(data_writer->pos(), pos_before)
        << "Data writer position decreased";
    bucket_lengths.push_back(IntCast<size_t>(data_writer->pos() - pos_before));
  }

  if (ABSL_PREDICT_FALSE(!WriteVarint32(
          header_writer, IntCast<uint32_t>(bucket_lengths.size()))) ||
      ABSL_PREDICT_FALSE(!WriteVarint32(
          header_writer, IntCast<uint32_t>(buffer_lengths.size())))) {
    return Fail(*header_writer);
  }
  for (const size_t length : bucket_lengths) {
    if (ABSL_PREDICT_FALSE(
            !WriteVarint64(header_writer, IntCast<uint64_t>(length)))) {
      return Fail(*header_writer);
    }
  }
  for (const size_t length : buffer_lengths) {
    if (ABSL_PREDICT_FALSE(
            !WriteVarint64(header_writer, IntCast<uint64_t>(length)))) {
      return Fail(*header_writer);
    }
  }
  return true;
}

inline bool TransposeEncoder::WriteStatesAndData(
    uint32_t max_transition, const std::vector<StateInfo>& state_machine,
    Writer* header_writer, Writer* data_writer) {
  if (!encoded_tags_.empty() &&
      tags_list_[encoded_tags_[0]].dest_info.size() == 1) {
    // There should be no implicit transition from the last state. If there was
    // one, then it would not be obvious whether to stop or continue decoding.
    // Only if transition is explicit we check whether there is more transition
    // bytes.
    absl::flat_hash_map<uint32_t, DestInfo>& dest_info =
        tags_list_[encoded_tags_[0]].dest_info;
    const uint32_t first_key = dest_info.begin()->first;
    dest_info[first_key + 1];
    RIEGELI_ASSERT_NE(tags_list_[encoded_tags_[0]].dest_info.size(), 1u)
        << "Number of transitions from the last state did not increase";
  }
  absl::flat_hash_map<NodeId, uint32_t> buffer_pos;
  if (ABSL_PREDICT_FALSE(
          !WriteBuffers(header_writer, data_writer, &buffer_pos))) {
    return false;
  }

  std::string subtype_to_write;
  std::vector<uint32_t> buffer_index_to_write;
  std::vector<uint32_t> base_to_write;

  base_to_write.reserve(state_machine.size());

  if (ABSL_PREDICT_FALSE(!WriteVarint32(
          header_writer, IntCast<uint32_t>(state_machine.size())))) {
    return Fail(*header_writer);
  }
  for (const StateInfo state_info : state_machine) {
    if (state_info.etag_index == kInvalidPos) {
      // NoOp state.
      if (ABSL_PREDICT_FALSE(!WriteVarint32(
              header_writer,
              static_cast<uint32_t>(internal::MessageId::kNoOp)))) {
        return Fail(*header_writer);
      }
      base_to_write.push_back(state_info.base);
      continue;
    }
    const EncodedTagInfo& etag_info = tags_list_[state_info.etag_index];
    NodeId node_id = etag_info.node_id;
    internal::Subtype subtype = etag_info.subtype;
    if (node_id.tag != 0) {
      const bool is_string = static_cast<internal::WireType>(node_id.tag & 7) ==
                             internal::WireType::kLengthDelimited;
      if (is_string &&
          subtype == internal::Subtype::kLengthDelimitedStartOfSubmessage) {
        if (ABSL_PREDICT_FALSE(!WriteVarint32(
                header_writer, static_cast<uint32_t>(
                                   internal::MessageId::kStartOfSubmessage)))) {
          return Fail(*header_writer);
        }
      } else if (is_string &&
                 subtype ==
                     internal::Subtype::kLengthDelimitedEndOfSubmessage) {
        // End of submessage is encoded as WireType::kSubmessage instead of
        // WireType::kLengthDelimited.
        if (ABSL_PREDICT_FALSE(!WriteVarint32(
                header_writer,
                node_id.tag + (internal::WireType::kSubmessage -
                               internal::WireType::kLengthDelimited)))) {
          return Fail(*header_writer);
        }
      } else {
        if (ABSL_PREDICT_FALSE(!WriteVarint32(header_writer, node_id.tag))) {
          return Fail(*header_writer);
        }
        if (internal::HasSubtype(node_id.tag)) {
          subtype_to_write.push_back(static_cast<char>(subtype));
        }
        if (internal::HasDataBuffer(node_id.tag, subtype)) {
          const absl::flat_hash_map<NodeId, uint32_t>::const_iterator iter =
              buffer_pos.find(NodeId(node_id.parent_message_id, node_id.tag));
          RIEGELI_ASSERT(iter != buffer_pos.end())
              << "Buffer not found: "
              << static_cast<uint32_t>(node_id.parent_message_id) << "/"
              << node_id.tag;
          buffer_index_to_write.push_back(iter->second);
        }
      }
    } else {
      // NonProto and StartOfMessage special IDs.
      if (ABSL_PREDICT_FALSE(!WriteVarint32(
              header_writer,
              static_cast<uint32_t>(node_id.parent_message_id)))) {
        return Fail(*header_writer);
      }
      if (node_id.parent_message_id == internal::MessageId::kNonProto) {
        // NonProto has data buffer.
        const absl::flat_hash_map<NodeId, uint32_t>::const_iterator iter =
            buffer_pos.find(NodeId(internal::MessageId::kNonProto, 0));
        RIEGELI_ASSERT(iter != buffer_pos.end())
            << "Buffer of non-proto records not found";
        buffer_index_to_write.push_back(iter->second);
      } else {
        RIEGELI_ASSERT_EQ(
            static_cast<uint32_t>(node_id.parent_message_id),
            static_cast<uint32_t>(internal::MessageId::kStartOfMessage))
            << "Unexpected message ID with no tag";
      }
    }
    if (tags_list_[state_info.etag_index].base != kInvalidPos) {
      // Signal implicit transition by adding "state_machine.size()".
      base_to_write.push_back(
          tags_list_[state_info.etag_index].base +
          (tags_list_[state_info.etag_index].dest_info.size() == 1
               ? IntCast<uint32_t>(state_machine.size())
               : uint32_t{0}));
    } else {
      // If there is no outgoing transition from this state, just output zero.
      base_to_write.push_back(0);
    }
  }
  for (const uint32_t value : base_to_write) {
    if (ABSL_PREDICT_FALSE(!WriteVarint32(header_writer, value))) {
      return Fail(*header_writer);
    }
  }
  if (ABSL_PREDICT_FALSE(!header_writer->Write(std::move(subtype_to_write)))) {
    return Fail(*header_writer);
  }
  for (const uint32_t value : buffer_index_to_write) {
    if (ABSL_PREDICT_FALSE(!WriteVarint32(header_writer, value))) {
      return Fail(*header_writer);
    }
  }

  // Find the smallest index that has first tag.
  // Note: encoded_tags_ is stored in reverse order so we look for the last
  // element of encoded_tags_.
  uint32_t first_tag_pos = 0;
  if (!encoded_tags_.empty()) {
    while (state_machine[first_tag_pos].etag_index != encoded_tags_.back()) {
      ++first_tag_pos;
    }
  }
  if (ABSL_PREDICT_FALSE(!WriteVarint32(header_writer, first_tag_pos))) {
    return Fail(*header_writer);
  }

  compressor_.Reset();
  if (ABSL_PREDICT_FALSE(!WriteTransitions(max_transition, state_machine))) {
    return false;
  }
  if (ABSL_PREDICT_FALSE(!compressor_.EncodeAndClose(data_writer))) {
    return Fail(compressor_);
  }
  return true;
}

inline bool TransposeEncoder::WriteTransitions(
    uint32_t max_transition, const std::vector<StateInfo>& state_machine) {
  if (encoded_tags_.empty()) return true;
  uint32_t prev_etag = encoded_tags_.back();
  uint32_t current_base = tags_list_[prev_etag].base;
  // Assuming an approximately balanced tree of NoOp nodes covering transitions
  // from the given node in the state machine, the maximal number of bytes
  // needed to encode one transition should be the depth of the tree, i.e.
  // O(log_max_transition(state_machine_size)). We allocate buffer of size
  // kWriteBufSize to store the entire encoded transition.
  // For experiments with low max_transition we use much larger buffer then
  // needed for optimal max_transition == 63.
  constexpr size_t kWriteBufSize = 32;
  uint8_t write[kWriteBufSize];
  absl::optional<uint8_t> last_transition;
  // Go through all transitions and encode them.
  for (uint32_t i = IntCast<uint32_t>(encoded_tags_.size() - 1); i > 0; --i) {
    // There are multiple options how transition may be encoded:
    // 1. Transition is common and it's in the private list for the previous
    //    node.
    // 2. Transition is common and is served from public list. This can have two
    //    forms:
    //      a) Previous node has no private list so we simply serve the
    //         transition using the public node list.
    //      b) Node has private list so we first make a NoOp transition to the
    //         public list and then continue as above.
    uint32_t tag = encoded_tags_[i - 1];
    // Check whether this is implicit transition.
    if (tags_list_[prev_etag].dest_info.size() != 1) {
      // Position in the private list.
      uint32_t pos = tags_list_[prev_etag].dest_info[tag].pos;
      if (pos == kInvalidPos) {
        // "pos" is not in the private list, go to public_list_noop_pos if
        // available.
        // Otherwise base is already in the public list (option 2a).
        pos = tags_list_[prev_etag].public_list_noop_pos;
        if (pos != kInvalidPos) {
          // Option 2b.
          const uint32_t orig_pos = pos;
          size_t write_start = kWriteBufSize;
          // Encode transition from "current_base" to "public_list_noop_pos"
          // which is a NoOp that would lead us to the public list.
          while (current_base > pos || pos - current_base > max_transition) {
            // While desired pos is not reachable using one transition, move to
            // "canonical_source".
            const uint32_t cs = state_machine[pos].canonical_source;
            RIEGELI_ASSERT_LT(cs, state_machine.size())
                << "Canonical source out of range: " << pos;
            RIEGELI_ASSERT_LE(state_machine[cs].base, pos)
                << "Position unreachable from its base: " << pos;
            RIEGELI_ASSERT_LE(pos - state_machine[cs].base, max_transition)
                << "Position unreachable from its base: " << pos;
            RIEGELI_ASSERT_NE(write_start, 0u) << "Write buffer overflow";
            write[--write_start] =
                IntCast<uint8_t>(pos - state_machine[cs].base);
            pos = cs;
          }
          RIEGELI_ASSERT_NE(write_start, 0u) << "Write buffer overflow";
          write[--write_start] = IntCast<uint8_t>(pos - current_base);

          for (size_t j = write_start; j < kWriteBufSize; ++j) {
            if (write[j] == 0 && last_transition.has_value() &&
                (*last_transition & 3) < 3) {
              ++*last_transition;
            } else {
              if (last_transition.has_value()) {
                if (ABSL_PREDICT_FALSE(
                        !WriteByte(compressor_.writer(), *last_transition))) {
                  return Fail(*compressor_.writer());
                }
              }
              last_transition = IntCast<uint8_t>(write[j] << 2);
            }
          }
          // "current_base" is the base of the NoOp that we reached using the
          // transitions so far.
          current_base = state_machine[orig_pos].base;
        }
        // "pos" becomes the position of the state in the public list.
        pos = tags_list_[tag].state_machine_pos;
      }
      RIEGELI_ASSERT_NE(current_base, kInvalidPos)
          << "No outgoing transition from current base";
      RIEGELI_ASSERT_LT(pos, state_machine.size()) << "Position out of range";
      size_t write_start = kWriteBufSize;
      // Encode transition from "current_base" to "pos".
      while (current_base > pos || pos - current_base > max_transition) {
        // While desired pos is not reachable using one transition, move to
        // "canonical_source".
        const uint32_t cs = state_machine[pos].canonical_source;
        RIEGELI_ASSERT_LT(cs, state_machine.size())
            << "Canonical source out of range: " << pos;
        RIEGELI_ASSERT_LE(state_machine[cs].base, pos)
            << "Position unreachable from its base: " << pos;
        RIEGELI_ASSERT_LE(pos - state_machine[cs].base, max_transition)
            << "Position unreachable from its base: " << pos;
        RIEGELI_ASSERT_NE(write_start, 0u) << "Write buffer overflow";
        write[--write_start] = IntCast<uint8_t>(pos - state_machine[cs].base);
        pos = cs;
      }
      RIEGELI_ASSERT_NE(write_start, 0u) << "Write buffer overflow";
      write[--write_start] = IntCast<uint8_t>(pos - current_base);
      for (size_t j = write_start; j < kWriteBufSize; ++j) {
        if (write[j] == 0 && last_transition.has_value() &&
            (*last_transition & 3) < 3) {
          ++*last_transition;
        } else {
          if (last_transition.has_value()) {
            if (ABSL_PREDICT_FALSE(
                    !WriteByte(compressor_.writer(), *last_transition))) {
              return Fail(*compressor_.writer());
            }
          }
          last_transition = IntCast<uint8_t>(write[j] << 2);
        }
      }
    } else {
      RIEGELI_ASSERT_EQ(state_machine[tags_list_[prev_etag].base].etag_index,
                        tag)
          << "Implicit transition goes to a wrong tag";
    }
    prev_etag = tag;
    current_base = tags_list_[prev_etag].base;
  }
  if (last_transition.has_value()) {
    if (ABSL_PREDICT_FALSE(
            !WriteByte(compressor_.writer(), *last_transition))) {
      return Fail(*compressor_.writer());
    }
  }
  return true;
}

inline void TransposeEncoder::CollectTransitionStatistics() {
  // Go through all the transitions from back to front and collect transition
  // distribution statistics.
  uint32_t prev_pos = encoded_tags_.back();
  for (size_t i = encoded_tags_.size() - 1; i > 0; --i) {
    const uint32_t pos = encoded_tags_[i - 1];
    ++tags_list_[prev_pos].dest_info[pos].num_transitions;
    ++tags_list_[pos].num_incoming_transitions;
    prev_pos = pos;
  }

  if (tags_list_[encoded_tags_.back()].num_incoming_transitions == 0) {
    // This guarantees that the initial state is created even if it has no other
    // incoming transition.
    tags_list_[encoded_tags_.back()].num_incoming_transitions = 1;
  }
}

inline void TransposeEncoder::ComputeBaseIndices(
    uint32_t max_transition, uint32_t public_list_base,
    const std::vector<std::pair<uint32_t, uint32_t>>& public_list_noops,
    std::vector<StateInfo>* state_machine_ptr) {
  std::vector<StateInfo>& state_machine = *state_machine_ptr;
  // The related transitions reach a state in the public list so the valid
  // approach would be to simply set all of these to "public_list_base".
  // However, we observe that most of the tags only target few destinations so
  // we can do better if we find the base that is closer to reachable states.
  //
  // We do this by computing "base" of the block that can reach all required
  // destination and "min_pos" of the state that is used in any such transition.

  // Compute "base" indices for NoOp states.
  for (const std::pair<uint32_t, uint32_t> tag_index_and_state_index :
       public_list_noops) {
    // Start of block that can reach all required destinations.
    uint32_t base = kInvalidPos;
    // Smallest position of node used in transition.
    uint32_t min_pos = kInvalidPos;
    for (const std::pair<const uint32_t, DestInfo>& dest_info :
         tags_list_[tag_index_and_state_index.first].dest_info) {
      uint32_t pos = dest_info.second.pos;
      if (pos != kInvalidPos) {
        // This tag has a node in the private list.
        continue;
      }
      // Position of the state that we need to reach.
      pos = tags_list_[dest_info.first].state_machine_pos;
      RIEGELI_ASSERT_NE(pos, kInvalidPos) << "Invalid position";
      // Assuming we processed some states already and "base" is already set to
      // non-kInvalidPos we find the base of the block that is the common
      // ancestor for both "pos" and current "base".
      // If "base <= pos && pos - base <= max_transition" then "pos" can
      // be encoded from "base" using one byte and "base" starts the block we
      // are looking for. If this is not the case then either:
      //  - "base > pos" and "pos" is reachable from one of the common ancestor
      //    blocks of "base" and "pos". In that case we move "base" to the
      //    parent block of "base".
      //  - "pos - base > max_transition" and to reach "pos" we need more than
      //    one transition. In that case we ensure the reachability of "pos" by
      //    ensuring reachability of its canonical_source which belongs to the
      //    parent block of "pos".
      // Note: We assume that transitions in the public list always go from
      // lower to higher indices. This is ensured by the public list generation
      // code.
      // Note: If "base" is kInvalidPos, the condition "base > pos" is true and
      // we handle the first state in there.
      while (base > pos || pos - base > max_transition) {
        if (base > pos) {
          // "cs" is the canonical_source that leads to the block we are looking
          // for.
          uint32_t cs;
          if (base == kInvalidPos) {
            // "base" not initialized yet. We'll use canonical_source of "pos".
            cs = state_machine[pos].canonical_source;
          } else {
            // Set "cs" to the NoOp that leads to "base".
            cs = state_machine[base].canonical_source;
            // If "cs" is kInvalidPos then "base" was already in the first
            // block. But then "base > pos" can't be true.
            RIEGELI_ASSERT_NE(cs, kInvalidPos) << "Unreachable base: " << base;
            // Transitions to previously added states will use "cs" so we update
            // "min_pos".
            min_pos = UnsignedMin(min_pos, cs);
            // To find "base" the block that contains "cs" we move one level
            // above.
            cs = state_machine[cs].canonical_source;
          }
          if (cs == kInvalidPos) {
            // No canonical_source means "base" is in the first block.
            base = public_list_base;
          } else {
            // Otherwise it's the base of the current "cs".
            base = state_machine[cs].base;
          }
        } else {
          // Update "pos" to canonical_source of "pos".
          const uint32_t cs = state_machine[pos].canonical_source;
          RIEGELI_ASSERT_LT(cs, state_machine.size())
              << "Canonical source out of range: " << pos;
          RIEGELI_ASSERT_LE(state_machine[cs].base, pos)
              << "Position unreachable from its base: " << pos;
          RIEGELI_ASSERT_LE(pos - state_machine[cs].base, max_transition)
              << "Position unreachable from its base: " << pos;
          pos = cs;
        }
      }
      min_pos = UnsignedMin(min_pos, pos);
    }
    RIEGELI_ASSERT_NE(min_pos, kInvalidPos)
        << "No outgoing transition from a public NoOp";
    state_machine[tag_index_and_state_index.second].base = min_pos;
  }

  // The same as above for tags without private list.
  for (EncodedTagInfo& tag : tags_list_) {
    if (tag.base != kInvalidPos) {
      // Skip tags with private list.
      continue;
    }
    uint32_t base = kInvalidPos;
    uint32_t min_pos = kInvalidPos;
    for (const std::pair<const uint32_t, DestInfo>& dest_info : tag.dest_info) {
      uint32_t pos = dest_info.second.pos;
      if (pos != kInvalidPos) {
        // Skip destinations in the private list.
        continue;
      }
      pos = tags_list_[dest_info.first].state_machine_pos;
      RIEGELI_ASSERT_NE(pos, kInvalidPos) << "Invalid position";
      while (base > pos || pos - base > max_transition) {
        if (base > pos) {
          uint32_t cs;
          if (base == kInvalidPos) {
            cs = state_machine[pos].canonical_source;
          } else {
            cs = state_machine[base].canonical_source;
            RIEGELI_ASSERT_NE(cs, kInvalidPos) << "Unreachable base: " << base;
            min_pos = UnsignedMin(min_pos, cs);
            cs = state_machine[cs].canonical_source;
          }
          if (cs == kInvalidPos) {
            base = public_list_base;
          } else {
            base = state_machine[cs].base;
          }
        } else {
          const uint32_t cs = state_machine[pos].canonical_source;
          RIEGELI_ASSERT_LT(cs, state_machine.size())
              << "Canonical source out of range: " << pos;
          RIEGELI_ASSERT_LE(state_machine[cs].base, pos)
              << "Position unreachable from its base: " << pos;
          RIEGELI_ASSERT_LE(pos - state_machine[cs].base, max_transition)
              << "Position unreachable from its base: " << pos;
          pos = cs;
        }
      }
      min_pos = UnsignedMin(min_pos, pos);
    }
    if (min_pos != kInvalidPos) tag.base = min_pos;
  }
}

inline std::vector<TransposeEncoder::StateInfo>
TransposeEncoder::CreateStateMachine(uint32_t max_transition,
                                     uint32_t min_count_for_state) {
  std::vector<StateInfo> state_machine;
  if (encoded_tags_.empty()) {
    state_machine.emplace_back(kInvalidPos, 0);
    return state_machine;
  }

  CollectTransitionStatistics();

  // Go through all the tag infos and update transitions that will be included
  // in the private list for the node.
  constexpr uint32_t kInListPos = 0;
  for (EncodedTagInfo& tag_info : tags_list_) {
    for (std::pair<const uint32_t, DestInfo>& dest_and_count :
         tag_info.dest_info) {
      if (dest_and_count.second.num_transitions >= min_count_for_state) {
        // Subtract transitions so we have the right estimate of the remaining
        // transitions into each node.
        tags_list_[dest_and_count.first].num_incoming_transitions -=
            dest_and_count.second.num_transitions;
        // Mark transition to be included in list.
        dest_and_count.second.pos = kInListPos;
      }
    }
  }

  // Priority_queue to order nodes by transition count.
  std::priority_queue<PriorityQueueEntry> tag_priority;
  // Pair of <tag_index, noop_position> where "noop_position" is the index of
  // the NoOp state created for this tag that has base index in the public node
  // list.
  std::vector<std::pair<uint32_t, uint32_t>> public_list_noops;
  // Helper vector to track the base index for NoOp nodes added in the loop
  // below.
  std::vector<uint32_t> noop_base;
  // Create private lists of states for all nodes that have one.
  // After this loop:
  //  - "state_machine" will contain states of created private lists.
  //  - "base" in "tags_list_" will be set for tags with private list.
  //  - "dest_info" in "tags_list_" will have pos != kInvalidPos for those nodes
  //    that already have state.
  //  - "public_list_noops" will have a record for all NoOp states reaching
  //    public list.
  for (uint32_t tag_id = 0; tag_id < tags_list_.size(); ++tag_id) {
    EncodedTagInfo& tag_info = tags_list_[tag_id];
    const uint32_t sz = IntCast<uint32_t>(tag_info.dest_info.size());
    // If we exclude just one state we add it instead of creating the NoOp
    // state.
    PriorityQueueEntry excluded_state;
    // Number of transitions into public list states.
    uint32_t num_excluded_transitions = 0;
    for (const std::pair<const uint32_t, DestInfo>& dest_info :
         tag_info.dest_info) {
      // If destination was marked as "kInListPos" or all transitions into it go
      // from this node.
      if (dest_info.second.pos == kInListPos ||
          dest_info.second.num_transitions ==
              tags_list_[dest_info.first].num_incoming_transitions) {
        if (dest_info.second.pos != kInListPos) {
          // Not yet subtracted.
          tags_list_[dest_info.first].num_incoming_transitions -=
              dest_info.second.num_transitions;
        }
        // Add to the priority queue.
        tag_priority.emplace(dest_info.first, dest_info.second.num_transitions);
      } else {
        num_excluded_transitions += dest_info.second.num_transitions;
        excluded_state = PriorityQueueEntry(dest_info.first,
                                            dest_info.second.num_transitions);
      }
    }
    uint32_t num_states = IntCast<uint32_t>(tag_priority.size());
    if (num_states == 0) {
      // No private list for this tag.
      continue;
    }
    if (num_states + 1 == sz) {
      // If only one state would go to the public list: just add it.
      ++num_states;
      tag_priority.push(excluded_state);
      tags_list_[excluded_state.dest_index].num_incoming_transitions -=
          excluded_state.num_transitions;
    }
    if (num_states != sz) {
      // If not all nodes are in the private list, we'll need NoOp into the
      // public list.
      tag_priority.emplace(kInvalidPos, num_excluded_transitions);
      ++num_states;
    }
    // update "base" for this tag.
    tag_info.base = IntCast<uint32_t>(state_machine.size());
    // Number of NoOp nodes for transitions that can't be encoded using one
    // byte.
    const uint32_t noop_nodes = num_states <= max_transition + 1
                                    ? uint32_t{0}
                                    : (num_states - 2) / max_transition;
    num_states += noop_nodes;
    // We create states back to front. After loop below there will be
    // "state_machine.size() + num_states" states.
    uint32_t prev_state = IntCast<uint32_t>(state_machine.size()) + num_states;
    state_machine.resize(prev_state);
    // States are created in blocks. All blocks except the last one have
    // "max_transition + 1" states. "block_size" is initialized to the size of
    // the last block.
    uint32_t block_size = (num_states - 1) % (max_transition + 1) + 1;
    noop_base.clear();
    for (;;) {
      // Sum of all num_transitions into this block. It will be used as the
      // weight of the NoOp created for this block.
      uint32_t total_block_nodes_weight = 0;
      for (uint32_t i = 0; i < block_size; ++i) {
        RIEGELI_ASSERT(!tag_priority.empty()) << "No remaining nodes";
        total_block_nodes_weight += tag_priority.top().num_transitions;
        const uint32_t node_index = tag_priority.top().dest_index;
        if (node_index == kInvalidPos) {
          // NoOp that goes to the public list.
          state_machine[--prev_state] = StateInfo(kInvalidPos, kInvalidPos);
          tag_info.public_list_noop_pos = prev_state;
          public_list_noops.emplace_back(tag_id, prev_state);
        } else if (node_index >= tags_list_.size()) {
          // NoOp that goes to private list.
          const uint32_t base = noop_base[node_index - tags_list_.size()];
          state_machine[--prev_state] = StateInfo(kInvalidPos, base);
          // Update canonical source for block that this node serves.
          for (uint32_t j = 0; j <= max_transition; ++j) {
            if (j + base >= state_machine.size()) break;
            state_machine[j + base].canonical_source = prev_state;
          }
        } else {
          // Regular state.
          state_machine[--prev_state] = StateInfo(node_index, kInvalidPos);
          tag_info.dest_info[node_index].pos = prev_state;
        }
        tag_priority.pop();
      }
      if (tag_priority.empty()) break;
      // Add new NoOp node into "tag_priority" to serve the block that was just
      // created. Use position greater than tags_list_.size() to distinguish it
      // from both regular state and public_list_noop.
      tag_priority.emplace(tags_list_.size() + noop_base.size(),
                           total_block_nodes_weight);
      // Set the base to the start of the block.
      noop_base.push_back(prev_state);
      // All remaining blocks are "max_transition + 1" states long.
      block_size = max_transition + 1;
    }
  }

  // Base index of the public state list.
  const uint32_t public_list_base = IntCast<uint32_t>(state_machine.size());

  // Add all tags with non-zero incoming transition count to the priority queue.
  for (uint32_t i = 0; i < tags_list_.size(); ++i) {
    if (tags_list_[i].num_incoming_transitions != 0) {
      tag_priority.emplace(i, tags_list_[i].num_incoming_transitions);
    }
  }

  // Create a public list of states. The loop is similar to the public list
  // creation above.
  // After this loop:
  //  - All states in the state machine are created.
  //  - All tags that have an state in the public list have "state_machine_pos"
  //    set.
  uint32_t num_states = IntCast<uint32_t>(tag_priority.size());
  if (num_states > 0) {
    const uint32_t noop_nodes = num_states <= max_transition + 1
                                    ? uint32_t{0}
                                    : (num_states - 2) / max_transition;
    num_states += noop_nodes;
    // Note: The code that assigns "base" indices to states assumes that all
    // NoOp transitions to the child block increase the state index. This is
    // ensured by creating the blocks in reverse order.
    uint32_t prev_node = IntCast<uint32_t>(state_machine.size()) + num_states;
    state_machine.resize(prev_node);
    uint32_t block_size = (num_states - 1) % (max_transition + 1) + 1;
    noop_base.clear();
    for (;;) {
      uint32_t total_block_nodes_weight = 0;
      for (uint32_t i = 0; i < block_size; ++i) {
        RIEGELI_ASSERT(!tag_priority.empty()) << "No remaining nodes";
        total_block_nodes_weight += tag_priority.top().num_transitions;
        const uint32_t node_index = tag_priority.top().dest_index;
        if (node_index >= tags_list_.size()) {
          // NoOp state.
          const uint32_t base = noop_base[node_index - tags_list_.size()];
          state_machine[--prev_node] = StateInfo(kInvalidPos, base);
          for (uint32_t j = 0; j <= max_transition; ++j) {
            if (j + base >= state_machine.size()) break;
            state_machine[j + base].canonical_source = prev_node;
          }
        } else {
          // Regular state.
          state_machine[--prev_node] = StateInfo(node_index, kInvalidPos);
          tags_list_[node_index].state_machine_pos = prev_node;
        }
        tag_priority.pop();
      }
      if (tag_priority.empty()) break;
      tag_priority.emplace(tags_list_.size() + noop_base.size(),
                           total_block_nodes_weight);
      noop_base.push_back(prev_node);
      block_size = max_transition + 1;
    }
  }

  // At this point, the only thing missing is the "base" index for tags
  // without a private list and for NoOp nodes that go to public list.
  ComputeBaseIndices(max_transition, public_list_base, public_list_noops,
                     &state_machine);

  return state_machine;
}

// Maximum transition number. Transitions are encoded as values
// [0..max_transition].
constexpr uint32_t kMaxTransition = 63;
// Minimum number of transitions between nodes A and B for state for node B to
// appear in the private state list for node A.
constexpr uint32_t kMinCountForState = 10;

bool TransposeEncoder::EncodeAndClose(Writer* dest, ChunkType* chunk_type,
                                      uint64_t* num_records,
                                      uint64_t* decoded_data_size) {
  *chunk_type = ChunkType::kTransposed;
  return EncodeAndCloseInternal(kMaxTransition, kMinCountForState, dest,
                                num_records, decoded_data_size);
}

bool TransposeEncoder::EncodeAndCloseInternal(uint32_t max_transition,
                                              uint32_t min_count_for_state,
                                              Writer* dest,
                                              uint64_t* num_records,
                                              uint64_t* decoded_data_size) {
  RIEGELI_ASSERT_LE(max_transition, 63u)
      << "Failed precondition of TransposeEncoder::EncodeAndCloseInternal(): "
         "maximum transition too large to encode";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  *num_records = num_records_;
  *decoded_data_size = decoded_data_size_;
  for (const std::pair<const NodeId, MessageNode>& entry : message_nodes_) {
    if (entry.second.writer != nullptr) {
      if (ABSL_PREDICT_FALSE(!entry.second.writer->Close())) {
        return Fail(*entry.second.writer);
      }
    }
  }
  if (ABSL_PREDICT_FALSE(!nonproto_lengths_writer_.Close())) {
    return Fail(nonproto_lengths_writer_);
  }

  if (ABSL_PREDICT_FALSE(
          !WriteByte(dest, static_cast<uint8_t>(compression_type_)))) {
    return Fail(*dest);
  }

  const std::vector<StateInfo> state_machine =
      CreateStateMachine(max_transition, min_count_for_state);

  ChainWriter<Chain> header_writer((Chain()));
  ChainWriter<Chain> data_writer((Chain()));
  if (ABSL_PREDICT_FALSE(!WriteStatesAndData(max_transition, state_machine,
                                             &header_writer, &data_writer))) {
    return false;
  }
  if (ABSL_PREDICT_FALSE(!header_writer.Close())) return Fail(header_writer);
  if (ABSL_PREDICT_FALSE(!data_writer.Close())) return Fail(data_writer);

  ChainWriter<Chain> compressed_header_writer((Chain()));
  // Uncompressed header size is known before compression, but a size hint
  // cannot be passed to compressor_ because it is reused for compressing
  // buckets and transitions. Reusing the compressor brings more benefits
  // (memory saving) than passing a size hint.
  compressor_.Reset();
  if (ABSL_PREDICT_FALSE(
          !compressor_.writer()->Write(std::move(header_writer.dest())))) {
    return Fail(*compressor_.writer());
  }
  if (ABSL_PREDICT_FALSE(
          !compressor_.EncodeAndClose(&compressed_header_writer))) {
    return Fail(compressor_);
  }
  if (ABSL_PREDICT_FALSE(!compressed_header_writer.Close())) {
    return Fail(compressed_header_writer);
  }
  if (ABSL_PREDICT_FALSE(!WriteVarint64(
          dest, IntCast<uint64_t>(compressed_header_writer.dest().size()))) ||
      ABSL_PREDICT_FALSE(
          !dest->Write(std::move(compressed_header_writer.dest())))) {
    return Fail(*dest);
  }
  if (ABSL_PREDICT_FALSE(!dest->Write(std::move(data_writer.dest())))) {
    return Fail(*dest);
  }
  return Close();
}

}  // namespace riegeli
