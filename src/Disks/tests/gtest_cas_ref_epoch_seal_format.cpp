#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <vector>

/// v3 text codec tests for the `EpochSeal` record kind + strict seal grammar added to `cas_ref_log`
/// (stage A task 1, spec INV-2). Split into its own file per the plan's "prefer NEW test files"
/// constraint, rather than extending `gtest_cas_ref_log_format.cpp`. Covers: the new op kind's round
/// trip (including the meta-line `prev_epoch_seal` field), the context-free structural grammar
/// (`validateEpochSealGrammarStructural`, run by both `encodeRefLogTxn` and `decodeRefLogTxn`), and
/// the contextual required-iff rule (`validateEpochSealGrammarContextual`, exercised directly against
/// explicit `life_epoch` values -- its writer-runtime call sites land in later tasks).

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

namespace
{

RefOp epochSealOp()
{
    RefOp op;
    op.kind = RefOpKind::EpochSeal;
    return op;
}

RefOp namespaceBirthOp()
{
    RefOp op;
    op.kind = RefOpKind::NamespaceBirth;
    return op;
}

}

/// ===================================================================================
/// refLogTxnIsEpochSeal / refLogTxnIsRemovalClass classification
/// ===================================================================================

TEST(CasRefEpochSealFormat, IsEpochSealTrueForSoleSealOp)
{
    RefLogTxn txn;
    txn.ops.push_back(epochSealOp());
    EXPECT_TRUE(refLogTxnIsEpochSeal(txn));
}

TEST(CasRefEpochSealFormat, IsEpochSealFalseForSealPlusOtherOp)
{
    RefLogTxn txn;
    txn.ops.push_back(epochSealOp());
    txn.ops.push_back(namespaceBirthOp());
    EXPECT_FALSE(refLogTxnIsEpochSeal(txn));
}

TEST(CasRefEpochSealFormat, IsEpochSealFalseForNonSealOp)
{
    RefLogTxn txn;
    txn.ops.push_back(namespaceBirthOp());
    EXPECT_FALSE(refLogTxnIsEpochSeal(txn));
}

TEST(CasRefEpochSealFormat, IsEpochSealFalseForEmptyOps)
{
    RefLogTxn txn;
    EXPECT_FALSE(refLogTxnIsEpochSeal(txn));
}

/// Step 3's explicit regression note: an `EpochSeal`-only op vector is not removal-class.
TEST(CasRefEpochSealFormat, RemovalClassIsFalseForEpochSeal)
{
    std::vector<RefOp> ops{epochSealOp()};
    EXPECT_FALSE(refLogTxnIsRemovalClass(ops));
}

/// ===================================================================================
/// Round trip
/// ===================================================================================

TEST(CasRefEpochSealFormat, RoundTripSealAtSequenceOneWithPrevEpochSeal)
{
    /// An empty dead epoch (3) closes with a sequence-1 seal, which is therefore itself required to
    /// carry `prev_epoch_seal` chaining to the seal that closed epoch 2 (spec INV-2's grammar: required
    /// on exactly sequence 1 of every epoch above genesis).
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{3, 1};
    txn.prev_epoch_seal = RefTxnId{2, 9};
    txn.ops.push_back(epochSealOp());

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
    ASSERT_TRUE(decoded.prev_epoch_seal.has_value());
    EXPECT_EQ(*decoded.prev_epoch_seal, (RefTxnId{2, 9}));
    ASSERT_EQ(decoded.ops.size(), 1u);
    EXPECT_EQ(decoded.ops[0].kind, RefOpKind::EpochSeal);
}

TEST(CasRefEpochSealFormat, RoundTripSealWithoutPrevEpochSeal)
{
    /// The common case: epoch 2 had real records (greatest applied sequence 5), so its closing seal
    /// lands at sequence 6 -- not sequence 1 -- and therefore must NOT carry `prev_epoch_seal`.
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{2, 6};
    txn.ops.push_back(epochSealOp());

    const String bytes = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded, txn);
    EXPECT_FALSE(decoded.prev_epoch_seal.has_value());
}

/// A re-encode of a decoded seal transaction is byte-identical (the encoder is a pure function of the
/// txn), matching the pin `gtest_cas_ref_log_format.cpp` keeps for the other op kinds.
TEST(CasRefEpochSealFormat, ByteIdenticalReencodeWithPrevEpochSeal)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{3, 1};
    txn.prev_epoch_seal = RefTxnId{2, 9};
    txn.ops.push_back(epochSealOp());

    const String bytes1 = encodeRefLogTxn(txn);
    const RefLogTxn decoded = decodeRefLogTxn(bytes1, txn.ns, txn.txn_id);
    const String bytes2 = encodeRefLogTxn(decoded);
    EXPECT_EQ(bytes1, bytes2);
}

/// ===================================================================================
/// Structural grammar (validateEpochSealGrammarStructural, via encode/decode -- context-free)
/// ===================================================================================

TEST(CasRefEpochSealFormat, EncodeRejectsSealTxnWithTwoSealOps)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    txn.ops.push_back(epochSealOp());
    txn.ops.push_back(epochSealOp());
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefEpochSealFormat, EncodeRejectsSealTxnWithSecondNonSealOp)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    txn.ops.push_back(epochSealOp());
    txn.ops.push_back(namespaceBirthOp());
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

TEST(CasRefEpochSealFormat, EncodeRejectsPrevEpochSealAtNonUnitSequence)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 2};
    txn.prev_epoch_seal = RefTxnId{1, 1};
    txn.ops.push_back(namespaceBirthOp());
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodeRefLogTxn(txn); });
}

/// Decode-side splice: `prev_epoch_seal` present as only one of its two wire fields ("pse" without
/// "pss") -- a shape only reachable via corrupted bytes, since the encoder always writes both
/// together. Boundary-plus-one for the additive-field decode contract (Constraint 7).
TEST(CasRefEpochSealFormat, DecodeRejectsPrevEpochSealMissingPssComponent)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{3, 1};
    txn.prev_epoch_seal = RefTxnId{2, 9};
    txn.ops.push_back(epochSealOp());
    const String bytes = encodeRefLogTxn(txn);

    const String needle = ",\"pss\":\"9\"";
    const auto pos = bytes.find(needle);
    ASSERT_NE(pos, String::npos);
    String tampered = bytes;
    tampered.erase(pos, needle.size());

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefLogTxn(tampered, txn.ns, txn.txn_id); });
}

/// ===================================================================================
/// Contextual grammar (validateEpochSealGrammarContextual, called directly against explicit
/// life_epoch values -- the writer-runtime call sites are wired by later tasks)
/// ===================================================================================

TEST(CasRefEpochSealFormat, ContextualRejectsMissingPrevEpochSealWhenRequired)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{3, 1};
    txn.ops.push_back(namespaceBirthOp());
    /// life_epoch 1 < writer_epoch 3: a sequence-1 txn above genesis MUST carry prev_epoch_seal.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { validateEpochSealGrammarContextual(txn, /*life_epoch=*/1); });
}

TEST(CasRefEpochSealFormat, ContextualRejectsPrevEpochSealWhenForbidden)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{5, 1};
    txn.prev_epoch_seal = RefTxnId{4, 3};
    txn.ops.push_back(namespaceBirthOp());
    /// life_epoch == writer_epoch == 5: this IS the namespace's genesis sequence-1 txn, so
    /// prev_epoch_seal is forbidden -- there is no preceding epoch to chain to.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { validateEpochSealGrammarContextual(txn, /*life_epoch=*/5); });
}

/// codex r2 finding 2: "genesis" is per-namespace. A namespace first born at global epoch 5 (not
/// epoch 1) appends {5, 1} with NO prev_epoch_seal -- that IS its genesis, not a transition.
TEST(CasRefEpochSealFormat, ContextualAllowsGenesisBirthAboveEpochOneWithoutPrevEpochSeal)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{5, 1};
    txn.ops.push_back(namespaceBirthOp());
    EXPECT_NO_THROW(validateEpochSealGrammarContextual(txn, /*life_epoch=*/5));
}

/// ===================================================================================
/// Regression guard: existing unknown-op-word behavior stays intact after adding "epoch_seal"
/// ===================================================================================

TEST(CasRefEpochSealFormat, DecodeRejectsUnknownOpWordRegressionGuard)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    txn.ops.push_back(epochSealOp());
    const String bytes = encodeRefLogTxn(txn);

    const String needle = "\"epoch_seal\"";
    const auto pos = bytes.find(needle);
    ASSERT_NE(pos, String::npos);
    String tampered = bytes;
    tampered.replace(pos, needle.size(), "\"totally_bogus_op\"");

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodeRefLogTxn(tampered, txn.ns, txn.txn_id); });
}

/// ===================================================================================
/// Shape-level failure-mode battery (truncation / v+1 gate / wrong type / leading garbage)
/// ===================================================================================

TEST(CasRefEpochSealFormat, FormatBatteryEpochSeal)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{3, 1};
    txn.prev_epoch_seal = RefTxnId{2, 9};
    txn.ops.push_back(epochSealOp());

    const String ns = txn.ns;
    const RefTxnId id = txn.txn_id;
    runFormatBattery({FormatId::RefLog,
        [txn] { return sealObject(FormatId::RefLog, encodeRefLogTxn(txn)); },
        [ns, id](std::string_view s) { decodeRefLogTxn(openObject(FormatId::RefLog, s), ns, id); },
        "{\"type\":\"cas_ref_log\",\"v\":3}\n"
        "{\"ns\":\"ns\",\"we\":\"3\",\"rs\":\"1\",\"pse\":\"2\",\"pss\":\"9\"}\n"
        "{\"op\":\"epoch_seal\"}\n"
        "{\"n\":1}\n"});
}
