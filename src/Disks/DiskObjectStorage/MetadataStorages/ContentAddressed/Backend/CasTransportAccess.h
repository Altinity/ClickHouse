#pragma once

namespace DB::Cas
{

/// A capability token: holding one proves the holder is `CasRequests`. Not copyable, not
/// constructible outside that one friend, and carries no data -- its only job is to gate access at
/// compile time to the backend entry points that must not be called except through the contract.
class TransportAccess
{
    friend class CasRequests;
    TransportAccess() = default;

public:
    TransportAccess(const TransportAccess &) = delete;
    TransportAccess & operator=(const TransportAccess &) = delete;
};

}
