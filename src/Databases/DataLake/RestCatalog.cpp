#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Net/HTTPRequest.h>
#include <Access/AccessControl.h>
#include <Access/ForwardedAuthToken.h>
#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/RemoteHostFilter.h>
#include <Common/logger_useful.h>
#include <Common/setThreadName.h>
#include <Common/CurrentThread.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergWrites.h>
#include <mutex>
#include <chrono>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <Core/SettingsEnums.h>
#include "config.h"

#if USE_AVRO
#include <Databases/DataLake/RestCatalog.h>
#include <Databases/DataLake/DatabaseDataLakeSettings.h>
#include <Databases/DataLake/StorageCredentials.h>

#include <base/find_symbols.h>
#include <Core/Settings.h>
#include <Common/escapeForFileName.h>
#include <Common/threadPoolCallbackRunner.h>
#include <Common/Base64.h>
#include <Common/checkStackSize.h>
#include <Common/HTTPHeaderFilter.h>

#include <IO/ConnectionTimeouts.h>
#include <IO/GCPOAuth.h>
#include <IO/HTTPCommon.h>
#include <IO/ReadBuffer.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>
#include <Interpreters/Context.h>
#include <filesystem>

#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadata.h>
#include <Server/HTTP/HTMLForm.h>
#include <Formats/FormatFactory.h>

#include <Poco/URI.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/StreamCopier.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Common/FailPoint.h>
#include <Poco/DateTime.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/DateTimeParser.h>
#include <Poco/StringTokenizer.h>
#include <Poco/Timestamp.h>
#include <fmt/ranges.h>


namespace DB::ErrorCodes
{
    extern const int DATALAKE_DATABASE_ERROR;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int FAULT_INJECTED;
    extern const int NOT_IMPLEMENTED;
    extern const int CATALOG_NAMESPACE_DISABLED;
    extern const int CATALOG_USER_TOKEN_NOT_AVAILABLE;
}

namespace DB::Setting
{
    extern const SettingsBool allow_experimental_geo_types_in_iceberg;
}

namespace DB::FailPoints
{
    extern const char check_database_datalake_negative[];
    extern const char iceberg_alter_catalog_update_schema_fail[];
    extern const char iceberg_alter_catalog_commit_reported_as_failed[];
}

namespace ProfileEvents
{
    extern const Event DataLakeRestCatalogCredentialsVended;
    extern const Event DataLakeRestCatalogCredentialsCacheHits;
    extern const Event DataLakeRestCatalogCredentialsCacheMisses;
    extern const Event DataLakeRestCatalogTokenExchange;
    extern const Event DataLakeRestCatalogTokenExchangeMicroseconds;
    extern const Event DataLakeRestCatalogTokenExchangeFailures;
    extern const Event DataLakeRestCatalogUserTokenCacheHits;
    extern const Event DataLakeRestCatalogClientCredentialsGrants;
    extern const Event DataLakeRestCatalogLoadConfig;
    extern const Event DataLakeRestCatalogLoadConfigMicroseconds;
    extern const Event DataLakeRestCatalogGetNamespaces;
    extern const Event DataLakeRestCatalogGetNamespacesMicroseconds;
    extern const Event DataLakeRestCatalogGetTables;
    extern const Event DataLakeRestCatalogGetTablesMicroseconds;
    extern const Event DataLakeRestCatalogGetTableMetadata;
    extern const Event DataLakeRestCatalogGetTableMetadataMicroseconds;
    extern const Event DataLakeRestCatalogGetCredentials;
    extern const Event DataLakeRestCatalogGetCredentialsMicroseconds;
    extern const Event DataLakeRestCatalogAuthTokenCachedValid;
    extern const Event DataLakeRestCatalogAuthTokenRetrieve;
    extern const Event DataLakeRestCatalogAuthTokenRefreshedMicroseconds;
    extern const Event DataLakeRestCatalogUnauthorized;
    extern const Event DataLakeRestCatalogCreateNamespace;
    extern const Event DataLakeRestCatalogCreateNamespaceMicroseconds;
    extern const Event DataLakeRestCatalogCreateTable;
    extern const Event DataLakeRestCatalogCreateTableMicroseconds;
    extern const Event DataLakeRestCatalogUpdateTable;
    extern const Event DataLakeRestCatalogUpdateTableMicroseconds;
    extern const Event DataLakeRestCatalogDropTable;
    extern const Event DataLakeRestCatalogDropTableMicroseconds;
}

namespace CurrentMetrics
{
    extern const Metric DataLakeCatalogUserTokenCacheBytes;
    extern const Metric DataLakeCatalogUserTokenCacheEntries;
}

namespace DB::DatabaseDataLakeSetting
{
    extern const DatabaseDataLakeSettingsString catalog_credential;
    extern const DatabaseDataLakeSettingsString auth_header;
    extern const DatabaseDataLakeSettingsString onelake_tenant_id;
    extern const DatabaseDataLakeSettingsString onelake_bearer_token;
    extern const DatabaseDataLakeSettingsString onelake_client_id;
    extern const DatabaseDataLakeSettingsString onelake_client_secret;
}

namespace DataLake
{

static constexpr auto CONFIG_ENDPOINT = "config";
static constexpr auto NAMESPACES_ENDPOINT = "namespaces";

namespace
{

String parseTableUuid(const Poco::JSON::Object::Ptr & metadata_object)
{
    if (metadata_object && metadata_object->has("table-uuid"))
        return metadata_object->get("table-uuid").extract<String>();
    return {};
}

std::pair<std::string, std::string> parseCatalogCredential(const std::string & catalog_credential)
{
    /// Parse a string of format "<client_id>:<client_secret>"
    /// into separare strings client_id and client_secret.

    std::string client_id;
    std::string client_secret;
    if (!catalog_credential.empty())
    {
        auto pos = catalog_credential.find(':');
        if (pos == std::string::npos)
        {
            throw DB::Exception(
                DB::ErrorCodes::BAD_ARGUMENTS, "Unexpected format of catalog credential: "
                "expected client_id and client_secret separated by `:`");
        }
        client_id = catalog_credential.substr(0, pos);
        client_secret = catalog_credential.substr(pos + 1);
    }
    return std::pair(client_id, client_secret);
}

DB::HTTPHeaderEntry parseAuthHeader(const std::string & auth_header)
{
    /// Parse a string of format "Authorization: <auth_scheme> <auth_token>"
    /// into a key-value header "Authorization", "<auth_scheme> <auth_token>"

    auto pos = auth_header.find(':');
    if (pos == std::string::npos)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "Unexpected format of auth header");

    return DB::HTTPHeaderEntry(auth_header.substr(0, pos), auth_header.substr(pos + 1));
}

/// Percent-encodes one `application/x-www-form-urlencoded` value.
///
/// `Poco::URI::encode(str, reserved, out)` takes the set of *reserved characters* as its second
/// argument; passing the value itself (as the pre-existing `client_credentials` path did) makes
/// every character of the value reserved and therefore escapes all of them. That is accidentally
/// safe but would triple the size of a 4 KB JWT, so the exchange, which carries exactly such a
/// value, encodes properly.
std::string formUrlEncode(const std::string & value)
{
    std::string encoded;
    Poco::URI::encode(value, "!$&'()*+,;=:@/?", encoded);
    return encoded;
}

std::string correctAPIURI(const std::string & uri)
{
    if (uri.ends_with("v1"))
        return uri;
    return std::filesystem::path(uri) / "v1";
}

String encodeNamespaceForURI(const String & namespace_name)
{
    String encoded;
    for (const auto & ch : namespace_name)
    {
        if (ch == '.')
            encoded += "%1F";
        else
            encoded.push_back(ch);
    }
    return encoded;
}

std::unordered_set<std::string> getAllowedBigLakeMetadataServiceHosts(
    const Poco::Util::AbstractConfiguration & config)
{
    static constexpr auto SECTION = "iceberg_biglake_metadata_service_hosts";
    std::unordered_set<std::string> allowed;
    if (!config.has(SECTION))
        return allowed;

    std::vector<std::string> keys;
    config.keys(SECTION, keys);
    for (const auto & key : keys)
        allowed.insert(config.getString(std::string(SECTION) + "." + key));
    return allowed;
}


}

namespace
{

constexpr auto IDENTIFIER_FIELD_IDS = "identifier-field-ids";

Poco::JSON::Object::Ptr cloneJsonObject(const Poco::JSON::Object::Ptr & obj)
{
    std::ostringstream oss; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    obj->stringify(oss);
    Poco::JSON::Parser parser;
    return parser.parse(oss.str()).extract<Poco::JSON::Object::Ptr>();
}

bool icebergJsonValueEquals(const Poco::Dynamic::Var & lhs, const Poco::Dynamic::Var & rhs);

bool icebergJsonObjectEquals(const Poco::JSON::Object::Ptr & lhs, const Poco::JSON::Object::Ptr & rhs)
{
    if (lhs.isNull() || rhs.isNull())
        return lhs.isNull() && rhs.isNull();
    if (lhs->size() != rhs->size())
        return false;
    for (auto it = lhs->begin(); it != lhs->end(); ++it)
    {
        if (!rhs->has(it->first))
            return false;
        if (!icebergJsonValueEquals(it->second, rhs->get(it->first)))
            return false;
    }
    return true;
}

bool icebergJsonArrayEquals(const Poco::JSON::Array::Ptr & lhs, const Poco::JSON::Array::Ptr & rhs)
{
    if (lhs.isNull() || rhs.isNull())
        return lhs.isNull() && rhs.isNull();
    if (lhs->size() != rhs->size())
        return false;
    for (UInt32 i = 0; i < lhs->size(); ++i)
        if (!icebergJsonValueEquals(lhs->get(i), rhs->get(i)))
            return false;
    return true;
}

/// Structural, key-order-independent comparison of two parsed JSON values.
bool icebergJsonValueEquals(const Poco::Dynamic::Var & lhs, const Poco::Dynamic::Var & rhs)
{
    const bool lhs_is_object = lhs.type() == typeid(Poco::JSON::Object::Ptr);
    const bool rhs_is_object = rhs.type() == typeid(Poco::JSON::Object::Ptr);
    if (lhs_is_object || rhs_is_object)
    {
        if (!(lhs_is_object && rhs_is_object))
            return false;
        return icebergJsonObjectEquals(lhs.extract<Poco::JSON::Object::Ptr>(), rhs.extract<Poco::JSON::Object::Ptr>());
    }
    const bool lhs_is_array = lhs.type() == typeid(Poco::JSON::Array::Ptr);
    const bool rhs_is_array = rhs.type() == typeid(Poco::JSON::Array::Ptr);
    if (lhs_is_array || rhs_is_array)
    {
        if (!(lhs_is_array && rhs_is_array))
            return false;
        return icebergJsonArrayEquals(lhs.extract<Poco::JSON::Array::Ptr>(), rhs.extract<Poco::JSON::Array::Ptr>());
    }
    return lhs.toString() == rhs.toString();
}

/// Two Iceberg schemas are equivalent when they differ only by their `schema-id`.
/// `identifier-field-ids` is ignored as well: a schema committed through this catalog always
/// carries it, while a freshly generated one does not, and an absent list means the same as an
/// empty one.
bool schemasEquivalentIgnoringId(const Poco::JSON::Object::Ptr & lhs, const Poco::JSON::Object::Ptr & rhs)
{
    Poco::JSON::Object::Ptr lhs_copy = cloneJsonObject(lhs);
    Poco::JSON::Object::Ptr rhs_copy = cloneJsonObject(rhs);
    for (auto * copy : {&lhs_copy, &rhs_copy})
    {
        (*copy)->remove(DB::Iceberg::f_schema_id);
        if (auto identifier_field_ids = (*copy)->getArray(IDENTIFIER_FIELD_IDS);
            identifier_field_ids.isNull() || identifier_field_ids->size() == 0)
            (*copy)->remove(IDENTIFIER_FIELD_IDS);
    }
    return icebergJsonObjectEquals(lhs_copy, rhs_copy);
}

}

Poco::JSON::Object::Ptr buildUpdateSchemaRequestBody(
    const String & namespace_name,
    const String & table_name,
    Poco::JSON::Object::Ptr metadata,
    Poco::JSON::Object::Ptr new_schema,
    Int32 previous_schema_id,
    Int32 new_last_column_id)
{
    Poco::JSON::Object::Ptr request_body = new Poco::JSON::Object;
    {
        Poco::JSON::Object::Ptr identifier = new Poco::JSON::Object;
        identifier->set("name", table_name);
        Poco::JSON::Array::Ptr namespaces = new Poco::JSON::Array;
        namespaces->add(namespace_name);
        identifier->set("namespace", namespaces);
        request_body->set("identifier", identifier);
    }

    if (previous_schema_id >= 0)
    {
        Poco::JSON::Object::Ptr requirement = new Poco::JSON::Object;
        requirement->set("type", "assert-current-schema-id");
        requirement->set("current-schema-id", previous_schema_id);

        Poco::JSON::Array::Ptr requirements = new Poco::JSON::Array;
        requirements->add(requirement);
        request_body->set("requirements", requirements);
    }

    Poco::JSON::Object::Ptr schema_for_rest = cloneJsonObject(new_schema);
    if (!schema_for_rest->has(IDENTIFIER_FIELD_IDS))
    {
        Poco::JSON::Array::Ptr empty_identifier_field_ids = new Poco::JSON::Array;
        schema_for_rest->set(IDENTIFIER_FIELD_IDS, empty_identifier_field_ids);
    }

    std::optional<Int32> existing_equivalent_schema_id;
    if (metadata && metadata->has(DB::Iceberg::f_schemas))
    {
        auto schemas = metadata->getArray(DB::Iceberg::f_schemas);
        auto new_schema_id = new_schema->getValue<Int32>(DB::Iceberg::f_schema_id);
        for (UInt32 i = 0; i < schemas->size(); ++i)
        {
            auto existing_schema = schemas->getObject(i);
            if (existing_schema->getValue<Int32>(DB::Iceberg::f_schema_id) == new_schema_id)
                continue;
            if (schemasEquivalentIgnoringId(existing_schema, new_schema))
            {
                existing_equivalent_schema_id = existing_schema->getValue<Int32>(DB::Iceberg::f_schema_id);
                break;
            }
        }
    }

    Poco::JSON::Array::Ptr updates = new Poco::JSON::Array;
    if (existing_equivalent_schema_id.has_value())
    {
        Poco::JSON::Object::Ptr set_current_schema = new Poco::JSON::Object;
        set_current_schema->set("action", "set-current-schema");
        set_current_schema->set("schema-id", *existing_equivalent_schema_id);
        updates->add(set_current_schema);
    }
    else
    {
        {
            Poco::JSON::Object::Ptr add_schema = new Poco::JSON::Object;
            add_schema->set("action", "add-schema");
            add_schema->set("schema", schema_for_rest);
            add_schema->set("last-column-id", new_last_column_id);
            updates->add(add_schema);
        }
        {
            Poco::JSON::Object::Ptr set_current_schema = new Poco::JSON::Object;
            set_current_schema->set("action", "set-current-schema");
            set_current_schema->set("schema-id", -1);
            updates->add(set_current_schema);
        }
    }

    request_body->set("updates", updates);
    return request_body;
}

Poco::JSON::Object::Ptr buildUpdateMetadataRequestBody(
    const String & namespace_name, const String & table_name, Poco::JSON::Object::Ptr new_snapshot)
{
    if (!new_snapshot)
        return nullptr;

    Poco::JSON::Object::Ptr request_body = new Poco::JSON::Object;
    {
        Poco::JSON::Object::Ptr identifier = new Poco::JSON::Object;
        identifier->set("name", table_name);
        Poco::JSON::Array::Ptr namespaces = new Poco::JSON::Array;
        namespaces->add(namespace_name);
        identifier->set("namespace", namespaces);

        request_body->set("identifier", identifier);
    }

    if (new_snapshot->has("parent-snapshot-id"))
    {
        auto parent_snapshot_id = new_snapshot->getValue<Int64>("parent-snapshot-id");
        if (parent_snapshot_id != -1)
        {
            Poco::JSON::Object::Ptr requirement = new Poco::JSON::Object;
            requirement->set("type", "assert-ref-snapshot-id");
            requirement->set("ref", "main");
            requirement->set("snapshot-id", parent_snapshot_id);

            Poco::JSON::Array::Ptr requirements = new Poco::JSON::Array;
            requirements->add(requirement);
            request_body->set("requirements", requirements);
        }
    }

    Poco::JSON::Array::Ptr updates = new Poco::JSON::Array;
    {
        Poco::JSON::Object::Ptr add_snapshot = new Poco::JSON::Object;
        add_snapshot->set("action", "add-snapshot");
        add_snapshot->set("snapshot", new_snapshot);
        updates->add(add_snapshot);
    }
    {
        Poco::JSON::Object::Ptr set_snapshot = new Poco::JSON::Object;
        set_snapshot->set("action", "set-snapshot-ref");
        set_snapshot->set("ref-name", "main");
        set_snapshot->set("type", "branch");
        set_snapshot->set("snapshot-id", new_snapshot->getValue<Int64>("snapshot-id"));
        updates->add(set_snapshot);
    }
    request_body->set("updates", updates);

    return request_body;
}

std::string RestCatalog::Config::toString() const
{
    DB::WriteBufferFromOwnString wb;

    if (!prefix.empty())
        wb << "prefix: " << prefix.string() << ", ";

    if (!default_base_location.empty())
        wb << "default_base_location: " << default_base_location << ", ";

    return wb.str();
}

RestCatalog::RestCatalog(
    const std::string & warehouse_,
    const std::string & base_url_,
    const std::string & catalog_credential_,
    const std::string & auth_scope_,
    const std::string & auth_header_,
    const std::string & oauth_server_uri_,
    bool oauth_server_use_request_body_,
    const std::string & namespaces_,
    DB::ContextPtr context_,
    const TokenForwardingConfig & token_forwarding_)
    : ICatalog(warehouse_)
    , DB::WithContext(context_)
    , base_url(correctAPIURI(base_url_))
    , log(getLogger("RestCatalog(" + warehouse_ + ")"))
    , auth_scope(auth_scope_)
    , oauth_server_uri(oauth_server_uri_)
    , oauth_server_use_request_body(oauth_server_use_request_body_)
    , token_forwarding(token_forwarding_)
    , user_token_cache(
          CurrentMetrics::DataLakeCatalogUserTokenCacheBytes,
          CurrentMetrics::DataLakeCatalogUserTokenCacheEntries,
          user_token_cache_max_entries)
    , allowed_namespaces(namespaces_)
{
    CatalogState initial_state;
    if (!catalog_credential_.empty())
    {
        std::tie(initial_state.client_id, initial_state.client_secret) = parseCatalogCredential(catalog_credential_);
        update_token_if_expired = true;
    }
    else if (!auth_header_.empty())
    {
        initial_state.auth_header = parseAuthHeader(auth_header_);
        validateAuthHeaders(initial_state.auth_header.value());
    }

    /// Without forwarding, `/v1/config` is fetched here exactly as before. With forwarding there
    /// may be no service credential at all, so an unauthenticated `GET /v1/config` would be
    /// rejected by a secured catalog. Defer it to the first user query instead -- the database is
    /// built lazily anyway, and `/v1/config` returns only `prefix` and `default-base-location`,
    /// so either identity is appropriate.
    if (!token_forwarding.forward_user_token)
    {
        initial_state.config = loadConfig(initial_state, /* generation */ 0, /* auth_token */ {});
        initial_state.config_loaded = true;
    }
    state.set(std::make_unique<const CatalogState>(std::move(initial_state)));
}

RestCatalog::RestCatalog(
    const std::string & warehouse_,
    const std::string & base_url_,
    const std::string & auth_scope_,
    const std::string & oauth_server_uri_,
    bool oauth_server_use_request_body_,
    const std::string & namespaces_,
    DB::ContextPtr context_)
    : ICatalog(warehouse_)
    , DB::WithContext(context_)
    , base_url(correctAPIURI(base_url_))
    , log(getLogger("RestCatalog(" + warehouse_ + ")"))
    , auth_scope(auth_scope_)
    , oauth_server_uri(oauth_server_uri_)
    , oauth_server_use_request_body(oauth_server_use_request_body_)
    , user_token_cache(
          CurrentMetrics::DataLakeCatalogUserTokenCacheBytes,
          CurrentMetrics::DataLakeCatalogUserTokenCacheEntries,
          user_token_cache_max_entries)
    , allowed_namespaces(namespaces_)
{
}


void RestCatalog::loadConfigIfNeeded(const DB::ForwardedAuthTokenPtr & auth_token) const
{
    if (state.get()->config_loaded)
        return;

    std::lock_guard lock(config_mutex);
    const auto old_state = getStateSnapshot();
    if (old_state->config_loaded)
        return;

    auto new_state = std::make_unique<CatalogState>(*old_state);
    new_state->config = loadConfig(*old_state, old_state.generation, auth_token);
    new_state->config_loaded = true;
    state.set(std::move(new_state));
}

RestCatalog::Config RestCatalog::loadConfig(
    const CatalogState & catalog_state,
    UInt64 generation,
    const DB::ForwardedAuthTokenPtr & auth_token,
    const std::optional<DB::HTTPHeaderEntries> & auth_headers) const
{
    Poco::URI::QueryParameters params = {{"warehouse", warehouse}};

    std::string json_str;

    {
        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogLoadConfig);
        auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogLoadConfigMicroseconds);
        auto buf = createReadBuffer(catalog_state, generation, CONFIG_ENDPOINT, auth_token, params, /* headers */{}, auth_headers);
        readJSONObjectPossiblyInvalid(json_str, *buf);
    }

    LOG_DEBUG(log, "Received catalog configuration settings: {}", json_str);

    Poco::JSON::Parser parser;
    Poco::Dynamic::Var json = parser.parse(json_str);
    const Poco::JSON::Object::Ptr & object = json.extract<Poco::JSON::Object::Ptr>();

    Config result;

    auto defaults_object = object->get("defaults").extract<Poco::JSON::Object::Ptr>();
    parseCatalogConfigurationSettings(defaults_object, result);

    auto overrides_object = object->get("overrides").extract<Poco::JSON::Object::Ptr>();
    parseCatalogConfigurationSettings(overrides_object, result);

    LOG_DEBUG(log, "Parsed catalog configuration settings: {}", result.toString());
    return result;
}

void RestCatalog::parseCatalogConfigurationSettings(const Poco::JSON::Object::Ptr & object, Config & result)
{
    if (!object)
        return;

    if (object->has("prefix"))
        result.prefix = object->get("prefix").extract<String>();

    if (object->has("default-base-location"))
        result.default_base_location = object->get("default-base-location").extract<String>();
}

void RestCatalog::validateAuthHeaders(const DB::HTTPHeaderEntry & header) const
{
    /// `registerDatabaseDataLake` validates `auth_header` on CREATE only, so that a database
    /// persisted with a forbidden or malformed header does not block server startup on ATTACH.
    /// The catalog is built lazily on first use instead; this is where the user-provided
    /// `auth_header` first becomes a header sent to the catalog, so enforce `http_forbid_headers`
    /// here, before `loadConfig` issues any request. Mirrors the CREATE-path check: a copy is
    /// validated and the original parsed header is kept.
    DB::HTTPHeaderEntries header_to_check{header};
    getContext()->getGlobalContext()->getHTTPHeaderFilter().checkAndNormalizeHeaders(header_to_check);
}

DB::HTTPHeaderEntries RestCatalog::getAuthHeaders(const AuthContext & auth_context) const
{
    fiu_do_on(DB::FailPoints::check_database_datalake_negative,
    {
        throw DB::Exception(DB::ErrorCodes::FAULT_INJECTED, "Injecting fault when checking database");
    });

    if (auth_context.used_cached_oauth_token)
        *auth_context.used_cached_oauth_token = false;

    const auto & catalog_state = auth_context.catalog_state;

    /// Option 1: user specified auth header manually.
    /// Header has format: 'Authorization: <scheme> <token>'.
    /// Mutually exclusive with forwarding -- `validateSettings` rejects the combination, because
    /// a static header short-circuits everything below and would silently defeat forwarding.
    if (catalog_state.auth_header.has_value())
    {
        return DB::HTTPHeaderEntries{catalog_state.auth_header.value()};
    }

    /// Option 2: forward the querying user's identity, either as-is (passthrough) or as the
    /// session token obtained by exchanging it. Never falls back to Option 3: doing so would turn
    /// an authorization failure into a query that succeeds under the wrong identity.
    if (token_forwarding.forward_user_token)
    {
        DB::HTTPHeaderEntries headers;
        headers.emplace_back(
            "Authorization",
            "Bearer "
                + getForwardedToken(catalog_state, auth_context.generation, auth_context.auth_token, auth_context.update_token));
        return headers;
    }

    /// Option 3: user provided grant_type, client_id and client_secret.
    /// We would make OAuthClientCredentialsRequest
    /// https://github.com/apache/iceberg/blob/3badfe0c1fcf0c0adfc7aa4a10f0b50365c48cf9/open-api/rest-catalog-open-api.yaml#L3498C5-L3498C34
    if (!catalog_state.client_id.empty())
    {
        /// The cached token may have been minted with other credentials than the ones in
        /// `catalog_state` (e.g. right after `ALTER DATABASE ... MODIFY SETTING`); then the
        /// request fails with 401/403 and is retried with `update_token = true`, fetching
        /// a token with the snapshot's credentials.
        auto current = access_token.get();
        if (!current || auth_context.update_token || current->isExpired())
        {
            current = publishServiceToken(
                retrieveAccessToken(catalog_state.client_id, catalog_state.client_secret), auth_context.generation);
        }
        else if (auth_context.used_cached_oauth_token)
        {
            *auth_context.used_cached_oauth_token = true;
        }

        DB::HTTPHeaderEntries headers;
        headers.emplace_back("Authorization", "Bearer " + current->token);
        return headers;
    }
    return {};
}

MultiVersion<AccessToken>::Version RestCatalog::publishServiceToken(AccessToken minted, UInt64 generation) const
{
    auto result = std::make_shared<const AccessToken>(std::move(minted));

    /// A grant that started before `ALTER DATABASE ... MODIFY SETTING catalog_credential` can only
    /// finish after it, and would otherwise overwrite the token the ALTER eagerly published --
    /// putting the rotated-away credential back in force for the whole lifetime of that token,
    /// which no cache TTL bounds. The generation says whether the credentials it was minted with
    /// are still the ones in force; the lock stops the ALTER from landing between the two.
    std::lock_guard lock(auth_publish_mutex);
    if (auth_generation.load(std::memory_order_acquire) == generation)
        access_token.set(std::make_unique<AccessToken>(*result));

    /// Returned regardless: the catalog accepted this request under these credentials, so the
    /// request itself completes. Only sharing the token with later requests is withheld.
    return result;
}

String RestCatalog::getForwardedToken(
    const CatalogState & catalog_state, UInt64 generation, const DB::ForwardedAuthTokenPtr & auth_token, bool update_token) const
{
    /// Re-read the server-level switch on every forwarded request instead of trusting the decision
    /// `Session::authenticate` made once. `enable_token_forwarding` is hot-reloadable
    /// (`AccessControl::setExternalAuthenticatorsConfig` re-reads it on `SYSTEM RELOAD CONFIG`), so
    /// without this an operator turning it off in response to a credential leak or an IdP outage
    /// would keep forwarding the token captured by every already-authenticated session -- for the
    /// whole life of a native connection or a named session -- until the server restarts.
    ///
    /// Checked before the token itself: when the switch is off it is the reason a session has no
    /// token in the first place, so reporting the missing token would name a symptom, not a cause.
    if (!getContext()->getGlobalContext()->getAccessControl().isTokenForwardingEnabled())
    {
        /// Session tokens exchanged while the previous policy was in force must not outlive it:
        /// dropping them means turning the switch back on cannot serve a token minted under the
        /// policy the operator has just revoked, and every user is exchanged for anew. The clear
        /// costs nothing that matters -- this path throws, so it is only ever reached by a request
        /// that is about to fail anyway.
        user_token_cache.clear();

        throw DB::Exception(
            DB::ErrorCodes::CATALOG_USER_TOKEN_NOT_AVAILABLE,
            "Catalog `{}` is configured with `oauth_forward_user_token = 1`, but the server-level "
            "`enable_token_forwarding` setting is off, so the querying user's token cannot be "
            "presented to the catalog. Falling back to the catalog's service principal would run "
            "the query under the wrong identity, so the request is refused instead. Set "
            "`enable_token_forwarding` to `1` in the server configuration and reconnect (a session "
            "authenticated while the setting was off carries no token), or recreate the database "
            "without `oauth_forward_user_token`.",
            warehouse);
    }

    if (!auth_token || auth_token->token.empty())
        throw DB::Exception(
            DB::ErrorCodes::CATALOG_USER_TOKEN_NOT_AVAILABLE,
            "Catalog `{}` is configured with `oauth_forward_user_token = 1`, so every catalog "
            "request must carry the querying user's bearer token, but this session has none. "
            "Either authenticate with a token (an `Authorization: Bearer` HTTP header, or "
            "`--jwt` for the native protocol) and make sure the server-level "
            "`enable_token_forwarding` setting is on, or recreate the database without "
            "`oauth_forward_user_token`.",
            warehouse);

    /// Passthrough: the user's token is presented to the catalog unchanged. Nothing is cached --
    /// the token arrives with every request anyway.
    if (!token_forwarding.exchangeEnabled())
        return auth_token->token;

    const auto ttl = std::chrono::seconds(token_forwarding.user_token_cache_ttl);
    const bool caching_enabled = ttl > std::chrono::seconds::zero();

    auto exchange = [&]
    {
        return std::make_shared<AccessToken>(exchangeUserToken(catalog_state, generation, *auth_token));
    };

    if (!caching_enabled)
        return exchange()->token;

    /// Scoped to the generation the exchange authenticated in, so that an exchange still in
    /// flight when the catalog credentials are rotated writes its result under a key nothing
    /// reads any more, instead of reinstating a session minted with the old client secret.
    const String cache_key = fmt::format("{}:{}", generation, auth_token->fingerprint);

    if (!update_token)
    {
        if (auto cached = user_token_cache.get(cache_key); cached && !cached->isExpired())
        {
            ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogUserTokenCacheHits);
            return cached->token;
        }
    }

    /// Either the entry expired or the caller asked for a fresh one. Drop it first so that
    /// `getOrSetWithOutcome` reloads instead of handing back the stale value, and so that the
    /// stampede protection still collapses the concurrent re-exchanges a single `SHOW TABLES`
    /// fanned across the catalog thread pool would otherwise cause.
    user_token_cache.remove(cache_key);
    auto [session_token, outcome] = user_token_cache.getOrSetWithOutcome(cache_key, exchange);
    if (outcome == DB::CacheGetOrSetOutcome::Hit)
        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogUserTokenCacheHits);
    return session_token->token;
}

AccessToken RestCatalog::exchangeUserToken(
    const CatalogState & catalog_state, UInt64 generation, const DB::ForwardedAuthToken & auth_token) const
{
    TokenRequest request;
    request.grant = TokenRequest::Grant::TokenExchange;
    request.url = Poco::URI(token_forwarding.token_exchange_uri);
    request.scope = auth_scope;
    request.client_id = catalog_state.client_id;
    request.client_secret = catalog_state.client_secret;
    request.subject_token = auth_token.token;
    request.subject_token_type = token_forwarding.subject_token_type;
    request.requested_token_type = token_forwarding.requested_token_type;

    /// An `actor_token` is only meaningful to a server that can validate it, and the catalog's
    /// own service token is not something an IdP can. Off by default; turn it on for a
    /// spec-implementing catalog to get RFC 8693 delegation semantics (`sub=user, act=clickhouse`).
    /// Minting it is a `client_credentials` grant; if it fails the error propagates, because an
    /// exchange silently downgraded from delegation to plain impersonation is exactly what
    /// enabling the setting was meant to prevent.
    if (token_forwarding.forward_actor_token)
    {
        request.actor_token = getServicePrincipalToken(catalog_state, generation);
        request.actor_token_type = "urn:ietf:params:oauth:token-type:access_token";
    }

    ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogTokenExchange);
    auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogTokenExchangeMicroseconds);

    AccessToken exchanged;
    try
    {
        exchanged = requestToken(request);
    }
    catch (...)
    {
        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogTokenExchangeFailures);
        throw;
    }

    /// A cached user token with no expiry would survive IdP revocation indefinitely, so when the
    /// response carries no `expires_in` fall back to the configured TTL rather than "never".
    /// When it does, still cap at the TTL so an entry never outlives the documented maximum.
    if (token_forwarding.user_token_cache_ttl > 0)
    {
        const auto ttl_bound = std::chrono::system_clock::now() + std::chrono::seconds(token_forwarding.user_token_cache_ttl);
        if (!exchanged.expires_at.has_value() || exchanged.expires_at.value() > ttl_bound)
            exchanged.expires_at = ttl_bound;
    }

    LOG_DEBUG(log, "Exchanged the token of user `{}` for a catalog session token", auth_token.principal);
    return exchanged;
}

OneLakeCatalog::OneLakeCatalog(
    const std::string & warehouse_,
    const std::string & base_url_,
    const std::string & onelake_tenant_id,
    const std::string & onelake_client_id,
    const std::string & onelake_client_secret,
    const std::string & bearer_token_,
    const std::string & auth_scope_,
    const std::string & oauth_server_uri_,
    bool oauth_server_use_request_body_,
    const std::string & namespaces_,
    DB::ContextPtr context_)
    : RestCatalog(warehouse_, base_url_, auth_scope_, oauth_server_uri_, oauth_server_use_request_body_, namespaces_, context_)
{
    CatalogState initial_state;
    initial_state.tenant_id = onelake_tenant_id;
    if (!bearer_token_.empty())
    {
        /// Pre-obtained token scoped to https://storage.azure.com. Used for both catalog header
        /// and Azure Blob access. Does not support refresh.
        initial_state.bearer_token = bearer_token_;
        initial_state.auth_header = DB::HTTPHeaderEntry("Authorization", "Bearer " + bearer_token_);
        validateAuthHeaders(initial_state.auth_header.value());
    }
    else
    {
        initial_state.client_id = onelake_client_id;
        initial_state.client_secret = onelake_client_secret;
        update_token_if_expired = true;
    }
    initial_state.config = loadConfig(initial_state, /* generation */ 0, /* auth_token */ {});
    initial_state.config_loaded = true;
    state.set(std::make_unique<const CatalogState>(std::move(initial_state)));
}

void RestCatalog::validateSettingsChangesImpl(
    const DB::SettingsChanges & changes,
    const std::unordered_set<std::string> & alterable_settings,
    const std::string & auth_mode_description)
{
    for (const auto & change : changes)
    {
        if (alterable_settings.empty())
            throw DB::Exception(
                DB::ErrorCodes::BAD_ARGUMENTS,
                "Setting `{}` cannot be altered for a {}: the database was created without authentication settings",
                change.name,
                auth_mode_description);

        if (!alterable_settings.contains(change.name))
            throw DB::Exception(
                DB::ErrorCodes::BAD_ARGUMENTS,
                "Setting `{}` cannot be altered for a {} "
                "(the authentication mode is fixed when the database is created; "
                "alterable settings are: {})",
                change.name,
                auth_mode_description,
                fmt::join(alterable_settings, ", "));

        if (change.value.getType() != DB::Field::Types::String)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "Setting `{}` must be a string", change.name);

        if (change.value.safeGet<String>().empty())
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "Setting `{}` cannot be set to an empty value", change.name);
    }
}

void RestCatalog::validateSettingsChanges(const DB::SettingsChanges & changes, bool credential_mode, bool header_mode)
{
    static const std::unordered_set<std::string> credential_mode_settings = {
        DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::catalog_credential)};
    static const std::unordered_set<std::string> header_mode_settings = {
        DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::auth_header)};
    static const std::unordered_set<std::string> no_auth_settings = {};

    if (credential_mode)
        validateSettingsChangesImpl(changes, credential_mode_settings, "REST catalog with catalog credential authentication");
    else if (header_mode)
        validateSettingsChangesImpl(changes, header_mode_settings, "REST catalog with auth header authentication");
    else
        validateSettingsChangesImpl(changes, no_auth_settings, "REST catalog");
}

struct RestCatalog::PreparedAuthChanges : ICatalog::PreparedSettingsChanges
{
    std::unique_ptr<const CatalogState> new_state;
    /// Set only when the OAuth credentials changed.
    std::unique_ptr<AccessToken> new_access_token;
};

ICatalog::PreparedSettingsChangesPtr RestCatalog::prepareSettingsChanges(const DB::SettingsChanges & changes)
{
    const auto old_state = getStateSnapshot();
    CatalogState new_state = *old_state;

    auto prepared = std::make_unique<PreparedAuthChanges>();
    std::optional<DB::HTTPHeaderEntries> new_auth_headers;
    applySettingsChangesToState(changes, *old_state, new_state, new_auth_headers, prepared->new_access_token);

    /// The config was loaded with the old credentials; the new ones may resolve the
    /// warehouse to a different prefix or base location, so reload it before publishing.
    new_state.config = loadConfig(new_state, old_state.generation, /* auth_token */ {}, new_auth_headers);
    prepared->new_state = std::make_unique<const CatalogState>(std::move(new_state));
    return prepared;
}

void RestCatalog::commitSettingsChanges(ICatalog::PreparedSettingsChangesPtr prepared)
{
    auto * prepared_auth = dynamic_cast<PreparedAuthChanges *>(prepared.get());
    if (!prepared_auth || !prepared_auth->new_state)
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Settings changes to commit were not prepared by this catalog");

    {
        /// Under the lock so that a request cannot check the generation, find it unchanged, and
        /// only then publish a token minted with the credentials being replaced here.
        std::lock_guard lock(auth_publish_mutex);
        state.set(std::move(prepared_auth->new_state));
        if (prepared_auth->new_access_token)
            access_token.set(std::move(prepared_auth->new_access_token));

        /// Bumped after the state is published, never before: a reader takes the generation
        /// first and the state second, so this order leaves it either correct or paired with a
        /// generation older than its state, which merely costs a wasted cache fill.
        auth_generation.fetch_add(1, std::memory_order_release);
    }

    /// Both caches hold artifacts derived from the credentials that were just replaced: session
    /// tokens exchanged with the old `client_id`/`client_secret`, and credentials the catalog
    /// vended to the old identity. Keeping them lets a rotated -- typically leaked -- credential
    /// keep working for the rest of the cache TTL, which is what the ALTER was meant to stop.
    ///
    /// Cleared after the generation is bumped, which is what makes the two mechanisms cover each
    /// other: a write that slips past the generation check necessarily started before this clear
    /// and is wiped by it, and a write that lands after it is already keyed to a dead generation.
    user_token_cache.clear();
    {
        std::lock_guard lock(credentials_cache_mutex);
        credentials_cache.clear();
    }
}

void RestCatalog::applySettingsChangesToState(
    const DB::SettingsChanges & changes,
    const CatalogState & old_state,
    CatalogState & new_state,
    std::optional<DB::HTTPHeaderEntries> & new_auth_headers,
    std::unique_ptr<AccessToken> & new_access_token)
{
    const bool credential_mode = !old_state.client_id.empty();
    const bool header_mode = old_state.auth_header.has_value();

    validateSettingsChanges(changes, credential_mode, header_mode);

    for (const auto & change : changes)
    {
        if (change.name == DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::catalog_credential))
        {
            std::tie(new_state.client_id, new_state.client_secret) = parseCatalogCredential(change.value.safeGet<String>());
        }
        else if (change.name == DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::auth_header))
        {
            new_state.auth_header = parseAuthHeader(change.value.safeGet<String>());
            validateAuthHeaders(new_state.auth_header.value());
        }
        else
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Unexpected setting `{}` after validation", change.name);
    }

    if (credential_mode && (new_state.client_id != old_state.client_id || new_state.client_secret != old_state.client_secret))
    {
        /// Eagerly fetch a token with the not-yet-published credentials: wrong credentials
        /// fail the ALTER right here, and the config reload authenticates with that token
        /// instead of the cached one.
        new_access_token = std::make_unique<AccessToken>(retrieveAccessToken(new_state.client_id, new_state.client_secret));
        new_auth_headers = DB::HTTPHeaderEntries{{"Authorization", "Bearer " + new_access_token->token}};
    }
}

DB::HTTPHeaderEntries OneLakeCatalog::getAuthHeaders(const AuthContext & auth_context) const
{
    auto headers = RestCatalog::getAuthHeaders(auth_context);
    headers.emplace_back("User-Agent", fmt::format("ClickHouse/{}{} OneLake-Catalog", VERSION_STRING, VERSION_OFFICIAL));
    return headers;
}

void OneLakeCatalog::validateSettingsChanges(const DB::SettingsChanges & changes, bool bearer_mode)
{
    static const std::unordered_set<std::string> bearer_mode_settings = {
        DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_tenant_id),
        DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_bearer_token)};
    static const std::unordered_set<std::string> client_mode_settings = {
        DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_tenant_id),
        DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_client_id),
        DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_client_secret)};

    RestCatalog::validateSettingsChangesImpl(
        changes,
        bearer_mode ? bearer_mode_settings : client_mode_settings,
        bearer_mode ? "OneLake catalog with bearer token authentication" : "OneLake catalog with client credentials authentication");
}

void OneLakeCatalog::applySettingsChangesToState(
    const DB::SettingsChanges & changes,
    const CatalogState & old_state,
    CatalogState & new_state,
    std::optional<DB::HTTPHeaderEntries> & new_auth_headers,
    std::unique_ptr<AccessToken> & new_access_token)
{
    const bool bearer_mode = !old_state.bearer_token.empty();

    validateSettingsChanges(changes, bearer_mode);

    for (const auto & change : changes)
    {
        if (change.name == DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_tenant_id))
            new_state.tenant_id = change.value.safeGet<String>();
        else if (change.name == DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_bearer_token))
            new_state.bearer_token = change.value.safeGet<String>();
        else if (change.name == DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_client_id))
            new_state.client_id = change.value.safeGet<String>();
        else if (change.name == DB::DatabaseDataLakeSettings::getSettingName(DB::DatabaseDataLakeSetting::onelake_client_secret))
            new_state.client_secret = change.value.safeGet<String>();
        else
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Unexpected setting `{}` after validation", change.name);
    }

    if (bearer_mode)
    {
        new_state.auth_header = DB::HTTPHeaderEntry("Authorization", "Bearer " + new_state.bearer_token);
        validateAuthHeaders(new_state.auth_header.value());
    }
    else if (new_state.client_id != old_state.client_id || new_state.client_secret != old_state.client_secret)
    {
        /// Eagerly fetch a token with the not-yet-published credentials: wrong credentials
        /// fail the ALTER right here, and the config reload authenticates with that token
        /// instead of the cached one.
        new_access_token = std::make_unique<AccessToken>(retrieveAccessToken(new_state.client_id, new_state.client_secret));
        new_auth_headers = DB::HTTPHeaderEntries{{"Authorization", "Bearer " + new_access_token->token}};
    }
}

namespace
{

[[maybe_unused]] const bool rest_settings_alter_validator_registered = []
{
    CatalogSettingsAlterValidatorFactory::instance().registerValidator(
        DB::DatabaseDataLakeCatalogType::ICEBERG_REST,
        [](const DB::DatabaseDataLakeSettings & current_settings, const DB::SettingsChanges & changes)
        {
            const bool credential_mode = !current_settings[DB::DatabaseDataLakeSetting::catalog_credential].value.empty();
            const bool header_mode = !current_settings[DB::DatabaseDataLakeSetting::auth_header].value.empty();
            RestCatalog::validateSettingsChanges(changes, credential_mode, header_mode);
        });
    return true;
}();

[[maybe_unused]] const bool onelake_settings_alter_validator_registered = []
{
    CatalogSettingsAlterValidatorFactory::instance().registerValidator(
        DB::DatabaseDataLakeCatalogType::ICEBERG_ONELAKE,
        [](const DB::DatabaseDataLakeSettings & current_settings, const DB::SettingsChanges & changes)
        {
            const bool bearer_mode = !current_settings[DB::DatabaseDataLakeSetting::onelake_bearer_token].value.empty();
            OneLakeCatalog::validateSettingsChanges(changes, bearer_mode);
        });
    return true;
}();

}

AccessToken RestCatalog::requestToken(const TokenRequest & token_request) const
{
    Poco::URI url = token_request.url;
    DB::ReadWriteBufferFromHTTP::OutStreamCallback out_stream_callback;
    size_t body_size = 0;
    String body;

    /// Both grants authenticate the request itself with `client_id`/`client_secret` in the form
    /// body -- standard OAuth token-endpoint client authentication. Iceberg's own client instead
    /// sends the catalog's bearer token for the exchange; supporting both would mean guessing
    /// which kind of target we are talking to, and sending both at once is rejected by strict
    /// servers as multiple client-authentication methods. One rule, documented.
    std::vector<std::pair<String, String>> params;
    if (token_request.grant == TokenRequest::Grant::ClientCredentials)
    {
        params.emplace_back("grant_type", "client_credentials");
        params.emplace_back("scope", token_request.scope);
        params.emplace_back("client_id", token_request.client_id);
        params.emplace_back("client_secret", token_request.client_secret);
    }
    else
    {
        params.emplace_back("grant_type", "urn:ietf:params:oauth:grant-type:token-exchange");
        params.emplace_back("subject_token", token_request.subject_token);
        params.emplace_back("subject_token_type", token_request.subject_token_type);
        /// An empty `requested_token_type` means "omit the field", per the setting's description.
        if (!token_request.requested_token_type.empty())
            params.emplace_back("requested_token_type", token_request.requested_token_type);
        if (!token_request.scope.empty())
            params.emplace_back("scope", token_request.scope);
        /// Absent rather than empty when disabled: an empty `actor_token` is not the same thing
        /// as no delegation, and strict servers reject it.
        if (!token_request.actor_token.empty())
        {
            params.emplace_back("actor_token", token_request.actor_token);
            params.emplace_back("actor_token_type", token_request.actor_token_type);
        }
        params.emplace_back("client_id", token_request.client_id);
        params.emplace_back("client_secret", token_request.client_secret);
    }

    if (token_request.use_query_parameters)
    {
        Poco::URI::QueryParameters query_params(params.begin(), params.end());
        url.setQueryParameters(query_params);
    }
    else
    {
        DB::WriteBufferFromOwnString wb;
        bool first = true;
        for (const auto & [name, value] : params)
        {
            if (!first)
                wb << "&";
            first = false;
            wb << name << "=" << formUrlEncode(value);
        }
        body = wb.str();
        body_size = body.size();
        out_stream_callback = [&](std::ostream & os)
        {
            os << body;
        };
    }

    const auto & context = getContext();
    /// Also checked for the exchange endpoint, not only for catalog GETs: the URL is chosen by
    /// whoever created the database, and the request carries the querying user's own token.
    context->getRemoteHostFilter().checkHostAndPort(url.getHost(), std::to_string(url.getPort()));
    auto timeouts = DB::ConnectionTimeouts::getHTTPTimeouts(context->getSettingsRef(), context->getServerSettings());
    auto session = makeHTTPSession(DB::HTTPConnectionGroupType::HTTP, url, timeouts, {});

    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, url.getPathAndQuery(),
                                Poco::Net::HTTPMessage::HTTP_1_1);
    request.setContentType("application/x-www-form-urlencoded");
    request.setContentLength(body_size);
    request.set("Accept", "application/json");

    std::ostream & os = session->sendRequest(request);
    /// The query-parameters flavor of the request has no body.
    if (out_stream_callback)
        out_stream_callback(os);

    Poco::Net::HTTPResponse response;
    std::istream & rs = session->receiveResponse(response);

    std::string json_str;
    Poco::StreamCopier::copyToString(rs, json_str);

    /// Every failure below names the endpoint and the status but never the response body: an OAuth
    /// error response may echo the request, and for an exchange the request carries the user's
    /// token. Pointing at an endpoint that does not implement the grant is the common
    /// misconfiguration, and without these checks it surfaced as a bare Poco "JSON Exception"
    /// from parsing a 404 HTML page.
    const auto describe_endpoint = [&url, &response]
    {
        return fmt::format(
            "OAuth token endpoint {}://{}:{}{} returned HTTP {}",
            url.getScheme(), url.getHost(), url.getPort(), url.getPath(),
            static_cast<int>(response.getStatus()));
    };

    Poco::JSON::Object::Ptr object;
    try
    {
        object = Poco::JSON::Parser().parse(json_str).extract<Poco::JSON::Object::Ptr>();
    }
    catch (const Poco::Exception &)
    {
        object = nullptr;
    }
    if (!object)
        throw DB::Exception(
            DB::ErrorCodes::DATALAKE_DATABASE_ERROR, "{} with a body that is not a JSON object", describe_endpoint());

    AccessToken token;
    if (!object->has("access_token"))
        throw DB::Exception(
            DB::ErrorCodes::DATALAKE_DATABASE_ERROR, "{} with no `access_token` field", describe_endpoint());
    token.token = object->get("access_token").extract<String>();

    if (object->has("expires_in"))
    {
        Int64 expires_in = object->getValue<Int64>("expires_in");
        /// Use 90% of the token lifetime as the validity window so that short-lived tokens
        /// (e.g. expires_in=300) still get a sensible buffer instead of going non-positive.
        token.expires_at = std::chrono::system_clock::now() + std::chrono::seconds(expires_in * 9 / 10);
    }

    return token;
}

AccessToken RestCatalog::retrieveAccessToken(const std::string & client_id, const std::string & client_secret) const
{
    static constexpr auto oauth_tokens_endpoint = "oauth/tokens";

    /// Deliberately does NOT honour the catalog-advertised `oauth2-server-uri` from `/v1/config`:
    /// the explicit settings cover everything deployable, and auto-redirecting the grant would be
    /// a surprising behaviour change for existing databases.

    TokenRequest request;
    request.grant = TokenRequest::Grant::ClientCredentials;
    request.scope = auth_scope;
    request.client_id = client_id;
    request.client_secret = client_secret;

    if (oauth_server_uri.empty() && !oauth_server_use_request_body)
    {
        request.url = Poco::URI(base_url / oauth_tokens_endpoint);
        request.use_query_parameters = true;
    }
    else
    {
        request.url = oauth_server_uri.empty() ? Poco::URI(base_url / oauth_tokens_endpoint) : Poco::URI(oauth_server_uri);
    }

    ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogClientCredentialsGrants);
    ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogAuthTokenRetrieve);
    auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogAuthTokenRefreshedMicroseconds);
    return requestToken(request);
}

String RestCatalog::getServicePrincipalToken(const CatalogState & catalog_state, UInt64 generation) const
{
    /// Same caching rule as the `client_credentials` branch of `getAuthHeaders`: reuse the token
    /// held in `access_token` until it falls outside its validity window, then mint a new one.
    /// Storing it there is what that member is for -- it is the service principal's token, shared
    /// by every user of the database, and never a per-user one.
    auto current = access_token.get();
    if (!current || current->isExpired())
        current = publishServiceToken(retrieveAccessToken(catalog_state.client_id, catalog_state.client_secret), generation);
    return current->token;
}

BigLakeCatalog::BigLakeCatalog(
    const std::string & warehouse_,
    const std::string & base_url_,
    const std::string & google_project_id_,
    const std::string & google_service_account_,
    const std::string & google_metadata_service_,
    const std::string & google_adc_client_id_,
    const std::string & google_adc_client_secret_,
    const std::string & google_adc_refresh_token_,
    const std::string & google_adc_quota_project_id_,
    const std::string & namespaces_,
    DB::ContextPtr context_)
    : RestCatalog(warehouse_, base_url_, "", "", false, namespaces_, context_)
    , google_project_id(google_project_id_)
    , google_service_account(google_service_account_)
    , google_metadata_service(google_metadata_service_)
    , google_adc_client_id(google_adc_client_id_)
    , google_adc_client_secret(google_adc_client_secret_)
    , google_adc_refresh_token(google_adc_refresh_token_)
    , google_adc_quota_project_id(google_adc_quota_project_id_)
{
    update_token_if_expired = true;
    // Get token before loading config so getAuthHeaders() can work
    if (!google_project_id.empty() || !google_adc_client_id.empty())
    {
        access_token.set(std::make_unique<AccessToken>(retrieveGoogleCloudAccessToken()));
    }
    CatalogState initial_state;
    initial_state.config = loadConfig(initial_state, /* generation */ 0, /* auth_token */ {});
    initial_state.config_loaded = true;
    state.set(std::make_unique<const CatalogState>(std::move(initial_state)));
}

DB::HTTPHeaderEntries BigLakeCatalog::getAuthHeaders(const AuthContext & auth_context) const
{
    /// Google Cloud OAuth2 for BigLake.
    /// Uses GCP metadata service or Application Default Credentials to get access token.
    /// Only use Google OAuth if explicitly configured (google_project_id or google_adc_client_id).
    /// https://developers.google.com/identity/protocols/oauth2
    if (!google_project_id.empty() || !google_adc_client_id.empty())
    {
        if (auth_context.used_cached_oauth_token)
            *auth_context.used_cached_oauth_token = false;

        auto current = access_token.get();
        if (!current || auth_context.update_token || current->isExpired())
        {
            access_token.set(std::make_unique<AccessToken>(retrieveGoogleCloudAccessToken()));
            current = access_token.get();
        }
        else if (auth_context.used_cached_oauth_token)
        {
            *auth_context.used_cached_oauth_token = true;
        }

        DB::HTTPHeaderEntries headers;
        headers.emplace_back("Authorization", "Bearer " + current->token);

        std::string project_id = google_project_id;
        if (project_id.empty() && !google_adc_quota_project_id.empty())
        {
            project_id = google_adc_quota_project_id;
        }

        if (!project_id.empty())
        {
            headers.emplace_back("x-goog-user-project", project_id);
        }

        return headers;
    }

    return RestCatalog::getAuthHeaders(auth_context);
}

AccessToken BigLakeCatalog::retrieveGoogleCloudAccessTokenFromRefreshToken() const
{
    if (google_adc_client_id.empty() || google_adc_client_secret.empty() || google_adc_refresh_token.empty())
        throw DB::Exception(
            DB::ErrorCodes::BAD_ARGUMENTS,
            "Invalid ADC credentials: client_id, client_secret, and refresh_token are required");

    const auto & context = getContext();
    auto timeouts = DB::ConnectionTimeouts::getHTTPTimeouts(context->getSettingsRef(), context->getServerSettings());
    auto result = fetchGCPOAuthToken(google_adc_client_id, google_adc_client_secret, google_adc_refresh_token, timeouts);

    AccessToken token;
    token.token = std::move(result.access_token);
    token.expires_at = std::chrono::system_clock::now() + std::chrono::seconds(result.expires_in * 9 / 10);
    return token;
}

AccessToken BigLakeCatalog::retrieveGoogleCloudAccessToken() const
{
    ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogAuthTokenRetrieve);
    auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogAuthTokenRefreshedMicroseconds);

    if (!google_adc_client_id.empty() && !google_adc_client_secret.empty() && !google_adc_refresh_token.empty())
    {
        try
        {
            return retrieveGoogleCloudAccessTokenFromRefreshToken();
        }
        catch (const DB::Exception & e)
        {
            LOG_DEBUG(log, "Failed to use ADC credentials, falling back to metadata service: {}", e.what());
        }
    }

    /// Fallback to GCP metadata service (works inside GCP infrastructure)
    /// https://cloud.google.com/compute/docs/metadata/overview
    static constexpr auto DEFAULT_REQUEST_TOKEN_PATH = "/computeMetadata/v1/instance/service-accounts";

    const auto & context = getContext();

    const auto allowed_metadata_hosts = getAllowedBigLakeMetadataServiceHosts(context->getConfigRef());
    if (allowed_metadata_hosts.empty())
        throw DB::Exception(
            DB::ErrorCodes::BAD_ARGUMENTS,
            "BigLake metadata service requests are disabled. To enable, configure "
            "<iceberg_biglake_metadata_service_hosts> in server config with the allowed metadata "
            "hosts (typically `metadata.google.internal` and `169.254.169.254`).");

    if (!allowed_metadata_hosts.contains(google_metadata_service))
        throw DB::Exception(
            DB::ErrorCodes::BAD_ARGUMENTS,
            "google_metadata_service host `{}` is not in the server-side allow-list "
            "<iceberg_biglake_metadata_service_hosts>",
            google_metadata_service);

    Poco::URI url;
    url.setScheme("http");
    url.setHost(google_metadata_service);
    url.setPath(fmt::format("{}/{}/token", DEFAULT_REQUEST_TOKEN_PATH, google_service_account));

    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, url.toString(), Poco::Net::HTTPRequest::HTTP_1_1);
    request.add("metadata-flavor", "Google");

    LOG_DEBUG(log, "Requesting Google Cloud access token from metadata service: {}", url.toString());

    context->getRemoteHostFilter().checkHostAndPort(url.getHost(), std::to_string(url.getPort()));
    auto timeouts = DB::ConnectionTimeouts::getHTTPTimeouts(context->getSettingsRef(), context->getServerSettings());
    auto session = makeHTTPSession(DB::HTTPConnectionGroupType::HTTP, url, timeouts, {});

    if (!session)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "Can not create HTTP session");
    session->sendRequest(request);

    Poco::Net::HTTPResponse response;
    auto & in = session->receiveResponse(response);

    if (response.getStatus() != Poco::Net::HTTPResponse::HTTP_OK)
    {
        throw DB::Exception(
            DB::ErrorCodes::BAD_ARGUMENTS,
            "Failed to request Google Cloud bearer token from metadata service: {} (status: {})",
            response.getReason(),
            static_cast<int>(response.getStatus()));
    }

    String token_json_raw;
    Poco::StreamCopier::copyToString(in, token_json_raw);

    LOG_DEBUG(log, "Received Google Cloud token response from metadata service");

    Poco::JSON::Parser parser;
    auto object = parser.parse(token_json_raw).extract<Poco::JSON::Object::Ptr>();

    if (!object->has("access_token") || !object->has("expires_in") || !object->has("token_type"))
    {
        throw DB::Exception(
            DB::ErrorCodes::BAD_ARGUMENTS,
            "Unexpected structure of Google Cloud token response. Response should have fields: 'access_token', 'expires_in', 'token_type'");
    }

    auto token_type = object->getValue<String>("token_type");
    if (token_type != "Bearer")
    {
        throw DB::Exception(
            DB::ErrorCodes::BAD_ARGUMENTS,
            "Unexpected token type in Google Cloud response. Expected Bearer token, got {}",
            token_type);
    }

    AccessToken token;
    token.token = object->getValue<String>("access_token");

    if (object->has("expires_in"))
    {
        Int64 expires_in = object->getValue<Int64>("expires_in");
        token.expires_at = std::chrono::system_clock::now() + std::chrono::seconds(expires_in * 9 / 10);
    }

    return token;
}

DB::ForwardedAuthTokenPtr RestCatalog::getForwardedAuthToken(const DB::ContextPtr & context_)
{
    if (!context_)
        return {};
    return context_->getForwardedAuthToken();
}

std::optional<StorageType> RestCatalog::getStorageType() const
{
    const auto state_snapshot = getStateSnapshot();
    /// Under forwarding the config is filled in lazily by the first user query.
    if (!state_snapshot->config_loaded || state_snapshot->config.default_base_location.empty())
        return std::nullopt;
    return parseStorageTypeFromLocation(state_snapshot->config.default_base_location);
}

DB::ReadWriteBufferFromHTTPPtr RestCatalog::createReadBuffer(
    const CatalogState & catalog_state,
    UInt64 generation,
    const std::string & endpoint,
    const DB::ForwardedAuthTokenPtr & auth_token,
    const Poco::URI::QueryParameters & params,
    const DB::HTTPHeaderEntries & headers,
    const std::optional<DB::HTTPHeaderEntries> & auth_headers) const
{
    const auto & context = getContext();

    /// enable_url_encoding=false to allow use tables with encoded sequences in names like 'foo%2Fbar'
    Poco::URI url(base_url / endpoint, /* enable_url_encoding */ false);
    if (!params.empty())
        url.setQueryParameters(params);

    auto create_buffer = [&](bool update_token, bool & used_cached_oauth_token)
    {
        AuthContext auth_context{
            .catalog_state = catalog_state,
            .generation = generation,
            .update_token = update_token,
            .method = Poco::Net::HTTPRequest::HTTP_GET,
            .url = url,
            .extra_headers = headers,
            .body = {},
            .auth_token = auth_token,
            .used_cached_oauth_token = &used_cached_oauth_token,
        };
        auto result_headers = auth_headers ? *auth_headers : getAuthHeaders(auth_context);
        std::move(headers.begin(), headers.end(), std::back_inserter(result_headers));

        return DB::BuilderRWBufferFromHTTP(url)
            .withConnectionGroup(DB::HTTPConnectionGroupType::HTTP)
            .withSettings(getContext()->getReadSettings())
            .withTimeouts(DB::ConnectionTimeouts::getHTTPTimeouts(context->getSettingsRef(), context->getServerSettings()))
            .withHostFilter(&getContext()->getRemoteHostFilter())
            .withHeaders(result_headers)
            .withDelayInit(false)
            .withSkipNotFound(false)
            .create(credentials);
    };

    LOG_DEBUG(log, "Requesting: {}", url.toString());

    try
    {
        bool used_cached_oauth_token = false;
        auto buf = create_buffer(false, used_cached_oauth_token);
        if (used_cached_oauth_token)
            ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogAuthTokenCachedValid);
        return buf;
    }
    catch (const DB::HTTPException & e)
    {
        const auto status = e.getHTTPStatus();
        if (!shouldRetryWithFreshToken(status))
            throw;

        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogUnauthorized);
        bool used_cached_oauth_token_on_retry = false;
        return create_buffer(true, used_cached_oauth_token_on_retry);
    }
}

bool RestCatalog::shouldRetryWithFreshToken(Poco::Net::HTTPResponse::HTTPStatus status) const
{
    /// Under forwarding the retry must never re-mint as the service principal: that would turn a
    /// denied user into a successful one and would also overwrite the catalog-wide `access_token`
    /// for everyone. Only 401 (the token may genuinely have expired mid-query) is retried, by
    /// re-running *that principal's* exchange; 403 is an authorization decision and is terminal.
    /// Passthrough has nothing to re-mint at all, so it never retries.
    if (token_forwarding.forward_user_token)
        return token_forwarding.exchangeEnabled() && status == Poco::Net::HTTPResponse::HTTPStatus::HTTP_UNAUTHORIZED;

    return update_token_if_expired
        && (status == Poco::Net::HTTPResponse::HTTPStatus::HTTP_UNAUTHORIZED
            || status == Poco::Net::HTTPResponse::HTTPStatus::HTTP_FORBIDDEN);
}

bool RestCatalog::empty(const DB::ForwardedAuthTokenPtr & auth_token) const
{
    loadConfigIfNeeded(auth_token);

    bool found_table = false;
    auto stop_condition = [&](const std::string & namespace_name) -> bool
    {
        if (found_table)
            return true;

        if (!allowed_namespaces.isNamespaceAllowed(namespace_name, /*nested*/ false))
            return false;

        const auto tables = getTablesInNamespace(namespace_name, auth_token, /* limit */1);
        if (!tables.empty())
            found_table = true;

        return found_table;
    };

    Namespaces namespaces;
    getNamespacesRecursive("", namespaces, stop_condition, /* execute_func */{}, auth_token);

    return !found_table;
}

DB::Names RestCatalog::getTables(const DB::ForwardedAuthTokenPtr & auth_token) const
{
    loadConfigIfNeeded(auth_token);

    auto & pool = getContext()->getIcebergCatalogThreadpool();
    DB::Names tables;
    std::mutex mutex;

    {
        /// Ensure tables and mutex (capture by reference) outlive runner
        DB::ThreadPoolCallbackRunnerLocal<void> runner(pool, DB::ThreadName::DATALAKE_REST_CATALOG);

        auto execute_for_each_namespace = [&](const std::string & current_namespace)
        {
            if (!allowed_namespaces.isNamespaceAllowed(current_namespace, /*nested*/ false))
                return;
            runner.enqueueAndKeepTrack(
            [=, &tables, &mutex, this]
            {
                auto tables_in_namespace = getTablesInNamespace(current_namespace, auth_token);
                std::lock_guard lock(mutex);
                std::move(tables_in_namespace.begin(), tables_in_namespace.end(), std::back_inserter(tables));
            });
        };

        Namespaces namespaces;
        getNamespacesRecursive(
            /* base_namespace */"", /// Empty base namespace means starting from root.
            namespaces,
            /* stop_condition */{},
            /* execute_func */execute_for_each_namespace,
            auth_token);

        runner.waitForAllToFinishAndRethrowFirstError();
    }

    return tables;
}

void RestCatalog::getNamespacesRecursive(
    const std::string & base_namespace,
    Namespaces & result,
    StopCondition stop_condition,
    ExecuteFunc func,
    const DB::ForwardedAuthTokenPtr & auth_token) const
{
    checkStackSize();

    auto namespaces = getNamespaces(base_namespace, auth_token);
    result.reserve(result.size() + namespaces.size());
    result.insert(result.end(), namespaces.begin(), namespaces.end());

    for (const auto & current_namespace : namespaces)
    {
        chassert(current_namespace.starts_with(base_namespace));

        /// Protection from subnamepsaces with empty names
        if (current_namespace == base_namespace)
        {
            LOG_WARNING(log, "Namespace {} has a subnamespace with empty name. This is an error in catalog implementation.", base_namespace);
            continue;
        }

        if (stop_condition && stop_condition(current_namespace))
            break;

        if (func)
        {
            if (allowed_namespaces.isNamespaceAllowed(current_namespace, /*nested*/ false))
                func(current_namespace);
            else
            {
                LOG_DEBUG(log, "Tables in namespace {} are filtered", current_namespace);
            }
        }

        if (allowed_namespaces.isNamespaceAllowed(current_namespace, /*nested*/ true))
            getNamespacesRecursive(current_namespace, result, stop_condition, func, auth_token);
        else
        {
            LOG_DEBUG(log, "Nested namespaces in namespace {} are filtered", current_namespace);
        }
    }
}

Poco::URI::QueryParameters RestCatalog::createParentNamespaceParams(const std::string & base_namespace) const
{
    std::vector<std::string_view> parts;
    splitInto<'.'>(parts, base_namespace);
    std::string parent_param;
    for (const auto & part : parts)
    {
        /// 0x1F is a unit separator
        /// https://github.com/apache/iceberg/blob/70d87f1750627b14b3b25a0216a97db86a786992/open-api/rest-catalog-open-api.yaml#L264
        if (!parent_param.empty())
            parent_param += static_cast<char>(0x1F);
        parent_param += part;
    }
    return {{"parent", parent_param}};
}

RestCatalog::Namespaces RestCatalog::getNamespaces(const std::string & base_namespace, const DB::ForwardedAuthTokenPtr & auth_token) const
{
    const auto state_snapshot = getStateSnapshot();

    Poco::URI::QueryParameters base_params;
    if (!base_namespace.empty())
        base_params = createParentNamespaceParams(base_namespace);

    Namespaces all_namespaces;
    String page_token;
    /// Cycle-detection guard: tracks every non-empty `next-page-token` we have seen on this
    /// request so we can refuse to loop when a malformed catalog repeats a token. Covers both
    /// the immediate-repeat case (`A -> A`) and longer cycles (`A -> B -> A -> ...`), since
    /// any revisit triggers a duplicate `insert`.
    std::unordered_set<String> seen_tokens;

    try
    {
        while (true)
        {
            /// The Iceberg REST OpenAPI spec uses `pageToken` (request) / `next-page-token` (response)
            /// for paginating the list-namespaces endpoint. Without this loop we silently return
            /// only the first page when the catalog server (e.g. OneLake / BigLake / Microsoft Fabric)
            /// caps the page size.
            Poco::URI::QueryParameters params = base_params;
            if (!page_token.empty())
                params.push_back({"pageToken", page_token});

            ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogGetNamespaces);
            auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogGetNamespacesMicroseconds);
            auto buf = createReadBuffer(
                *state_snapshot, state_snapshot.generation, state_snapshot->config.prefix / NAMESPACES_ENDPOINT, auth_token, params);
            String next_page_token;
            auto page_namespaces = parseNamespaces(*buf, base_namespace, next_page_token);
            LOG_DEBUG(
                log,
                "Loaded {} namespaces in base namespace `{}` (page_token=`{}`, next_page_token=`{}`)",
                page_namespaces.size(), base_namespace, page_token, next_page_token);

            all_namespaces.insert(
                all_namespaces.end(),
                std::make_move_iterator(page_namespaces.begin()),
                std::make_move_iterator(page_namespaces.end()));

            if (next_page_token.empty())
                break;
            /// Cycle guard: if the catalog returns a `next-page-token` we have already seen
            /// on this request, iterating further would loop forever. Treat it as a malformed
            /// catalog response rather than hanging `SHOW TABLES` / `system.tables`. This
            /// covers immediate repeats (`A -> A`) and longer cycles (`A -> B -> A`, etc.).
            if (!seen_tokens.insert(next_page_token).second)
                throw DB::Exception(
                    DB::ErrorCodes::DATALAKE_DATABASE_ERROR,
                    "Iceberg REST catalog returned a `next-page-token` (`{}`) already seen on this "
                    "request while listing namespaces under `{}` — refusing to loop.",
                    next_page_token, base_namespace);
            page_token = std::move(next_page_token);
        }
        return all_namespaces;
    }
    catch (const DB::HTTPException & e)
    {
        std::string message = fmt::format(
            "Received error while fetching list of namespaces from iceberg catalog `{}`. ",
            warehouse);

        if (e.code() == Poco::Net::HTTPResponse::HTTPStatus::HTTP_NOT_FOUND)
            message += "Namespace provided in the `parent` query parameter is not found. ";

        message += fmt::format(
            "Code: {}, status: {}, message: {}",
            e.code(), e.getHTTPStatus(), e.displayText());

        throw DB::Exception(DB::ErrorCodes::DATALAKE_DATABASE_ERROR, "{}", message);
    }
}

RestCatalog::Namespaces RestCatalog::parseNamespaces(DB::ReadBuffer & buf, const std::string & base_namespace, String & next_page_token) const
{
    next_page_token.clear();

    if (buf.eof())
        return {};

    String json_str;
    readJSONObjectPossiblyInvalid(json_str, buf);

    LOG_DEBUG(log, "Received response: {}", json_str);

    try
    {
        Poco::JSON::Parser parser;
        Poco::Dynamic::Var json = parser.parse(json_str);
        if (json.type() == typeid(Poco::JSON::Object::Ptr))
        {
            const Poco::JSON::Object::Ptr & obj = json.extract<Poco::JSON::Object::Ptr>();
            if (obj->size() == 0)
                return {};
        }
        const Poco::JSON::Object::Ptr & object = json.extract<Poco::JSON::Object::Ptr>();

        auto namespaces_object = object->get("namespaces").extract<Poco::JSON::Array::Ptr>();
        if (!namespaces_object)
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Cannot parse result");

        Namespaces namespaces;
        for (size_t i = 0; i < namespaces_object->size(); ++i)
        {
            auto current_namespace_array = namespaces_object->get(static_cast<int>(i)).extract<Poco::JSON::Array::Ptr>();
            if (current_namespace_array->size() == 0)
                throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Expected namespace array to be non-empty");

            const int idx = static_cast<int>(current_namespace_array->size()) - 1;
            const auto current_namespace = current_namespace_array->get(idx).extract<String>();
            /// BigLake does not support multi-level namespaces. When asked for sub-namespaces of
            /// a non-empty parent (via ?parent=X), BigLake ignores the filter and returns other
            /// top-level namespaces instead. Skip all sub-namespace results to avoid constructing
            /// fake multi-level paths like "ns1.ns2" that BigLake will reject with HTTP 400.
            if (getCatalogType() == DB::DatabaseDataLakeCatalogType::ICEBERG_BIGLAKE && !base_namespace.empty())
            {
                continue;
            }
            const auto full_namespace = base_namespace.empty()
                ? current_namespace
                : base_namespace + "." + current_namespace;

            namespaces.push_back(full_namespace);
        }

        /// Iceberg REST OpenAPI spec: response carries `next-page-token` (kebab-case).
        /// Empty / null / missing token all mean "no more pages".
        ///
        /// BigLake-non-empty-base-namespace short-circuit: when the BigLake quirk
        /// above drops every returned entry (because BigLake ignores `parent`
        /// and returns unrelated top-level namespaces), continuing pagination
        /// just burns O(pages) REST calls per parent namespace without ever
        /// contributing to the result. Treat the first page as terminal by
        /// leaving `next_page_token` empty (already cleared at function entry)
        /// so the outer `getNamespaces` loop returns immediately.
        const bool biglake_drops_all_entries
            = getCatalogType() == DB::DatabaseDataLakeCatalogType::ICEBERG_BIGLAKE
            && !base_namespace.empty();
        if (!biglake_drops_all_entries
            && object->has("next-page-token")
            && !object->isNull("next-page-token"))
        {
            next_page_token = object->get("next-page-token").extract<String>();
        }

        return namespaces;
    }
    catch (DB::Exception & e)
    {
        e.addMessage("while parsing JSON: " + json_str);
        throw;
    }
}

DB::Names RestCatalog::getTablesInNamespace(const std::string & base_namespace, const DB::ForwardedAuthTokenPtr & auth_token, size_t limit) const
{
    if (!allowed_namespaces.isNamespaceAllowed(base_namespace, /*nested*/ false))
        throw DB::Exception(DB::ErrorCodes::CATALOG_NAMESPACE_DISABLED,
            "Namespace {} is filtered by `namespaces` database parameter", base_namespace);

    const auto state_snapshot = getStateSnapshot();

    auto encoded_namespace = encodeNamespaceForURI(base_namespace);
    const std::string endpoint = std::filesystem::path(NAMESPACES_ENDPOINT) / encoded_namespace / "tables";

    DB::Names tables;
    String page_token;
    /// Cycle-detection guard: tracks every non-empty `next-page-token` we have seen on this
    /// request so we can refuse to loop when a malformed catalog repeats a token. Covers both
    /// the immediate-repeat case (`A -> A`) and longer cycles (`A -> B -> A -> ...`), since
    /// any revisit triggers a duplicate `insert`.
    std::unordered_set<String> seen_tokens;

    while (true)
    {
        /// The Iceberg REST OpenAPI spec uses `pageToken` (request) / `next-page-token` (response)
        /// for paginating the list-tables endpoint. Without this loop we silently return only the
        /// first page when the catalog server (e.g. OneLake / BigLake / Microsoft Fabric) caps the
        /// page size — making tables on later pages invisible to `SHOW TABLES` and `system.tables`.
        Poco::URI::QueryParameters params;
        if (!page_token.empty())
            params.push_back({"pageToken", page_token});

        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogGetTables);
        auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogGetTablesMicroseconds);
        auto buf = createReadBuffer(*state_snapshot, state_snapshot.generation, state_snapshot->config.prefix / endpoint, auth_token, params);

        /// Pass through the remaining limit so that single-page short-circuiting still works
        /// when the caller is in `empty()` (limit=1) and the first page already contains a row.
        const size_t remaining_limit = (limit == 0) ? 0 : (limit > tables.size() ? limit - tables.size() : 0);
        String next_page_token;
        auto page_tables = parseTables(*buf, base_namespace, remaining_limit, next_page_token);

        tables.insert(
            tables.end(),
            std::make_move_iterator(page_tables.begin()),
            std::make_move_iterator(page_tables.end()));

        if (limit && tables.size() >= limit)
            break;
        if (next_page_token.empty())
            break;
        /// Cycle guard: if the catalog returns a `next-page-token` we have already seen
        /// on this request, iterating further would loop forever. Treat it as a malformed
        /// catalog response rather than hanging `SHOW TABLES` / `system.tables`. This
        /// covers immediate repeats (`A -> A`) and longer cycles (`A -> B -> A`, etc.).
        if (!seen_tokens.insert(next_page_token).second)
            throw DB::Exception(
                DB::ErrorCodes::DATALAKE_DATABASE_ERROR,
                "Iceberg REST catalog returned a `next-page-token` (`{}`) already seen on this "
                "request while listing tables in namespace `{}` — refusing to loop.",
                next_page_token, base_namespace);
        page_token = std::move(next_page_token);
    }

    return tables;
}

DB::Names RestCatalog::parseTables(DB::ReadBuffer & buf, const std::string & base_namespace, size_t limit, String & next_page_token) const
{
    next_page_token.clear();

    if (buf.eof())
        return {};

    String json_str;
    readJSONObjectPossiblyInvalid(json_str, buf);

    LOG_DEBUG(log, "Received tables response for namespace: {}", base_namespace);

    try
    {
        Poco::JSON::Parser parser;
        Poco::Dynamic::Var json = parser.parse(json_str);
        const Poco::JSON::Object::Ptr & object = json.extract<Poco::JSON::Object::Ptr>();

        auto identifiers_object = object->get("identifiers").extract<Poco::JSON::Array::Ptr>();
        if (!identifiers_object)
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Cannot parse result");

        DB::Names tables;
        for (size_t i = 0; i < identifiers_object->size(); ++i)
        {
            const auto current_table_json = identifiers_object->get(static_cast<int>(i)).extract<Poco::JSON::Object::Ptr>();
            /// If table has encoded sequence (like 'foo%2Fbar')
            /// catalog returns decoded character instead of sequence ('foo/bar')
            /// Here name encoded back to 'foo%2Fbar' format
            const auto table_name_raw = current_table_json->get("name").extract<String>();
            std::string table_name;
            Poco::URI::encode(table_name_raw, "/", table_name);

            tables.push_back(base_namespace + "." + table_name);
            if (limit && tables.size() >= limit)
                break;
        }

        /// Iceberg REST OpenAPI spec: response carries `next-page-token` (kebab-case).
        /// Empty / null / missing token all mean "no more pages".
        if (object->has("next-page-token") && !object->isNull("next-page-token"))
            next_page_token = object->get("next-page-token").extract<String>();

        return tables;
    }
    catch (DB::Exception & e)
    {
        e.addMessage("while parsing JSON: " + json_str);
        throw;
    }
}

bool RestCatalog::existsTable(const std::string & namespace_name, const std::string & table_name, const DB::ForwardedAuthTokenPtr & auth_token) const
{
    TableMetadata table_metadata;
    /// The catalog's own (global) context is fine here -- `table_metadata` asks for neither a
    /// schema nor credentials, so it is never used to interpret a response. The identity that
    /// matters travels in `auth_token`; before forwarding existed this call had no identity
    /// channel at all and always ran as the service principal.
    return tryGetTableMetadataImpl(namespace_name, table_name, getContext(), table_metadata, auth_token);
}

bool RestCatalog::tryGetTableMetadata(
    const std::string & namespace_name,
    const std::string & table_name,
    DB::ContextPtr context_,
    TableMetadata & result) const
{
    return tryGetTableMetadataImpl(namespace_name, table_name, context_, result, getForwardedAuthToken(context_));
}

bool RestCatalog::tryGetTableMetadataImpl(
    const std::string & namespace_name,
    const std::string & table_name,
    DB::ContextPtr context_,
    TableMetadata & result,
    const DB::ForwardedAuthTokenPtr & auth_token) const
{
    try
    {
        return getTableMetadataImpl(namespace_name, table_name, context_, result, auth_token);
    }
    catch (const DB::HTTPException & ex)
    {
        if (ex.getHTTPStatus() == Poco::Net::HTTPResponse::HTTPStatus::HTTP_NOT_FOUND)
        {
            LOG_DEBUG(log, "Table {}.{} does not exist: {}", namespace_name, table_name, ex.displayText());
            return false;
        }
        throw;
    }
}

void RestCatalog::getTableMetadata(
    const std::string & namespace_name,
    const std::string & table_name,
    DB::ContextPtr context_,
    TableMetadata & result) const
{
    if (!getTableMetadataImpl(namespace_name, table_name, context_, result, getForwardedAuthToken(context_)))
        throw DB::Exception(DB::ErrorCodes::DATALAKE_DATABASE_ERROR, "No response from iceberg catalog");
}

namespace
{

/// Effective vended-credentials config of a LoadTableResult: "config" overlaid with the
/// "storage-credentials" entry whose prefix is the longest match of the location.
/// Returns nullptr when the response contains neither.
Poco::JSON::Object::Ptr effectiveVendedConfig(const Poco::JSON::Object::Ptr & load_table_result, const std::string & location)
{
    static constexpr auto storage_credentials_str = "storage-credentials";

    Poco::JSON::Object::Ptr config_object;
    if (load_table_result->has("config"))
    {
        config_object = load_table_result->getObject("config");
        if (!config_object)
            throw DB::Exception(DB::ErrorCodes::DATALAKE_DATABASE_ERROR, "Cannot parse config result");
    }

    const auto entries
        = load_table_result->isArray(storage_credentials_str) ? load_table_result->getArray(storage_credentials_str) : nullptr;

    Poco::JSON::Object::Ptr best_config;
    size_t best_prefix_size = 0;
    for (size_t i = 0; entries && i < entries->size(); ++i)
    {
        const auto entry = entries->getObject(static_cast<unsigned>(i));
        if (!entry)
            continue;
        const auto prefix_var = entry->get("prefix");
        if (!prefix_var.isString())
            continue;
        const auto & prefix = prefix_var.extract<String>();
        if (!location.starts_with(prefix))
            continue;
        const auto entry_config = entry->getObject("config");
        if (!entry_config)
            continue;
        if (!best_config || prefix.size() > best_prefix_size)
        {
            best_config = entry_config;
            best_prefix_size = prefix.size();
        }
    }

    if (!best_config)
        return config_object;
    if (!config_object)
        config_object = new Poco::JSON::Object();

    Poco::JSON::Object::Ptr merged = new Poco::JSON::Object(*config_object);
    std::vector<std::string> names;
    best_config->getNames(names);

    /// An entry that supplies any key of a credential group replaces that whole group.
    static const std::vector<std::vector<std::string>> credential_groups = {
        {"s3.access-key-id", "s3.secret-access-key", "s3.session-token", "s3.session-token-expires-at-ms"},
        {"gcs.oauth2.token", "gcs.oauth2.token-expires-at"},
    };
    for (const auto & group : credential_groups)
        if (std::any_of(group.begin(), group.end(), [&](const auto & key) { return best_config->has(key); }))
            for (const auto & key : group)
                merged->remove(key);

    /// Azure SAS tokens form one group keyed by account name (adls.sas-token.<account>...).
    static constexpr auto sas_prefix = "adls.sas-token.";
    if (std::any_of(names.begin(), names.end(), [](const auto & name) { return name.starts_with(sas_prefix); }))
    {
        std::vector<std::string> base_names;
        merged->getNames(base_names);
        for (const auto & name : base_names)
            if (name.starts_with(sas_prefix))
                merged->remove(name);
    }

    for (const auto & name : names)
        merged->set(name, best_config->get(name));

    return merged;
}

}

bool RestCatalog::getTableMetadataImpl(
    const std::string & namespace_name,
    const std::string & table_name,
    DB::ContextPtr context_,
    TableMetadata & result,
    const DB::ForwardedAuthTokenPtr & auth_token,
    bool allow_credentials_cache) const
{
    LOG_DEBUG(log, "Checking table {} in namespace {}", table_name, namespace_name);

    loadConfigIfNeeded(auth_token);

    if (!allowed_namespaces.isNamespaceAllowed(namespace_name, /*nested*/ false))
        throw DB::Exception(DB::ErrorCodes::CATALOG_NAMESPACE_DISABLED,
            "Namespace {} is filtered by `namespaces` database parameter", namespace_name);

    DB::HTTPHeaderEntries headers;

    const auto state_snapshot = getStateSnapshot();

    const bool want_credentials = result.requiresCredentials();
    const CredentialsCacheKey credentials_key{
        state_snapshot.generation, getCredentialsCachePrincipal(auth_token), namespace_name, table_name};

    /// Reuse previously vended credentials is possible
    std::optional<VendedStorageCredentials> cached_credentials;
    if (want_credentials)
    {
        if (allow_credentials_cache)
            cached_credentials = tryGetCachedCredentials(credentials_key);

        /// Header `X-Iceberg-Access-Delegation` tells catalog to include storage credentials in LoadTableResponse.
        /// Value can be one of the two:
        /// 1. `vended-credentials`
        /// 2. `remote-signing`
        /// Currently we support only the first.
        /// https://github.com/apache/iceberg/blob/3badfe0c1fcf0c0adfc7aa4a10f0b50365c48cf9/open-api/rest-catalog-open-api.yaml#L1832
        if (!cached_credentials)
        {
            ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogCredentialsVended);
            ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogCredentialsCacheMisses);
            headers.emplace_back("X-Iceberg-Access-Delegation", "vended-credentials");
        }
    }

    const std::string endpoint = std::filesystem::path(NAMESPACES_ENDPOINT) / encodeNamespaceForURI(namespace_name) / "tables" / table_name;
    String json_str;

    {
        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogGetTableMetadata);
        auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogGetTableMetadataMicroseconds);
        auto buf = createReadBuffer(
            *state_snapshot, state_snapshot.generation, state_snapshot->config.prefix / endpoint, auth_token, /* params */{}, headers);

        if (buf->eof())
        {
            LOG_DEBUG(log, "Table doesn't exist (endpoint: {})", endpoint);
            return false;
        }

        readJSONObjectPossiblyInvalid(json_str, *buf);
    }

#ifdef DEBUG_OR_SANITIZER_BUILD
    /// This log message might contain credentials,
    /// so log it only for debugging.
    LOG_DEBUG(log, "Received metadata for table {}: {}", table_name, json_str);
#endif

    Poco::JSON::Parser parser;
    Poco::Dynamic::Var json = parser.parse(json_str);
    const Poco::JSON::Object::Ptr & object = json.extract<Poco::JSON::Object::Ptr>();

    auto metadata_object = object->get("metadata").extract<Poco::JSON::Object::Ptr>();
    if (!metadata_object)
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Cannot parse result");

    const std::string table_uuid = parseTableUuid(metadata_object);

    std::string location;
    if (result.requiresLocation())
    {
        if (metadata_object->has("location"))
        {
            location = metadata_object->get("location").extract<String>();
            result.setLocation(location);
            LOG_DEBUG(log, "Location for table {}: {}", table_name, location);
        }
        else
        {
            result.setTableIsNotReadable(fmt::format("Cannot read table {}, because no 'location' in response", table_name));
        }
    }

    if (result.requiresSchema())
    {
        const bool allow_geo_parser
            = getContext()->getSettingsRef()[DB::Setting::allow_experimental_geo_types_in_iceberg].value;
        auto schema_processor = DB::Iceberg::IcebergSchemaProcessor(context_, allow_geo_parser);
        auto id = DB::IcebergMetadata::parseTableSchema(metadata_object, schema_processor, context_, log);
        auto schema = schema_processor.getClickhouseTableSchemaById(id);
        result.setSchema(*schema);
    }

    if (want_credentials && result.isDefaultReadableTable())
    {
        if (cached_credentials)
        {
            /// Reuse the cached credentials only for the very same table with the very same UUID.
            if (table_uuid.empty() || cached_credentials->table_uuid != table_uuid)
            {
                {
                    std::lock_guard lock(credentials_cache_mutex);
                    credentials_cache.erase(credentials_key);
                }
                return getTableMetadataImpl(namespace_name, table_name, context_, result, auth_token, /* allow_credentials_cache */ false);
            }
            ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogCredentialsCacheHits);
            result.setStorageCredentials(cached_credentials->credentials);
            if (!cached_credentials->endpoint.empty())
                result.setEndpoint(cached_credentials->endpoint);
        }
        else if (const auto config_object = effectiveVendedConfig(object, location))
        {
            auto parsed = getCredentialsAndEndpoint(config_object, location);
            parsed.table_uuid = table_uuid;
            if (parsed.credentials)
            {
                result.setStorageCredentials(parsed.credentials);
                cacheCredentials(credentials_key, parsed);
            }
            if (!parsed.endpoint.empty())
                result.setEndpoint(parsed.endpoint);
        }
    }

    if (result.requiresDataLakeSpecificProperties())
    {
        if (object->has("metadata-location") && !object->get("metadata-location").isEmpty())
        {
            auto metadata_location = object->get("metadata-location").extract<String>();
            result.setDataLakeSpecificProperties(DataLakeSpecificProperties{ .iceberg_metadata_file_location = metadata_location });
        }
    }

    if (!table_uuid.empty())
        result.setTableUUID(table_uuid);

    return true;
}

void RestCatalog::sendRequest(
    const CatalogState & catalog_state,
    UInt64 generation,
    const String & endpoint,
    Poco::JSON::Object::Ptr request_body,
    const DB::ForwardedAuthTokenPtr & auth_token,
    const String & method,
    bool ignore_result) const
{
    std::ostringstream oss;  // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    if (request_body)
        request_body->stringify(oss);
    const std::string body_str = DB::removeEscapedSlashes(oss.str());

    const auto & context = getContext();

    DB::ReadWriteBufferFromHTTP::OutStreamCallback out_stream_callback;
    if (!body_str.empty())
    {
        out_stream_callback = [body_str](std::ostream & os)
        {
            os << body_str;
        };
    }

    /// enable_url_encoding=false to allow use tables with encoded sequences in names like 'foo%2Fbar'
    Poco::URI url(endpoint, /* enable_url_encoding */ false);

    DB::HTTPHeaderEntries extra_headers;
    extra_headers.emplace_back("Content-Type", "application/json");

    /// `update_token = false` plus a 401 retry, mirroring `createReadBuffer`. Unconditionally
    /// re-minting cost a full token round trip on every catalog mutation; under forwarding it
    /// would cost a full token *exchange* per mutation.
    auto create_buffer = [&](bool update_token, bool & used_cached_oauth_token)
    {
        AuthContext auth_context{
            .catalog_state = catalog_state,
            .generation = generation,
            .update_token = update_token,
            .method = method,
            .url = url,
            .extra_headers = extra_headers,
            .body = body_str,
            .auth_token = auth_token,
            .used_cached_oauth_token = &used_cached_oauth_token,
        };
        DB::HTTPHeaderEntries headers = getAuthHeaders(auth_context);
        headers.emplace_back("Content-Type", "application/json");
        return DB::BuilderRWBufferFromHTTP(url)
            .withConnectionGroup(DB::HTTPConnectionGroupType::HTTP)
            .withMethod(method)
            .withSettings(context->getReadSettings())
            .withTimeouts(DB::ConnectionTimeouts::getHTTPTimeouts(context->getSettingsRef(), context->getServerSettings()))
            .withHostFilter(&context->getRemoteHostFilter())
            .withHeaders(headers)
            .withOutCallback(out_stream_callback)
            .withSkipNotFound(false)
            .create(credentials);
    };

    try
    {
        bool used_cached_oauth_token = false;
        auto wb = create_buffer(false, used_cached_oauth_token);

        String response_str;
        if (!ignore_result)
            readJSONObjectPossiblyInvalid(response_str, *wb);
        else
            wb->ignoreAll();

        if (used_cached_oauth_token)
            ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogAuthTokenCachedValid);
    }
    catch (const DB::HTTPException & e)
    {
        if (!shouldRetryWithFreshToken(e.getHTTPStatus()))
            throw;

        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogUnauthorized);
        bool used_cached_oauth_token_on_retry = false;
        auto wb = create_buffer(true, used_cached_oauth_token_on_retry);

        String response_str;
        if (!ignore_result)
            readJSONObjectPossiblyInvalid(response_str, *wb);
        else
            wb->ignoreAll();
    }
}

void RestCatalog::createNamespaceIfNotExists(const String & namespace_name, const String & location, const DB::ForwardedAuthTokenPtr & auth_token) const
{
    loadConfigIfNeeded(auth_token);

    const auto state_snapshot = getStateSnapshot();

    /// Check existence first: creation may be denied to a principal that is still
    /// allowed to use a pre-provisioned namespace.
    const std::string check_endpoint
        = (base_url / state_snapshot->config.prefix / NAMESPACES_ENDPOINT / encodeNamespaceForURI(namespace_name)).generic_string();
    try
    {
        sendRequest(
            *state_snapshot, state_snapshot.generation, check_endpoint, /* request_body */ nullptr, auth_token,
            Poco::Net::HTTPRequest::HTTP_GET, /* ignore_result */ true);
        return;
    }
    catch (const DB::HTTPException & e)
    {
        if (e.getHTTPStatus() != Poco::Net::HTTPResponse::HTTPStatus::HTTP_NOT_FOUND)
            throw;
    }

    const std::string endpoint = (base_url / state_snapshot->config.prefix / NAMESPACES_ENDPOINT).generic_string();

    Poco::JSON::Object::Ptr request_body = new Poco::JSON::Object;
    {
        Poco::JSON::Array::Ptr namespaces = new Poco::JSON::Array;
        namespaces->add(namespace_name);
        request_body->set("namespace", namespaces);
    }
    {
        Poco::JSON::Object::Ptr properties = new Poco::JSON::Object;
        properties->set("location", location);
        request_body->set("properties", properties);
    }

    try
    {
        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogCreateNamespace);
        auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogCreateNamespaceMicroseconds);
        sendRequest(*state_snapshot, state_snapshot.generation, endpoint, request_body, auth_token);
    }
    catch (const DB::HTTPException & e)
    {
        /// Lost the race to a concurrent creator.
        if (e.getHTTPStatus() != Poco::Net::HTTPResponse::HTTPStatus::HTTP_CONFLICT)
            throw;
    }
}

void RestCatalog::createTable(const String & namespace_name, const String & table_name, const String & /*new_metadata_path*/, Poco::JSON::Object::Ptr metadata_content, const DB::ForwardedAuthTokenPtr & auth_token) const
{
    loadConfigIfNeeded(auth_token);

    if (!allowed_namespaces.isNamespaceAllowed(namespace_name, /*nested*/ false))
        throw DB::Exception(DB::ErrorCodes::CATALOG_NAMESPACE_DISABLED,
            "Failed to create table {}, namespace {} is filtered by `namespaces` database parameter", table_name, namespace_name);

    const auto state_snapshot = getStateSnapshot();
    const std::string endpoint = (base_url / state_snapshot->config.prefix / NAMESPACES_ENDPOINT / encodeNamespaceForURI(namespace_name) / "tables").generic_string();

    Poco::JSON::Object::Ptr request_body = new Poco::JSON::Object;
    request_body->set("name", table_name);
    request_body->set("location", metadata_content->getValue<String>("location"));
    {
        Poco::JSON::Object::Ptr initial_schema = metadata_content->getArray("schemas")->getObject(0);
        Poco::JSON::Array::Ptr identifier_fields = new Poco::JSON::Array;
        initial_schema->set(IDENTIFIER_FIELD_IDS, identifier_fields);
        request_body->set("schema", initial_schema);
    }
    request_body->set("partition-spec", metadata_content->getArray("partition-specs")->get(0));

    {
        Poco::JSON::Object::Ptr write_order = new Poco::JSON::Object;
        write_order->set("order-id", 0);
        Poco::JSON::Array::Ptr fields = new Poco::JSON::Array;
        write_order->set("fields", fields);
        request_body->set("write-order", write_order);
    }
    request_body->set("stage-create", false);
    Poco::JSON::Object::Ptr properties = new Poco::JSON::Object;

    if (metadata_content->has("format-version"))
        properties->set("format-version", std::to_string(metadata_content->getValue<int>("format-version")));

    request_body->set("properties", properties);

    try
    {
        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogCreateTable);
        auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogCreateTableMicroseconds);
        sendRequest(*state_snapshot, state_snapshot.generation, endpoint, request_body, auth_token);
    }
    catch (const DB::HTTPException & ex)
    {
        throw DB::Exception(DB::ErrorCodes::DATALAKE_DATABASE_ERROR, "Failed to create table {}", ex.displayText());
    }
}


bool RestCatalog::updateMetadata(const String & namespace_name, const String & table_name, const String & /*new_metadata_path*/, Poco::JSON::Object::Ptr new_snapshot, const DB::ForwardedAuthTokenPtr & auth_token) const
{
    loadConfigIfNeeded(auth_token);

    if (!new_snapshot)
        throw DB::Exception(
            DB::ErrorCodes::NOT_IMPLEMENTED,
            "REST catalog does not support metadata-only updates without a snapshot "
            "(required for EXPIRE SNAPSHOTS)");

    const auto state_snapshot = getStateSnapshot();
    const std::string endpoint = (base_url / state_snapshot->config.prefix / NAMESPACES_ENDPOINT / encodeNamespaceForURI(namespace_name) / "tables" / table_name).generic_string();

    auto request_body = buildUpdateMetadataRequestBody(namespace_name, table_name, new_snapshot);

    try
    {
        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogUpdateTable);
        auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogUpdateTableMicroseconds);
        sendRequest(*state_snapshot, state_snapshot.generation, endpoint, request_body, auth_token);
    }
    catch (const DB::HTTPException & ex)
    {
        const auto status = static_cast<int>(ex.getHTTPStatus());
        if (status == 408 || status == 409 || status == 429 || status >= 500)
        {
            LOG_WARNING(log, "Iceberg REST updateMetadata for {}.{} got retryable HTTP {}: {}",
                namespace_name, table_name, status, ex.displayText());
            return false;
        }
        throw;
    }
    return true;
}

bool RestCatalog::updateSchema(
    const String & namespace_name,
    const String & table_name,
    const String & /*new_metadata_path*/,
    Poco::JSON::Object::Ptr new_schema,
    Int32 previous_schema_id,
    Int32 new_last_column_id,
    Poco::JSON::Object::Ptr metadata,
    const DB::ForwardedAuthTokenPtr & auth_token) const
{
    fiu_do_on(DB::FailPoints::iceberg_alter_catalog_update_schema_fail, { return false; });

    loadConfigIfNeeded(auth_token);

    const auto state_snapshot = getStateSnapshot();
    const std::string endpoint = (base_url / state_snapshot->config.prefix / NAMESPACES_ENDPOINT / encodeNamespaceForURI(namespace_name) / "tables" / table_name).generic_string();

    auto request_body = buildUpdateSchemaRequestBody(
        namespace_name, table_name, metadata, new_schema, previous_schema_id, new_last_column_id);

    try
    {
        sendRequest(*state_snapshot, state_snapshot.generation, endpoint, request_body, auth_token);
    }
    catch (const DB::HTTPException & ex)
    {
        const auto status = static_cast<int>(ex.getHTTPStatus());
        if (status == 408 || status == 409 || status == 429 || status >= 500)
        {
            LOG_WARNING(log, "Iceberg REST updateSchema for {}.{} got retryable HTTP {}: {}",
                namespace_name, table_name, status, ex.displayText());
            return false;
        }
        throw;
    }

    /// Simulates the Iceberg "commit state unknown" case: the catalog applied the update but the
    /// client observes a failure, e.g. because a proxy turned the response into a 5xx.
    fiu_do_on(DB::FailPoints::iceberg_alter_catalog_commit_reported_as_failed, { return false; });

    return true;
}

void RestCatalog::dropTable(const String & namespace_name, const String & table_name, const DB::ForwardedAuthTokenPtr & auth_token) const
{
    loadConfigIfNeeded(auth_token);

    if (!allowed_namespaces.isNamespaceAllowed(namespace_name, /*nested*/ false))
        throw DB::Exception(DB::ErrorCodes::CATALOG_NAMESPACE_DISABLED,
            "Failed to drop table {}, namespace {} is filtered by `namespaces` database parameter",
            table_name, namespace_name);

    const auto state_snapshot = getStateSnapshot();
    const std::string endpoint
        = (base_url / state_snapshot->config.prefix / NAMESPACES_ENDPOINT / encodeNamespaceForURI(namespace_name) / "tables" / table_name).generic_string()
        + "?purgeRequested=False";

    Poco::JSON::Object::Ptr request_body = nullptr;
    try
    {
        ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogDropTable);
        auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogDropTableMicroseconds);
        sendRequest(
            *state_snapshot, state_snapshot.generation, endpoint, request_body, auth_token, Poco::Net::HTTPRequest::HTTP_DELETE, true);
    }
    catch (const DB::HTTPException & ex)
    {
        throw DB::Exception(DB::ErrorCodes::DATALAKE_DATABASE_ERROR, "Failed to drop table {}", ex.displayText());
    }
}

namespace
{
/// Parse a "...-expires-at-ms" value (ms since epoch); nullopt if absent, epoch (= don't cache)
/// if invalid; values beyond the representable range are clamped to the maximum time point.
std::optional<std::chrono::system_clock::time_point>
parseExpiresAtMs(const Poco::JSON::Object::Ptr & object, const std::string & key)
{
    if (!object->has(key))
        return std::nullopt;
    try
    {
        static constexpr Int64 max_representable_sec
            = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::duration::max()).count();
        const Int64 expires_at_ms = object->get(key).convert<Int64>();
        if (expires_at_ms <= 0)
            return std::chrono::system_clock::time_point{};
        if (expires_at_ms / 1000 < max_representable_sec)
            return std::chrono::system_clock::from_time_t(static_cast<std::time_t>(expires_at_ms / 1000));
        return std::chrono::system_clock::time_point::max();
    }
    catch (...) // NOLINT(bugprone-empty-catch) Ok: fail close below
    {
    }
    return std::chrono::system_clock::time_point{};
}

std::chrono::system_clock::time_point parseSasTokenExpiry(const std::string & sas_token)
{
    std::string token = sas_token;
    if (!token.empty() && token.front() == '?')
        token.erase(0, 1);

    Poco::StringTokenizer params(token, "&", Poco::StringTokenizer::TOK_IGNORE_EMPTY | Poco::StringTokenizer::TOK_TRIM);
    for (const auto & param : params)
    {
        if (!param.starts_with("se="))
            continue;

        try
        {
            std::string decoded;
            Poco::URI::decode(param.substr(3), decoded);

            int time_zone_differential = 0;
            Poco::DateTime date_time;
            if (Poco::DateTimeParser::tryParse(Poco::DateTimeFormat::ISO8601_FORMAT, decoded, date_time, time_zone_differential))
            {
                date_time.makeUTC(time_zone_differential);
                return std::chrono::system_clock::from_time_t(date_time.timestamp().epochTime());
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch) Ok: handled by the fail-close return below
        {
        }

        break;
    }
    /// Absent or unparseable 'se': do not cache.
    return std::chrono::system_clock::time_point{};
}
}

VendedStorageCredentials RestCatalog::getCredentialsAndEndpoint(Poco::JSON::Object::Ptr object, const String & location) const
{
    auto storage_type = parseStorageTypeFromLocation(location);
    switch (storage_type)
    {
        case StorageType::S3:
        {
            static constexpr auto gcs_token_str = "gcs.oauth2.token";
            static constexpr auto gcs_token_expires_at_str = "gcs.oauth2.token-expires-at";
            static constexpr auto access_key_id_str = "s3.access-key-id";
            static constexpr auto secret_access_key_str = "s3.secret-access-key";
            static constexpr auto session_token_str = "s3.session-token";
            static constexpr auto storage_endpoint_str = "s3.endpoint";
            static constexpr auto session_token_expires_at_ms_str = "s3.session-token-expires-at-ms";

            /// gs:// also maps to StorageType::S3, and the merged config may carry a warehouse-level
            /// GCS token alongside per-table S3 keys, so select GCS by scheme, not by key presence.
            if (location.starts_with("gs://") && object->has(gcs_token_str))
            {
                auto gcs_token = object->get(gcs_token_str).extract<String>();
                LOG_DEBUG(log, "Using GCS OAuth2 token for location {}", location);
                /// Do not cache if expiry was not parsed.
                auto expires_at = parseExpiresAtMs(object, gcs_token_expires_at_str).value_or(std::chrono::system_clock::time_point{});
                return {std::make_shared<GCSCredentials>(gcs_token), "", expires_at};
            }

            std::string access_key_id;
            std::string secret_access_key;
            std::string session_token;
            std::string storage_endpoint;
            std::optional<std::chrono::system_clock::time_point> expires_at;
            if (object->has(access_key_id_str))
                access_key_id = object->get(access_key_id_str).extract<String>();
            if (object->has(secret_access_key_str))
                secret_access_key = object->get(secret_access_key_str).extract<String>();
            if (object->has(session_token_str))
                session_token = object->get(session_token_str).extract<String>();
            if (object->has(storage_endpoint_str))
                storage_endpoint = object->get(storage_endpoint_str).extract<String>();
            expires_at = parseExpiresAtMs(object, session_token_expires_at_ms_str);
            /// Temporary credentials (session token) with unreported expiry must not be cached (fail close).
            if (!expires_at.has_value() && !session_token.empty())
                expires_at = std::chrono::system_clock::time_point{};

            LOG_DEBUG(log, "get tokens for location {}", location);
            return {std::make_shared<S3Credentials>(access_key_id, secret_access_key, session_token), storage_endpoint, expires_at};
        }
        case StorageType::Azure:
        {
            /// Azure ADLS Gen2 vended credentials use SAS tokens.
            /// The config keys follow the pattern: adls.sas-token.<account_name>
            /// or adls.sas-token.<account_name>.dfs.core.windows.net
            /// We look for any key starting with "adls.sas-token." and use the first one found.
            String sas_token;
            std::vector<std::string> names;
            object->getNames(names);
            for (const auto & name : names)
            {
                if (name.starts_with("adls.sas-token."))
                {
                    sas_token = object->get(name).extract<String>();
                    LOG_DEBUG(log, "Found Azure SAS token with key: {}", name);
                    break;
                }
            }

            if (!sas_token.empty())
                return {std::make_shared<AzureCredentials>(sas_token), "", parseSasTokenExpiry(sas_token)};
            break;
        }
        default:
            break;
    }
    return {nullptr, "", std::nullopt};
}

String RestCatalog::getCredentialsCachePrincipal(const DB::ForwardedAuthTokenPtr & auth_token) const
{
    /// Empty when forwarding is off: the catalog vends the same service-principal credentials to
    /// everyone, so the pre-forwarding `(namespace, table)` key semantics are exactly right and
    /// the existing cache tests see an unchanged sequence of events.
    if (!token_forwarding.forward_user_token || !auth_token)
        return {};
    /// The fingerprint rather than the user name: rotating a token must not reuse the credentials
    /// vended for the token it replaced.
    return auth_token->fingerprint;
}

std::optional<VendedStorageCredentials> RestCatalog::tryGetCachedCredentials(const CredentialsCacheKey & key) const
{
    if (vended_credentials_cache_ttl.load(std::memory_order_relaxed) <= std::chrono::seconds::zero())
        return std::nullopt;

    std::lock_guard lock(credentials_cache_mutex);
    auto it = credentials_cache.find(key);
    if (it == credentials_cache.end())
        return std::nullopt;
    if (std::chrono::system_clock::now() >= it->second.expires_at.value())
    {
        credentials_cache.erase(it); /// Drop the stale entry.
        return std::nullopt;
    }

    return it->second;
}

void RestCatalog::cacheCredentials(const CredentialsCacheKey & key, const VendedStorageCredentials & parsed) const
{
    const auto ttl = vended_credentials_cache_ttl.load(std::memory_order_relaxed);
    if (ttl <= std::chrono::seconds::zero())
        return;

    if (!parsed.credentials || parsed.credentials->isEmpty())
        return;

    const auto now = std::chrono::system_clock::now();

    /// Cap at the configured TTL so an entry never outlives the documented maximum lifetime.
    auto refresh_after = now + ttl;
    if (parsed.expires_at)
    {
        const auto safe_expiry = parsed.expires_at.value() - credentials_expiry_safety_window;
        if (safe_expiry < refresh_after)
            refresh_after = safe_expiry;
    }
    if (refresh_after <= now)
        return;

    std::lock_guard lock(credentials_cache_mutex);

    if (credentials_cache.size() >= credentials_cache_cleanup_threshold)
        std::erase_if(credentials_cache, [&now](const auto & entry) { return now >= entry.second.expires_at.value(); });

    /// The sweep above only removes what has already expired, which is not a bound: with
    /// per-principal keys the cache is O(users x tables), so enforce a real capacity by evicting
    /// the entries that expire soonest.
    while (credentials_cache.size() >= credentials_cache_max_entries)
    {
        auto oldest = std::min_element(
            credentials_cache.begin(),
            credentials_cache.end(),
            [](const auto & lhs, const auto & rhs) { return lhs.second.expires_at.value() < rhs.second.expires_at.value(); });
        if (oldest == credentials_cache.end())
            break;
        credentials_cache.erase(oldest);
    }

    credentials_cache[key]
        = VendedStorageCredentials{parsed.credentials, parsed.endpoint, refresh_after, parsed.table_uuid};
}

ICatalog::CredentialsRefreshCallback RestCatalog::getCredentialsConfigurationCallback(
    const DB::StorageID & storage_id, const DB::ForwardedAuthTokenPtr & auth_token)
{
    /// `auth_token` is captured by value so that a mid-query credential refresh re-vends as the
    /// same user and writes back to the same cache key. The consequence, documented rather than
    /// fixed: the raw token then lives inside the object storage's credential refresher for the
    /// lifetime of the per-query storage, so it can appear in a core dump.
    return [this, storage_id, auth_token] () -> std::shared_ptr<IStorageCredentials>
    {
        LOG_DEBUG(log, "Update credentials in the catalog");

        DB::HTTPHeaderEntries headers;
        headers.emplace_back("X-Iceberg-Access-Delegation", "vended-credentials");

        const auto state_snapshot = getStateSnapshot();
        const auto & table = storage_id.getTableName();
        auto [namespace_name, table_name] = DataLake::parseTableName(table);
        const std::string endpoint = std::filesystem::path(NAMESPACES_ENDPOINT) / encodeNamespaceForURI(namespace_name) / "tables" / table_name;
        String json_str;

        {
            ProfileEvents::increment(ProfileEvents::DataLakeRestCatalogGetCredentials);
            auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::DataLakeRestCatalogGetCredentialsMicroseconds);
            auto buf = createReadBuffer(
                *state_snapshot, state_snapshot.generation, state_snapshot->config.prefix / endpoint, auth_token, /* params */{}, headers);

            if (buf->eof())
            {
                LOG_DEBUG(log, "Table doesn't exist (endpoint: {})", endpoint);
                return nullptr;
            }

            readJSONObjectPossiblyInvalid(json_str, *buf);
        }

        Poco::JSON::Parser parser;
        Poco::Dynamic::Var json = parser.parse(json_str);
        const Poco::JSON::Object::Ptr & object = json.extract<Poco::JSON::Object::Ptr>();

        Poco::JSON::Object::Ptr metadata_object;
        if (object->has("metadata"))
            metadata_object = object->get("metadata").extract<Poco::JSON::Object::Ptr>();

        /// Prefix matching uses the table location; metadata may live outside it with a custom `write.metadata.path`.
        std::string location;
        if (metadata_object && metadata_object->has("location"))
            location = metadata_object->get("location").extract<String>();
        else if (object->has("metadata-location"))
            location = object->get("metadata-location").extract<String>();
        else
            throw DB::Exception(DB::ErrorCodes::DATALAKE_DATABASE_ERROR, "Cannot read table {}, because no location in response", table_name);
        LOG_DEBUG(log, "Location for table {}: {}", table_name, location);

        const auto config_object = effectiveVendedConfig(object, location);
        if (!config_object)
        {
            LOG_DEBUG(log, "No credentials in response for table {} – catalog does not support credential vending", table_name);
            return nullptr;
        }

        auto parsed = getCredentialsAndEndpoint(config_object, location);
        if (metadata_object)
            parsed.table_uuid = parseTableUuid(metadata_object);
        /// Refresh the per-table cache so subsequent queries reuse these freshly vended credentials.
        cacheCredentials(
            {state_snapshot.generation, getCredentialsCachePrincipal(auth_token), namespace_name, table_name}, parsed);
        return parsed.credentials;
    };
}

/// "alpha,alpha.a1,bravo,bravo.*,charlie,delta.d1,echo.*"
/// allows tables from
/// - "alpha" namespace
/// - "alpha.a1" namespace
/// - "bravo" namespace
/// - any nested namespaces of "bravo"
/// - "charlie" namespace, but not from nested of "charlie"
/// - "delta.d1" namespace, but not from "delta"
/// - any nested namespaces of "echo", but not "echo" itself
/// "bravo.*.b2" makes no sense for now, asterisk allows all nested
RestCatalog::AllowedNamespaces::AllowedNamespaces(const std::string & namespaces_)
{
    std::vector<std::string> list_of_namespaces;
    boost::split(list_of_namespaces, namespaces_, boost::is_any_of(", "), boost::token_compress_on);
    for (const auto & ns : list_of_namespaces)
    {
        std::vector<std::string> list_of_nested_namespaces;
        boost::split(list_of_nested_namespaces, ns, boost::is_any_of("."));

        size_t len = list_of_nested_namespaces.size();
        if (!len)
            continue;

        AllowedNamespaces * current = &(nested_namespaces[list_of_nested_namespaces[0]]);
        for (size_t i = 1; i <= len; ++i)
        {
            if (i == len)
                current->allow_tables = true;
            else
            {
                current = &(current->nested_namespaces[list_of_nested_namespaces[i]]);
                if (list_of_nested_namespaces[i] == "*")
                {
                    current->allow_tables = true;
                    break;
                }
            }
        }
    }
}

bool RestCatalog::AllowedNamespaces::isNamespaceAllowed(const std::string & namespace_, bool nested) const
{
    // Trivial case, check here to avoid split namespace on nested
    if (nested_namespaces.contains("*"))
        return true;

    std::vector<std::string> list_of_nested_namespaces;
    boost::split(list_of_nested_namespaces, namespace_, boost::is_any_of("."));

    const AllowedNamespaces * current = this;
    for (const auto & nns : list_of_nested_namespaces)
    {
        if (current->nested_namespaces.contains("*"))
            return true;
        auto it = current->nested_namespaces.find(nns);
        if (it == current->nested_namespaces.end())
            return false;
        current = &(it->second);
    }

    return nested ? !current->nested_namespaces.empty() : current->allow_tables;
}

}

#endif
