#include <Storages/MergeTree/DataPartsExchangeCasRouting.h>

#include <base/sort.h>

#include <algorithm>

#include <boost/algorithm/string/join.hpp>

namespace DB::DataPartsExchange
{

namespace
{
const String CAS_POOL_ADVERTISE_DELIMITER = ", ";
}

String encodeCasPoolAdvertise(Strings pool_uuids)
{
    std::erase_if(pool_uuids, [](const String & id) { return id.empty(); });
    ::sort(pool_uuids.begin(), pool_uuids.end());
    pool_uuids.erase(std::unique(pool_uuids.begin(), pool_uuids.end()), pool_uuids.end());
    return boost::algorithm::join(pool_uuids, CAS_POOL_ADVERTISE_DELIMITER);
}

Strings decodeCasPoolAdvertise(const String & text)
{
    Strings pools;
    if (text.empty())
        return pools;

    size_t pos_start = 0;
    while (true)
    {
        const size_t pos_end = text.find(CAS_POOL_ADVERTISE_DELIMITER, pos_start);
        if (pos_end == String::npos)
        {
            pools.push_back(text.substr(pos_start));
            return pools;
        }
        pools.push_back(text.substr(pos_start, pos_end - pos_start));
        pos_start = pos_end + CAS_POOL_ADVERTISE_DELIMITER.size();
    }
}

String resolveOfferedCasPool(const Strings & advertised_pools, const String & offered_pool_cookie)
{
    if (!offered_pool_cookie.empty())
        return offered_pool_cookie;
    if (advertised_pools.size() == 1)
        return advertised_pools.front();
    return {};
}

std::optional<size_t> resolveForcedCaCandidate(
    const std::vector<CasRelinkCandidate> & candidates,
    const Strings & advertised_pools,
    const String & offered_pool_cookie)
{
    const String offered_pool = resolveOfferedCasPool(advertised_pools, offered_pool_cookie);
    if (offered_pool.empty())
        return std::nullopt;

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto & candidate = candidates[i];
        if (!candidate.read_only && !candidate.pool_uuid.empty() && candidate.pool_uuid == offered_pool)
            return i;
    }
    return std::nullopt;
}

std::optional<size_t> resolveConfirmRoutingCandidate(
    const std::vector<CasConfirmRoutingCandidate> & candidates,
    const String & pool_uuid)
{
    if (pool_uuid.empty())
        return std::nullopt;

    std::optional<size_t> matched;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto & candidate = candidates[i];
        if (candidate.pool_uuid != pool_uuid || !candidate.owns_namespace)
            continue;
        if (!matched)
        {
            matched = i;
            continue;
        }
        /// A second DISTINCT mount owning the namespace: ambiguous. An alias of the first is not.
        if (candidates[*matched].exchange_identity != candidate.exchange_identity)
            return std::nullopt;
    }
    return matched;
}

}
