#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

Gc::Gc(StorePtr store_, UInt128 gc_id_)
    : store(std::move(store_))
    , gc_id(gc_id_)
{
    if (!store)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: store must not be null");
    /// owner 0 in GcLease means "never held" — a leader with id 0 would be indistinguishable from it.
    if (gc_id == UInt128(0))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: gc_id must not be 0 (reserved for 'lease never held')");
}

RoundReport Gc::runRegularRound()
{
    RoundReport report;
    GcState state;
    Token state_token;
    report.acquired_lease = acquireOrRenewLease(state, state_token);
    if (!report.acquired_lease)
        return report;

    report.round = state.round;
    /// Tasks 6-12: fold / retire / fence / recheck / cascade / trim
    return report;
}

void Gc::rememberObservation(const GcLease & lease)
{
    has_observation = true;
    last_seen_owner = lease.owner;
    last_seen_seq = lease.seq;
}

bool Gc::acquireOrRenewLease(GcState & state, Token & state_token)
{
    const String key = store->layout().gcStateKey();

    /// Bounded: at most 2 CAS attempts per call. Iteration 2 is reached only after a create or
    /// renew Conflict (the lost-steal path returns directly — a contender that loses the steal
    /// race must re-enter the observation protocol on its NEXT round, never retry blindly).
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto got = store->backend().get(key);

        if (!got)
        {
            /// Step 1: fresh pool — create gc/state holding our lease (create-if-absent CAS).
            GcState fresh;
            fresh.lease = GcLease{gc_id, 1};
            Token committed_token;
            if (store->backend().casPut(key, encodeGcState(fresh), /*expected*/ std::nullopt, &committed_token)
                == CasOutcome::Committed)
            {
                rememberObservation(fresh.lease);
                state = std::move(fresh);
                state_token = committed_token;
                return true;
            }
            /// A racer created it first — re-read and fall through to renew/observe/steal.
            continue;
        }

        GcState current = decodeGcState(got->bytes);

        if (current.lease.owner == gc_id)
        {
            /// Step 2: renew — seq advances, fence_seq does NOT (renewal is not a new epoch).
            GcState next = current;
            ++next.lease.seq;
            Token committed_token;
            if (store->backend().casPut(key, encodeGcState(next), got->token, &committed_token)
                == CasOutcome::Committed)
            {
                /// Remember the COMMITTED (owner, seq) so the next renew never self-triggers
                /// a steal-window anomaly.
                rememberObservation(next.lease);
                state = std::move(next);
                state_token = committed_token;
                return true;
            }
            /// Someone moved the lease under us (a steal happened) — re-read once; if the owner
            /// is still us, retry the renew once; else the foreign-owner branch records the
            /// observation and backs off (our remembered observation carries owner == gc_id, so
            /// it can never match a foreign owner and turn this into a steal).
            continue;
        }

        /// Foreign owner.
        const bool incumbent_renewed = !has_observation
            || current.lease.owner != last_seen_owner
            || current.lease.seq != last_seen_seq;
        if (incumbent_renewed)
        {
            /// Step 3: the incumbent is alive (or this is our first sight of this lease) —
            /// record the observation and back off. Eligibility to steal requires seeing the
            /// SAME (owner, seq) across one whole prior round attempt of OURS.
            rememberObservation(current.lease);
            return false;
        }

        /// Step 4: the incumbent did not renew across our full observation window — steal.
        /// fence_seq++ opens a new leadership epoch: the new leader's retire/outcome paths
        /// (<round>.<fence_seq>) never collide with the old leader's (append-by-unique-path).
        GcState next = current;
        next.lease.owner = gc_id;
        ++next.lease.seq;
        ++next.fence_seq;
        Token committed_token;
        if (store->backend().casPut(key, encodeGcState(next), got->token, &committed_token)
            == CasOutcome::Committed)
        {
            rememberObservation(next.lease);
            state = std::move(next);
            state_token = committed_token;
            return true;
        }

        /// A racer stole (or the incumbent revived and renewed) first — our CAS carried the token
        /// we read BEFORE its write, so it lost. Re-read, record what is there now, back off.
        if (const auto reread = store->backend().get(key))
            rememberObservation(decodeGcState(reread->bytes).lease);
        else
            has_observation = false;   /// gc/state vanished (never expected — it is never deleted)
        return false;
    }

    /// Two CAS attempts exhausted (create/renew conflicted twice) — heavy contention on gc/state
    /// means another leader is moving it; back off, this round.
    return false;
}

}
