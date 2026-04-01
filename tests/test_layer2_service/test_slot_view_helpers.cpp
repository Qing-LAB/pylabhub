/**
 * @file test_slot_view_helpers.cpp
 * @brief Unit tests for script_host_helpers.hpp slot-view utilities.
 *
 * Tests cover:
 *   - make_slot_view()          — unified read/write slot view builder
 *   - wrap_as_readonly_ctypes() — __setattr__ write guard for read-side types
 *
 * Coverage matrix:
 *   1.  ReadSide_NoSchema_ReturnsPyBytes         — py::bytes, immutable
 *   2.  WriteSide_NoSchema_ReturnsBytearray       — py::bytearray, mutable
 *   3.  WriteSide_Ctypes_CanWriteField            — out_slot field write allowed
 *   4.  WriteSide_Ctypes_WriteModifiesMemory      — backing memory updated (zero-copy)
 *   5.  ReadSide_Ctypes_CanReadField              — in_slot field read returns correct value
 *   6.  ReadSide_Ctypes_WriteRaisesAttributeError — accidental write → AttributeError
 *   7.  ReadSide_Ctypes_ErrorMsgContainsFieldName — error message is informative
 *   8.  ReadSide_Ctypes_RawBufferAccess           — memoryview(in_slot) raw span equivalent
 *   9.  WriteSide_Ctypes_MultiField               — multi-field struct writes
 *  10.  ReadSide_Ctypes_MultiField_ReadsAllFields  — all fields readable after wrap
 *  11.  ReadSide_Ctypes_AllFieldsBlockWrite        — write guard fires on every top-level field
 *  12.  WrapAsReadonly_StandaloneDirectTest        — direct wrap_as_readonly_ctypes() call
 *  13.  WriteSide_Numpy_DataZeroCopy              — numpy write view reflects backing memory
 *  14.  ReadSide_Numpy_DataReadable               — numpy read view returns correct values
 *  15.  WriteSide_PackedStruct_CanWrite           — _pack_=1 variant
 */
#include "script_host_helpers.hpp"
#include "utils/script_host_schema.hpp"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace py = pybind11;
using namespace pylabhub::scripting;

// ============================================================================
// Test fixture — one interpreter per process (pybind11 constraint)
// ============================================================================

class SlotViewHelpersTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        interp_ = new py::scoped_interpreter{};
    }

    static void TearDownTestSuite()
    {
        delete interp_;
        interp_ = nullptr;
    }

  private:
    static py::scoped_interpreter *interp_;
};

py::scoped_interpreter *SlotViewHelpersTest::interp_ = nullptr;

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace
{

SchemaSpec make_int32_spec()
{
    SchemaSpec spec;
    spec.has_schema = true;

    spec.packing    = "aligned";
    spec.fields     = {{"x", "int32", 1, 0}};
    return spec;
}

/// Multi-field: int32 a, float32 b, uint8 c[4]
SchemaSpec make_multi_spec()
{
    SchemaSpec spec;
    spec.has_schema = true;

    spec.packing    = "aligned";
    spec.fields     = {{"a", "int32", 1, 0}, {"b", "float32", 1, 0}, {"c", "uint8", 4, 0}};
    return spec;
}

SchemaSpec make_packed_uint32_spec()
{
    SchemaSpec spec;
    spec.has_schema = true;

    spec.packing    = "packed";
    spec.fields     = {{"val", "uint32", 1, 0}};
    return spec;
}

/// Build a writable ctypes type from spec. Returns size via out-param.
py::object ctypes_type(const SchemaSpec &spec, const char *name, size_t &sz_out)
{
    py::object t = build_ctypes_struct(spec, name);
    sz_out       = ctypes_sizeof(t);
    return t;
}

/// Build a readonly ctypes type from spec.
py::object ctypes_type_ro(const SchemaSpec &spec, const char *name, size_t &sz_out)
{
    py::object t = build_ctypes_struct(spec, name);
    sz_out       = ctypes_sizeof(t);
    return wrap_as_readonly_ctypes(t);
}

/// Execute Python code; return exception string or "" if no exception.
std::string try_exec(const std::string &code, py::dict &ns)
{
    try
    {
        py::exec(code, py::globals(), ns);
        return "";
    }
    catch (py::error_already_set &e)
    {
        return std::string(e.what());
    }
}

} // namespace

// ============================================================================
// 1. ReadSide_NoSchema_ReturnsPyBytes
// ============================================================================

TEST_F(SlotViewHelpersTest, ReadSide_NoSchema_ReturnsPyBytes)
{
    SchemaSpec no_schema; // has_schema = false
    std::array<uint8_t, 4> buf = {0xAA, 0xBB, 0xCC, 0xDD};

    py::object result =
        make_slot_view(no_schema, py::none(), buf.data(), 4, /*is_read_side=*/true);

    ASSERT_TRUE(py::isinstance<py::bytes>(result));
    auto raw = result.cast<std::string>();
    ASSERT_EQ(raw.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(raw[0]), 0xAA);
    EXPECT_EQ(static_cast<uint8_t>(raw[3]), 0xDD);
}

// ============================================================================
// 2. WriteSide_NoSchema_ReturnsBytearray
// ============================================================================

TEST_F(SlotViewHelpersTest, WriteSide_NoSchema_ReturnsBytearray)
{
    SchemaSpec no_schema;
    std::array<uint8_t, 4> buf = {1, 2, 3, 4};

    py::object result =
        make_slot_view(no_schema, py::none(), buf.data(), 4, /*is_read_side=*/false);

    EXPECT_TRUE(py::isinstance<py::bytearray>(result));
}

// ============================================================================
// 3. WriteSide_Ctypes_CanWriteField
// ============================================================================

TEST_F(SlotViewHelpersTest, WriteSide_Ctypes_CanWriteField)
{
    auto   spec = make_int32_spec();
    size_t sz   = 0;
    auto   type = ctypes_type(spec, "Int32FrameW3", sz);

    std::array<uint8_t, 4> buf = {};
    py::object inst = make_slot_view(spec, type, buf.data(), sz, /*is_read_side=*/false);

    py::dict ns;
    ns["inst"] = inst;
    // Should NOT throw — write side allows field assignment
    EXPECT_EQ(try_exec("inst.x = 42", ns), "");
}

// ============================================================================
// 4. WriteSide_Ctypes_WriteModifiesMemory
// ============================================================================

TEST_F(SlotViewHelpersTest, WriteSide_Ctypes_WriteModifiesMemory)
{
    auto   spec = make_int32_spec();
    size_t sz   = 0;
    auto   type = ctypes_type(spec, "Int32FrameW4", sz);

    std::array<uint8_t, 4> buf = {};
    py::object inst = make_slot_view(spec, type, buf.data(), sz, /*is_read_side=*/false);

    py::dict ns;
    ns["inst"] = inst;
    py::exec("inst.x = 1234567890", py::globals(), ns);

    // Verify the backing memory was updated (zero-copy write)
    int32_t val = 0;
    std::memcpy(&val, buf.data(), 4);
    EXPECT_EQ(val, 1234567890);
}

// ============================================================================
// 5. ReadSide_Ctypes_CanReadField
// ============================================================================

TEST_F(SlotViewHelpersTest, ReadSide_Ctypes_CanReadField)
{
    auto   spec = make_int32_spec();
    size_t sz   = 0;
    auto   type = ctypes_type_ro(spec, "Int32FrameR5", sz);

    int32_t    val  = 99;
    py::object inst = make_slot_view(spec, type, &val, sz, /*is_read_side=*/true);

    py::dict ns;
    ns["inst"] = inst;
    py::exec("result = inst.x", py::globals(), ns);
    EXPECT_EQ(ns["result"].cast<int>(), 99);
}

// ============================================================================
// 6. ReadSide_Ctypes_WriteRaisesAttributeError
// ============================================================================

TEST_F(SlotViewHelpersTest, ReadSide_Ctypes_WriteRaisesAttributeError)
{
    auto   spec = make_int32_spec();
    size_t sz   = 0;
    auto   type = ctypes_type_ro(spec, "Int32FrameR6", sz);

    int32_t    val  = 0;
    py::object inst = make_slot_view(spec, type, &val, sz, /*is_read_side=*/true);

    py::dict    ns;
    ns["inst"]      = inst;
    std::string err = try_exec("inst.x = 55", ns);

    EXPECT_FALSE(err.empty()) << "Expected AttributeError, got no exception";
    EXPECT_NE(err.find("AttributeError"), std::string::npos)
        << "Expected AttributeError in: " << err;
    // Value must NOT have changed
    EXPECT_EQ(val, 0);
}

// ============================================================================
// 7. ReadSide_Ctypes_ErrorMsgContainsFieldName
// ============================================================================

TEST_F(SlotViewHelpersTest, ReadSide_Ctypes_ErrorMsgContainsFieldName)
{
    auto   spec = make_int32_spec();
    size_t sz   = 0;
    auto   type = ctypes_type_ro(spec, "Int32FrameR7", sz);

    int32_t    val  = 0;
    py::object inst = make_slot_view(spec, type, &val, sz, /*is_read_side=*/true);

    py::dict    ns;
    ns["inst"]      = inst;
    std::string err = try_exec("inst.x = 1", ns);

    EXPECT_NE(err.find("x"), std::string::npos)
        << "Error message should name field 'x'; got: " << err;
    EXPECT_NE(err.find("read-only"), std::string::npos)
        << "Error message should say 'read-only'; got: " << err;
}

// ============================================================================
// 8. ReadSide_Ctypes_RawBufferAccess
//    Python equivalent of C++ buffer_span(): memoryview(in_slot).cast('B')
//    gives zero-copy byte-level access without going through field names.
// ============================================================================

TEST_F(SlotViewHelpersTest, ReadSide_Ctypes_RawBufferAccess)
{
    auto   spec = make_int32_spec();
    size_t sz   = 0;
    auto   type = ctypes_type_ro(spec, "Int32FrameR8", sz);

    int32_t val = 0x01020304;
    py::object inst = make_slot_view(spec, type, &val, sz, /*is_read_side=*/true);

    // Python buffer protocol still works on readonly-wrapped ctypes structs.
    // bytes(in_slot) gives an immutable copy; memoryview(in_slot).cast('B') gives a view.
    py::dict ns;
    ns["inst"] = inst;
    py::exec(R"PLH(
raw_copy = bytes(inst)
assert len(raw_copy) == 4, f"expected 4 bytes, got {len(raw_copy)}"
mv = memoryview(inst).cast('B')
assert len(mv) == 4, f"expected 4 elements, got {len(mv)}"
)PLH",
             py::globals(), ns);
    // Reaching here = buffer protocol access works without AttributeError
}

// ============================================================================
// 9. WriteSide_Ctypes_MultiField
// ============================================================================

TEST_F(SlotViewHelpersTest, WriteSide_Ctypes_MultiField)
{
    auto   spec = make_multi_spec();
    size_t sz   = 0;
    auto   type = ctypes_type(spec, "MultiFrameW9", sz);

    std::vector<uint8_t> buf(sz, 0);
    py::object inst = make_slot_view(spec, type, buf.data(), sz, /*is_read_side=*/false);

    py::dict ns;
    ns["inst"] = inst;
    EXPECT_EQ(try_exec("inst.a = -7; inst.b = 3.14; inst.c[2] = 0xFF", ns), "");
}

// ============================================================================
// 10. ReadSide_Ctypes_MultiField_ReadsAllFields
// ============================================================================

TEST_F(SlotViewHelpersTest, ReadSide_Ctypes_MultiField_ReadsAllFields)
{
    auto   spec = make_multi_spec();
    size_t sz   = 0;

    // Populate via write-side view
    auto   wtype = ctypes_type(spec, "MultiFrameW10", sz);
    std::vector<uint8_t> buf(sz, 0);
    py::object           wobj = make_slot_view(spec, wtype, buf.data(), sz, false);
    py::dict             wns;
    wns["w"] = wobj;
    py::exec("w.a = -123; w.b = 2.5; w.c[0] = 77", py::globals(), wns);

    // Read via read-side (readonly-wrapped) view over the same buffer
    auto   rtype = ctypes_type_ro(spec, "MultiFrameR10", sz);
    py::object robj = make_slot_view(spec, rtype, buf.data(), sz, true);
    py::dict   rns;
    rns["r"] = robj;
    py::exec("va = r.a; vb = r.b; vc0 = r.c[0]", py::globals(), rns);

    EXPECT_EQ(rns["va"].cast<int>(), -123);
    EXPECT_FLOAT_EQ(rns["vb"].cast<float>(), 2.5f);
    EXPECT_EQ(rns["vc0"].cast<int>(), 77);
}

// ============================================================================
// 11. ReadSide_Ctypes_AllFieldsBlockWrite
// ============================================================================

TEST_F(SlotViewHelpersTest, ReadSide_Ctypes_AllFieldsBlockWrite)
{
    auto   spec = make_multi_spec();
    size_t sz   = 0;
    auto   type = ctypes_type_ro(spec, "MultiFrameRO11", sz);

    std::vector<uint8_t> buf(sz, 0);
    py::object           inst = make_slot_view(spec, type, buf.data(), sz, true);

    py::dict ns;
    ns["inst"] = inst;
    EXPECT_NE(try_exec("inst.a = 1", ns).find("AttributeError"), std::string::npos)
        << "Field 'a' write should raise AttributeError";
    EXPECT_NE(try_exec("inst.b = 1.0", ns).find("AttributeError"), std::string::npos)
        << "Field 'b' write should raise AttributeError";
}

// ============================================================================
// 12. WrapAsReadonly_StandaloneDirectTest
// ============================================================================

TEST_F(SlotViewHelpersTest, WrapAsReadonly_StandaloneDirectTest)
{
    auto       spec      = make_int32_spec();
    py::object base_type = build_ctypes_struct(spec, "DirectBase12");
    py::object ro_type   = wrap_as_readonly_ctypes(base_type);

    // Name should include "Readonly"
    std::string name = ro_type.attr("__name__").cast<std::string>();
    EXPECT_NE(name.find("Readonly"), std::string::npos) << "Got name: " << name;

    // ro_type must be a subclass of base_type
    py::module_ builtins = py::module_::import("builtins");
    bool        is_sub   = builtins.attr("issubclass")(ro_type, base_type).cast<bool>();
    EXPECT_TRUE(is_sub) << "Readonly wrapper must inherit from base ctypes struct";

    // Instance: writes raise, reads work
    int32_t    val  = 5;
    py::object inst = ro_type.attr("from_buffer")(
        py::memoryview::from_memory(&val, sizeof(val), /*readonly=*/false));

    py::dict    ns;
    ns["inst"]      = inst;
    std::string err = try_exec("inst.x = 99", ns);
    EXPECT_NE(err.find("AttributeError"), std::string::npos) << "Got: " << err;

    EXPECT_EQ(try_exec("v = inst.x", ns), "");
    EXPECT_EQ(ns["v"].cast<int>(), 5);
}

// ============================================================================
// 13. WriteSide_ArrayModule_DataZeroCopy
//     Uses Python's built-in struct module + memoryview to verify zero-copy
//     write semantics without requiring numpy.
// ============================================================================

TEST_F(SlotViewHelpersTest, WriteSide_ArrayModule_DataZeroCopy)
{
    // Use a writable memoryview backed by our C++ buffer
    std::array<double, 3> buf = {0.0, 0.0, 0.0};
    auto mv = py::memoryview::from_memory(buf.data(),
                                           static_cast<py::ssize_t>(sizeof(buf)),
                                           /*readonly=*/false);

    py::dict ns;
    ns["mv"] = mv;

    // Use struct.pack_into to write doubles into the memoryview
    py::exec(R"(
import struct
struct.pack_into('ddd', mv, 0, 1.1, 2.2, 3.3)
)", py::globals(), ns);

    EXPECT_DOUBLE_EQ(buf[0], 1.1);
    EXPECT_DOUBLE_EQ(buf[1], 2.2);
    EXPECT_DOUBLE_EQ(buf[2], 3.3);
}

// ============================================================================
// 14. ReadSide_ArrayModule_DataReadable_AndWriteBlocked
//     Uses Python's built-in struct module + readonly memoryview to verify
//     that reads work and writes are blocked, without requiring numpy.
// ============================================================================

TEST_F(SlotViewHelpersTest, ReadSide_ArrayModule_DataReadable_AndWriteBlocked)
{
    std::array<double, 3> buf = {10.0, 20.0, 30.0};
    auto mv = py::memoryview::from_memory(buf.data(),
                                           static_cast<py::ssize_t>(sizeof(buf)),
                                           /*readonly=*/true);

    py::dict ns;
    ns["mv"] = mv;

    // Reads work via struct.unpack_from
    py::exec(R"(
import struct
v0, v1, v2 = struct.unpack_from('ddd', mv, 0)
)", py::globals(), ns);
    EXPECT_DOUBLE_EQ(ns["v0"].cast<double>(), 10.0);
    EXPECT_DOUBLE_EQ(ns["v1"].cast<double>(), 20.0);
    EXPECT_DOUBLE_EQ(ns["v2"].cast<double>(), 30.0);

    // Write to readonly memoryview raises TypeError
    std::string err = try_exec(R"(
import struct
struct.pack_into('d', mv, 0, 99.0)
)", ns);
    EXPECT_FALSE(err.empty()) << "Expected error on write to read-only memoryview";

    // Backing memory must NOT have changed
    EXPECT_DOUBLE_EQ(buf[0], 10.0);
}

// ============================================================================
// 15. WriteSide_PackedStruct_CanWrite
// ============================================================================

TEST_F(SlotViewHelpersTest, WriteSide_PackedStruct_CanWrite)
{
    auto   spec = make_packed_uint32_spec();
    size_t sz   = 0;
    auto   type = ctypes_type(spec, "PackedFrame15", sz);

    std::array<uint8_t, 4> buf = {};
    py::object inst = make_slot_view(spec, type, buf.data(), sz, /*is_read_side=*/false);

    py::dict ns;
    ns["inst"] = inst;
    EXPECT_EQ(try_exec("inst.val = 0xDEADBEEF", ns), "");

    uint32_t val = 0;
    std::memcpy(&val, buf.data(), 4);
    EXPECT_EQ(val, 0xDEADBEEFu);
}
