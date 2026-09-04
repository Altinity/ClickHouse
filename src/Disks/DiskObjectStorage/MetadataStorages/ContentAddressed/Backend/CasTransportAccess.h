#pragma once

namespace DB::Cas
{

/// A capability token: holding one proves the holder is `CasRequests` (or, during the migration,
/// `Backend` -- deleted at the lock, Task 20). Not copyable, not constructible outside those two
/// friends, and carries no data -- its only job is to gate access at compile time to the backend
/// entry points that must not be called except through the contract.
class TransportAccess
{
    friend class CasRequests;
    friend class Backend;                    /// migration only; deleted at the lock (Task 20)
    TransportAccess() = default;

public:
    TransportAccess(const TransportAccess &) = delete;
    TransportAccess & operator=(const TransportAccess &) = delete;
};

}
