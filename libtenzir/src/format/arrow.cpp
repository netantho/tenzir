//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2019 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/format/arrow.hpp"

#include "tenzir/detail/assert.hpp"
#include "tenzir/error.hpp"
#include "tenzir/type.hpp"

#include <arrow/api.h>
#include <arrow/io/stdio.h>
#include <arrow/ipc/reader.h>
#include <caf/none.hpp>

#include <iostream>
#include <stdexcept>

namespace tenzir::format::arrow {

writer::writer() {
  out_ = std::make_shared<::arrow::io::StdoutStream>();
}

writer::writer(const caf::settings&) {
  out_ = std::make_shared<::arrow::io::StdoutStream>();
}

caf::error writer::write(const table_slice& slice) {
  if (out_ == nullptr)
    return caf::make_error(ec::logic_error, "invalid arrow output stream");
  auto batch = to_record_batch(slice);
  if (const auto& schema = slice.schema(); current_schema_ != schema) {
    if (!this->schema(batch->schema()))
      return caf::make_error(ec::logic_error, "failed to update schema");
    current_schema_ = schema;
  }
  TENZIR_ASSERT(batch != nullptr);
  if (auto status = current_batch_writer_->WriteRecordBatch(*batch);
      !status.ok())
    return caf::make_error(ec::unspecified, "failed to write record batch",
                           status.ToString());
  // The StdoutStream used `cout` internally. `cout` is buffered by default,
  // which can result in incomplete data at the destination until the next
  // results arrive. We accept the theoretical pessimisation for one-shot
  // writes because the typical batch size is not very small.
  std::cout.flush();
  return caf::none;
}

const char* writer::name() const {
  return "arrow-writer";
}

bool writer::schema(const std::shared_ptr<::arrow::Schema>& schema) {
  if (current_batch_writer_ != nullptr) {
    if (!current_batch_writer_->Close().ok())
      return false;
    current_batch_writer_ = nullptr;
  }
  auto writer_result = ::arrow::ipc::MakeStreamWriter(out_.get(), schema);
  if (writer_result.ok()) {
    current_batch_writer_ = std::move(*writer_result);
    return true;
  }
  return false;
}

arrow_istream_wrapper::arrow_istream_wrapper(std::shared_ptr<std::istream> input)
  : input_(std::move(input)), pos_(0) {
  set_mode(::arrow::io::FileMode::READ);
}

::arrow::Status arrow_istream_wrapper::Close() {
  input_ = nullptr;
  return ::arrow::Status::OK();
}

bool arrow_istream_wrapper::closed() const {
  return input_ && input_->eof();
}

::arrow::Result<int64_t> arrow_istream_wrapper::Tell() const {
  return pos_;
}

::arrow::Result<int64_t>
arrow_istream_wrapper::Read(int64_t nbytes, void* out) {
  if (input_) {
    input_->read(static_cast<char*>(out), nbytes);
    pos_ += nbytes;
    return nbytes;
  }
  return 0;
}

::arrow::Result<std::shared_ptr<::arrow::Buffer>>
arrow_istream_wrapper::Read(int64_t nbytes) {
  ARROW_ASSIGN_OR_RAISE(auto buffer, ::arrow::AllocateResizableBuffer(nbytes));
  ARROW_ASSIGN_OR_RAISE(int64_t bytes_read,
                        Read(nbytes, buffer->mutable_data()));
  ARROW_RETURN_NOT_OK(buffer->Resize(bytes_read, false));
  buffer->ZeroPadding();
  return std::move(buffer);
}

reader::reader(const caf::settings& options, std::unique_ptr<std::istream> in)
  : tenzir::format::reader(options),
    input_(std::make_unique<arrow_istream_wrapper>(std::move(in))) {
}

caf::error
reader::read_impl(size_t max_events, size_t max_slice_size, consumer& f) {
  TENZIR_TRACE_SCOPE("{} {}", TENZIR_ARG(max_events),
                     TENZIR_ARG(max_slice_size));
  TENZIR_ASSERT(max_events > 0);
  // TODO: we currently ignore `max_slize_size` because we're just passing
  // through existing table slices / record batches from the producer system.
  TENZIR_ASSERT(max_slice_size > 0);
  size_t produced = 0;
  while (produced < max_events) {
    if (!reader_) {
      auto open_result
        = ::arrow::ipc::RecordBatchStreamReader::Open(input_.get());
      if (!open_result.ok())
        return caf::make_error(ec::logic_error,
                               fmt::format("failed to open stream reader: '{}'",
                                           open_result.status().ToString()));
      reader_ = open_result.MoveValueUnsafe();
    }
    if (batch_events_ > 0 && batch_timeout_ > reader_clock::duration::zero()
        && last_batch_sent_ + batch_timeout_ < reader_clock::now()) {
      TENZIR_DEBUG("{} reached batch timeout", detail::pretty_type_name(this));
      return caf::make_error(ec::timeout, "reached batch timeout");
    }
    const auto read_result = reader_->ReadNext();
    if (!read_result.ok()) {
      // Reading the next record batch yields an error if the input stream ends.
      // We check if it's actually just eof or an actual error.
      if (input_->closed())
        return caf::make_error(ec::end_of_input, "input exhausted");
      return caf::make_error(ec::format_error,
                             fmt::format("failed to read next record batch: {}",
                                         read_result.status().ToString()));
    }
    // When the schema changes and a new IPC message begins, we see one
    // record batch with status `OK` and `nullptr` as data and re-initialize
    // the reader.
    if (!read_result->batch) {
      const auto close_result = reader_->Close();
      reader_ = nullptr;
      if (!close_result.ok())
        return caf::make_error(
          ec::logic_error, fmt::format("failed to close stream reader: '{}'",
                                       close_result.ToString()));
      continue;
    }
    // Skip record batches with incompatible metadata. This check is not
    // complete because it doesn't visit every (potentially nested) array in the
    // batch, but it's a good enough heuristic to prevent users from running
    // into crashes. A proper fix for this requires casting to Tenzir-compatible
    // record batch.
    const auto& metadata = *read_result->batch->schema()->metadata();
    if (metadata.FindKey("TENZIR:name:0") == -1
        && metadata.FindKey("VAST:name:0") == -1) {
      TENZIR_WARN("{} skips record batch with {} rows: metadata is "
                  "incomaptible with Tenzir",
                  detail::pretty_type_name(*this),
                  read_result->batch->num_rows());
      continue;
    }
    auto slice = table_slice{read_result->batch};
    produced += slice.rows();
    f(std::move(slice));
    ++batch_events_;
    last_batch_sent_ = reader_clock::now();
  }
  return caf::none;
}

void reader::reset(std::unique_ptr<std::istream> in) {
  input_ = std::make_unique<arrow_istream_wrapper>(std::move(in));
}

caf::error reader::module([[maybe_unused]] tenzir::module m) {
  // The Tenzir types are automatically generated and cannot be changed.
  return caf::make_error(ec::no_error, "schema is derived from the Arrow "
                                       "input and can't be changed");
}

module reader::module() const {
  return module_;
}

const char* reader::name() const {
  return "arrow-reader";
}

} // namespace tenzir::format::arrow
