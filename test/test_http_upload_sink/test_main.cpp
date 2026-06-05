// Host-side tests for crosspoint::net::HttpUploadSink.
//
// Run via:  pio test -e test_host -f test_http_upload_sink
//
// The file/SD writer is faked through the Writer lambda. No Arduino, no SdFat.
// These exercise the buffering edge cases the two real handlers (/upload and
// /api/fonts/upload) used to hand-roll: partial fills, exact-boundary flush,
// chunks larger than the buffer, short writes, and the END flush of a partial
// tail.

#include <unity.h>

#include <string>
#include <vector>

#include "network/HttpUploadSink.h"

using crosspoint::net::HttpUploadSink;

namespace {

// Collects everything the sink flushes, and can simulate a short write.
struct FakeFile {
  std::vector<uint8_t> bytes;      // all data the sink flushed, in order
  std::vector<size_t> flushSizes;  // size of each flush call
  bool shortWrite = false;         // when true, writer reports 1 byte short

  HttpUploadSink::Writer writer() {
    return [this](const uint8_t* d, size_t n) -> size_t {
      flushSizes.push_back(n);
      const size_t toStore = shortWrite ? (n > 0 ? n - 1 : 0) : n;
      bytes.insert(bytes.end(), d, d + toStore);
      return shortWrite ? toStore : n;
    };
  }
};

std::vector<uint8_t> seq(size_t n, uint8_t start = 0) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(start + i);
  return v;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ensureCapacity sizes the buffer and reports readiness.
void test_ensure_capacity() {
  HttpUploadSink sink(16);
  TEST_ASSERT_FALSE(sink.hasCapacity());
  TEST_ASSERT_TRUE(sink.ensureCapacity());
  TEST_ASSERT_TRUE(sink.hasCapacity());
  TEST_ASSERT_EQUAL_UINT32(16, sink.capacity());
  TEST_ASSERT_EQUAL_UINT32(0, sink.pending());
}

// A sub-buffer write stays buffered (no flush) until END flush.
void test_partial_fill_buffers_until_flush() {
  HttpUploadSink sink(16);
  sink.ensureCapacity();
  FakeFile f;

  auto data = seq(10);
  TEST_ASSERT_TRUE(sink.append(data.data(), data.size(), f.writer()));
  TEST_ASSERT_EQUAL_UINT32(10, sink.pending());
  TEST_ASSERT_EQUAL_UINT32(0, f.bytes.size());  // nothing flushed yet

  TEST_ASSERT_TRUE(sink.flush(f.writer()));
  TEST_ASSERT_EQUAL_UINT32(0, sink.pending());
  TEST_ASSERT_EQUAL_UINT32(10, f.bytes.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data.data(), f.bytes.data(), 10);
}

// Filling to the exact boundary triggers one auto-flush, leaving nothing.
void test_exact_boundary_autoflush() {
  HttpUploadSink sink(8);
  sink.ensureCapacity();
  FakeFile f;

  auto data = seq(8);
  TEST_ASSERT_TRUE(sink.append(data.data(), data.size(), f.writer()));
  TEST_ASSERT_EQUAL_UINT32(0, sink.pending());       // auto-flushed
  TEST_ASSERT_EQUAL_UINT32(1, f.flushSizes.size());  // exactly one flush
  TEST_ASSERT_EQUAL_UINT32(8, f.flushSizes[0]);
  TEST_ASSERT_EQUAL_UINT32(8, f.bytes.size());
}

// A single chunk larger than the buffer flushes repeatedly and keeps the tail.
void test_chunk_larger_than_buffer() {
  HttpUploadSink sink(8);
  sink.ensureCapacity();
  FakeFile f;

  auto data = seq(20);  // 8 + 8 + 4
  TEST_ASSERT_TRUE(sink.append(data.data(), data.size(), f.writer()));
  TEST_ASSERT_EQUAL_UINT32(4, sink.pending());       // 4-byte tail buffered
  TEST_ASSERT_EQUAL_UINT32(2, f.flushSizes.size());  // two full flushes
  TEST_ASSERT_EQUAL_UINT32(16, f.bytes.size());

  TEST_ASSERT_TRUE(sink.flush(f.writer()));
  TEST_ASSERT_EQUAL_UINT32(20, f.bytes.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data.data(), f.bytes.data(), 20);  // order intact
}

// Many small appends accumulate and flush correctly across boundaries.
void test_many_small_appends_order_preserved() {
  HttpUploadSink sink(8);
  sink.ensureCapacity();
  FakeFile f;

  std::vector<uint8_t> all;
  for (uint8_t i = 0; i < 25; ++i) {
    uint8_t b = i;
    TEST_ASSERT_TRUE(sink.append(&b, 1, f.writer()));
    all.push_back(b);
  }
  TEST_ASSERT_TRUE(sink.flush(f.writer()));
  TEST_ASSERT_EQUAL_UINT32(25, f.bytes.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(all.data(), f.bytes.data(), 25);
}

// A short write during an auto-flush makes append return false.
void test_short_write_on_autoflush_fails() {
  HttpUploadSink sink(8);
  sink.ensureCapacity();
  FakeFile f;
  f.shortWrite = true;

  auto data = seq(16);
  TEST_ASSERT_FALSE(sink.append(data.data(), data.size(), f.writer()));
  TEST_ASSERT_EQUAL_UINT32(0, sink.pending());  // position cleared after failed flush
}

// A short write during the END flush makes flush return false.
void test_short_write_on_final_flush_fails() {
  HttpUploadSink sink(16);
  sink.ensureCapacity();
  FakeFile f;

  auto data = seq(5);
  TEST_ASSERT_TRUE(sink.append(data.data(), data.size(), f.writer()));
  f.shortWrite = true;
  TEST_ASSERT_FALSE(sink.flush(f.writer()));
  TEST_ASSERT_EQUAL_UINT32(0, sink.pending());
}

// flush on an empty buffer is a successful no-op (no spurious write).
void test_empty_flush_is_noop() {
  HttpUploadSink sink(16);
  sink.ensureCapacity();
  FakeFile f;
  TEST_ASSERT_TRUE(sink.flush(f.writer()));
  TEST_ASSERT_EQUAL_UINT32(0, f.flushSizes.size());
}

// append before ensureCapacity fails safely instead of writing OOB.
void test_append_without_capacity_fails() {
  HttpUploadSink sink(16);
  FakeFile f;
  auto data = seq(4);
  TEST_ASSERT_FALSE(sink.append(data.data(), data.size(), f.writer()));
}

// reset releases the buffer; a subsequent ensureCapacity re-arms the sink.
void test_reset_then_reuse() {
  HttpUploadSink sink(8);
  sink.ensureCapacity();
  FakeFile f;
  auto data = seq(5);
  sink.append(data.data(), data.size(), f.writer());
  sink.reset();
  TEST_ASSERT_FALSE(sink.hasCapacity());
  TEST_ASSERT_EQUAL_UINT32(0, sink.pending());

  TEST_ASSERT_TRUE(sink.ensureCapacity());
  FakeFile f2;
  auto data2 = seq(3, 100);
  TEST_ASSERT_TRUE(sink.append(data2.data(), data2.size(), f2.writer()));
  TEST_ASSERT_TRUE(sink.flush(f2.writer()));
  TEST_ASSERT_EQUAL_UINT32(3, f2.bytes.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data2.data(), f2.bytes.data(), 3);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ensure_capacity);
  RUN_TEST(test_partial_fill_buffers_until_flush);
  RUN_TEST(test_exact_boundary_autoflush);
  RUN_TEST(test_chunk_larger_than_buffer);
  RUN_TEST(test_many_small_appends_order_preserved);
  RUN_TEST(test_short_write_on_autoflush_fails);
  RUN_TEST(test_short_write_on_final_flush_fails);
  RUN_TEST(test_empty_flush_is_noop);
  RUN_TEST(test_append_without_capacity_fails);
  RUN_TEST(test_reset_then_reuse);
  return UNITY_END();
}
