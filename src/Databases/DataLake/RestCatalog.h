#pragma once
#include "config.h"

#if USE_AVRO
#include <Access/ForwardedAuthToken.h>
#include <Databases/DataLake/ICatalog.h>
#include <Poco/Net/HTTPBasicCredentials.h>
#include <Poco/Net/HTTPResponse.h>
#include <Common/CacheBase.h>
#include <Common/MultiVersion.h>
#include <IO/ReadWriteBufferFromHTTP.h>
#include <IO/HTTPHeaderEntries.h>
#include <Interpreters/Context_fwd.h>
#include <base/defines.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <Poco/JSON/Object.h>

namespace DB
{
class ReadBuffer;
}

namespace DataLake
{

struct AccessToken
{
    std::string token;
    std::optional<std::chrono::system_clock::time_point> expires_at;

    bool isExpired() const
    {
        if (!expires_at.has_value())
            return false;
        return std::chrono::system_clock::now() >= expires_at.value();
    }
};

struct VendedStorageCredentials
{
    std::shared_ptr<IStorageCredentials> credentials;
    std::string endpoint;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::string table_uuid = {};
};

/// Per-database configuration of forwarding the querying user's own token to the catalog.
/// A struct rather than six more positional constructor parameters.
///
/// There is deliberately no mode enum: the presence of `token_exchange_uri` *is* the mode.
/// Empty means passthrough -- the user's bearer token is presented to the catalog unchanged,
/// which is what Lakekeeper, Nessie and Polaris-with-an-external-IdP accept. Non-empty switches
/// to an RFC 8693 token exchange against that URL, which may be the IdP's token endpoint (the
/// flow Lakekeeper documents) or a catalog's `/v1/oauth/tokens` (deprecated for removal in the
/// Iceberg REST spec, hence reachable only by writing its URL out in full).
struct TokenForwardingConfig
{
    bool forward_user_token = false;
    String token_exchange_uri;
    String subject_token_type;
    String requested_token_type;
    bool forward_actor_token = false;
    UInt64 user_token_cache_ttl = 0;

    bool exchangeEnabled() const { return forward_user_token && !token_exchange_uri.empty(); }
};

/// One OAuth token-endpoint request. `ClientCredentials` reproduces the pre-existing service
/// principal grant byte for byte; `TokenExchange` is RFC 8693.
struct TokenRequest
{
    enum class Grant
    {
        ClientCredentials,
        TokenExchange,
    };

    Grant grant = Grant::ClientCredentials;
    Poco::URI url;
    /// Send parameters in the query string rather than in the form body. Only ever set for
    /// `ClientCredentials`, to preserve the transport of `oauth_server_use_request_body = 0`;
    /// an exchange must never put the user's JWT in a request line.
    bool use_query_parameters = false;
    String scope;
    String client_id;
    String client_secret;
    /// `TokenExchange` only.
    String subject_token;
    String subject_token_type;
    String requested_token_type;
    String actor_token;
    String actor_token_type;
};

/// Key of the vended-credentials cache. `principal` comes first so that one user's entries are
/// contiguous, and is the empty string on the non-forwarding path -- which reproduces the
/// pre-forwarding `(namespace, table)` key semantics exactly. Without `principal` in the key a
/// warm cache would hand Bob the STS credentials the catalog vended for Alice, without the
/// catalog ever being consulted.
struct CredentialsCacheKey
{
    std::string principal;
    std::string namespace_name;
    std::string table_name;

    auto operator<=>(const CredentialsCacheKey &) const = default;
};

class RestCatalog : public ICatalog, public DB::WithContext
{
public:
    explicit RestCatalog(
        const std::string & warehouse_,
        const std::string & base_url_,
        const std::string & catalog_credential_,
        const std::string & auth_scope_,
        const std::string & auth_header_,
        const std::string & oauth_server_uri_,
        bool oauth_server_use_request_body_,
        const std::string & namespaces_,
        DB::ContextPtr context_,
        const TokenForwardingConfig & token_forwarding_ = {});

    ~RestCatalog() override = default;

    bool empty(const DB::ForwardedAuthTokenPtr & auth_token) const override;

    DB::Names getTables(const DB::ForwardedAuthTokenPtr & auth_token) const override;

    bool existsTable(const std::string & namespace_name, const std::string & table_name, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    void getTableMetadata(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result) const override;

    bool tryGetTableMetadata(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result) const override;

    std::optional<StorageType> getStorageType() const override;

    DB::DatabaseDataLakeCatalogType getCatalogType() const override
    {
        return DB::DatabaseDataLakeCatalogType::ICEBERG_REST;
    }

    void createTable(const String & namespace_name, const String & table_name, const String & new_metadata_path, Poco::JSON::Object::Ptr metadata_content, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    bool updateMetadata(const String & namespace_name, const String & table_name, const String & new_metadata_path, Poco::JSON::Object::Ptr new_snapshot, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    bool updateSchema(
        const String & namespace_name,
        const String & table_name,
        const String & new_metadata_path,
        Poco::JSON::Object::Ptr new_schema,
        Int32 previous_schema_id,
        Int32 new_last_column_id,
        Poco::JSON::Object::Ptr metadata,
        const DB::ForwardedAuthTokenPtr & auth_token) const override;

    bool isTransactional() const override { return true; }

    void dropTable(const String & namespace_name, const String & table_name, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    ICatalog::CredentialsRefreshCallback getCredentialsConfigurationCallback(
        const DB::StorageID & storage_id, const DB::ForwardedAuthTokenPtr & auth_token) override;

    bool supportsUserTokenForwarding() const override { return true; }

    /// `loadConfig` runs from the constructor, which has no user. Under passthrough there may be
    /// no service credential either, so the catalog is built lazily on the first *user* query and
    /// that user's token initializes it. See `DatabaseDataLake::getCatalog`.
    void loadConfigIfNeeded(const DB::ForwardedAuthTokenPtr & auth_token) const;

    void setVendedCredentialsCacheTTL(std::chrono::seconds ttl) override { vended_credentials_cache_ttl.store(ttl, std::memory_order_relaxed); }

    struct Config
    {
        /// Prefix is a path of the catalog endpoint,
        /// e.g. /v1/{prefix}/namespaces/{namespace}/tables/{table}
        std::filesystem::path prefix;
        /// Base location is location of data in storage
        /// (in filesystem or object storage).
        std::string default_base_location;

        std::string toString() const;
    };

    /// Credentials together with the catalog configuration they resolve to
    /// (the /v1/config response depends on the credentials), published as one
    /// atomic snapshot so readers never see a torn combination of them.
    struct CatalogState
    {
        std::optional<DB::HTTPHeaderEntry> auth_header;
        std::string client_id;
        std::string client_secret;
        std::string tenant_id;
        std::string bearer_token;
        Config config;
        /// `/v1/config` is fetched from the constructor for every catalog flavour except a
        /// forwarding one, which has no user (and possibly no service credential) there and
        /// fetches it on the first user query instead -- see `loadConfigIfNeeded`.
        bool config_loaded = false;
    };
    using CatalogStateVersion = MultiVersion<CatalogState>::Version;

    /// Everything a `getAuthHeaders` implementation may need about the request being authenticated.
    struct AuthContext
    {
        /// The snapshot the caller derived the endpoint from, so that one request never mixes
        /// the endpoint of one state version with the auth of another.
        const CatalogState & catalog_state;
        /// Force a fresh token instead of reusing the cached one. Under forwarding this re-runs
        /// the *user's* exchange, never a `client_credentials` grant -- see
        /// `RestCatalog::getAuthHeaders`.
        bool update_token = false;
        String method;
        Poco::URI url;
        DB::HTTPHeaderEntries extra_headers;
        String body;
        /// The token of the user on whose behalf this request is made, if any.
        DB::ForwardedAuthTokenPtr auth_token;
        /// Set to whether an already-cached OAuth token was reused, when not null. Only the caller
        /// knows whether the request it authenticates went on to succeed, so it, not
        /// `getAuthHeaders`, accounts for `DataLakeRestCatalogAuthTokenCachedValid`.
        bool * used_cached_oauth_token = nullptr;
    };

    CatalogStateVersion getStateSnapshot() const { return state.get(); }

    ICatalog::PreparedSettingsChangesPtr prepareSettingsChanges(const DB::SettingsChanges & changes) override;

    void commitSettingsChanges(ICatalog::PreparedSettingsChangesPtr prepared) override;

    /// Check that we actually support these settings alter
    static void validateSettingsChangesImpl(
        const DB::SettingsChanges & changes,
        const std::unordered_set<std::string> & alterable_settings,
        const std::string & auth_mode_description);

    /// `credential_mode` means the catalog authenticates with `catalog_credential`,
    /// `header_mode` with `auth_header`. The mode is fixed when the database is created.
    static void validateSettingsChanges(const DB::SettingsChanges & changes, bool credential_mode, bool header_mode);

protected:
    RestCatalog(
        const std::string & warehouse_,
        const std::string & base_url_,
        const std::string & auth_scope_,
        const std::string & oauth_server_uri_,
        bool oauth_server_use_request_body_,
        const std::string & namespaces_,
        DB::ContextPtr context_);

    void createNamespaceIfNotExists(const String & namespace_name, const String & location, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    const std::filesystem::path base_url;
    const LoggerPtr log;

    /// Mutable because a forwarding catalog publishes the lazily loaded `/v1/config` from the
    /// const query path.
    mutable MultiVersion<CatalogState> state{std::make_unique<const CatalogState>()};
    /// Serializes the lazy config load, so that the queries a single `SHOW TABLES` fans across
    /// the catalog thread pool issue one `GET /v1/config` between them.
    mutable std::mutex config_mutex;

    /// Parameters for OAuth (common for REST catalog).
    bool update_token_if_expired = false;
    std::string auth_scope;
    std::string oauth_server_uri;
    bool oauth_server_use_request_body;
    /// Strictly the service-principal / actor token. A per-user token must NEVER be stored here:
    /// this is one `MultiVersion` shared by every user of the database, so doing so would sign
    /// Bob's request with Alice's session.
    mutable MultiVersion<AccessToken> access_token;

    TokenForwardingConfig token_forwarding;

    /// Session tokens obtained by exchanging a user's token, keyed on the token fingerprint (not
    /// on the user name: the fingerprint changes on rotation, so a cached session cannot outlive
    /// the credential that produced it). Bounded rather than swept, because N concurrent users
    /// would otherwise grow it without limit, and `getOrSetWithOutcome` collapses the stampede a
    /// single `SHOW TABLES` fanned across the catalog thread pool would otherwise cause.
    /// Passthrough caches nothing -- the user's token arrives with every request.
    static constexpr size_t user_token_cache_max_entries = 1024;
    mutable DB::CacheBase<String, AccessToken> user_token_cache;

    /// TTL for caching vended credentials per table (0 means no caching).
    std::atomic<std::chrono::seconds> vended_credentials_cache_ttl{std::chrono::seconds::zero()};

    /// Sweep trigger threshold, not capacity!
    static constexpr size_t credentials_cache_cleanup_threshold = 1000;

    /// Hard capacity. The sweep above only triggers on expiry, which is not a bound: with
    /// per-user keys the cache is O(users x tables), so it needs a real cap. Eviction is by
    /// earliest `expires_at`.
    static constexpr size_t credentials_cache_max_entries = 10000;

    static constexpr std::chrono::seconds credentials_expiry_safety_window{60};
    mutable std::mutex credentials_cache_mutex;

    mutable std::map<CredentialsCacheKey, VendedStorageCredentials> credentials_cache
        TSA_GUARDED_BY(credentials_cache_mutex);

public:
    class AllowedNamespaces
    {
    public:
        AllowedNamespaces() {}
        explicit AllowedNamespaces(const std::string & namespaces_);

        /// Check if nested namespaces (nested=true) or tables (nested=false) are allowed in namespace
        bool isNamespaceAllowed(const std::string & namespace_, bool nested) const;

    private:
        /// List of allowed nested namespaces
        std::unordered_map<std::string, AllowedNamespaces> nested_namespaces;
        /// Tables from current level are allowed
        bool allow_tables = false;
    };

protected:
    AllowedNamespaces allowed_namespaces;

    Poco::Net::HTTPBasicCredentials credentials{};

    /// `catalog_state` is the snapshot the caller derived the endpoint from, so that one
    /// request never mixes the endpoint of one state version with the auth of another.
    DB::ReadWriteBufferFromHTTPPtr createReadBuffer(
        const CatalogState & catalog_state,
        const std::string & endpoint,
        const DB::ForwardedAuthTokenPtr & auth_token,
        const Poco::URI::QueryParameters & params = {},
        const DB::HTTPHeaderEntries & headers = {},
        const std::optional<DB::HTTPHeaderEntries> & auth_headers = std::nullopt) const;

    Poco::URI::QueryParameters createParentNamespaceParams(const std::string & base_namespace) const;

    using StopCondition = std::function<bool(const std::string & namespace_name)>;
    using ExecuteFunc = std::function<void(const std::string & namespace_name)>;

    void getNamespacesRecursive(
        const std::string & base_namespace,
        Namespaces & result,
        StopCondition stop_condition,
        ExecuteFunc func,
        const DB::ForwardedAuthTokenPtr & auth_token) const;

    Namespaces getNamespaces(const std::string & base_namespace, const DB::ForwardedAuthTokenPtr & auth_token) const;

    Namespaces parseNamespaces(DB::ReadBuffer & buf, const std::string & base_namespace, String & next_page_token) const;

    /// Named apart from the `getTables(auth_token)` override so that the two do not collide as
    /// overloads once both take a token.
    DB::Names getTablesInNamespace(const std::string & base_namespace, const DB::ForwardedAuthTokenPtr & auth_token, size_t limit = 0) const;

    DB::Names parseTables(DB::ReadBuffer & buf, const std::string & base_namespace, size_t limit, String & next_page_token) const;

    bool getTableMetadataImpl(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result,
        const DB::ForwardedAuthTokenPtr & auth_token,
        bool allow_credentials_cache = true) const;

    /// `tryGetTableMetadata` for callers that carry the token separately from the context
    /// (`existsTable`, which has no query context to take it from).
    bool tryGetTableMetadataImpl(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result,
        const DB::ForwardedAuthTokenPtr & auth_token) const;

    /// The token carried by a query context, or `{}` when there is none. Single point where the
    /// `ContextPtr`-taking methods reduce to the same internal representation as everything else.
    static DB::ForwardedAuthTokenPtr getForwardedAuthToken(const DB::ContextPtr & context_);

    /// Load catalog config (special http handler) utilizing information from catalog_state and auth_headers.
    Config loadConfig(
        const CatalogState & catalog_state,
        const DB::ForwardedAuthTokenPtr & auth_token,
        const std::optional<DB::HTTPHeaderEntries> & auth_headers = std::nullopt) const;

    virtual DB::HTTPHeaderEntries getAuthHeaders(const AuthContext & auth_context) const;

    /// The user's own token, or the session token obtained by exchanging it, depending on whether
    /// `oauth_token_exchange_uri` is set. Throws `CATALOG_USER_TOKEN_NOT_AVAILABLE` when forwarding
    /// is enabled and there is no token: never fall back to the service principal, which would
    /// turn an authorization failure into a query that succeeds under the wrong identity.
    String getForwardedToken(const CatalogState & catalog_state, const DB::ForwardedAuthTokenPtr & auth_token, bool update_token) const;

    /// Whether a failed catalog request should be retried once with a freshly minted token.
    bool shouldRetryWithFreshToken(Poco::Net::HTTPResponse::HTTPStatus status) const;

    void validateAuthHeaders(const DB::HTTPHeaderEntry & header) const;

    static void parseCatalogConfigurationSettings(const Poco::JSON::Object::Ptr & object, Config & result);

    void sendRequest(
        const CatalogState & catalog_state,
        const String & endpoint,
        Poco::JSON::Object::Ptr request_body,
        const DB::ForwardedAuthTokenPtr & auth_token,
        const String & method = Poco::Net::HTTPRequest::HTTP_POST,
        bool ignore_result = false) const;

    VendedStorageCredentials getCredentialsAndEndpoint(Poco::JSON::Object::Ptr object, const String & location) const;

    /// `""` when forwarding is off, which preserves the pre-forwarding cache-key semantics.
    String getCredentialsCachePrincipal(const DB::ForwardedAuthTokenPtr & auth_token) const;

    std::optional<VendedStorageCredentials> tryGetCachedCredentials(const CredentialsCacheKey & key) const;

    void cacheCredentials(const CredentialsCacheKey & key, const VendedStorageCredentials & parsed) const;

    /// Performs one OAuth token-endpoint request. Both grants share this so that the
    /// `client_credentials` path stays byte-identical to what it was before token exchange existed.
    AccessToken requestToken(const TokenRequest & request) const;

    /// RFC 8693 exchange of the user's token for a catalog session token, against
    /// `oauth_token_exchange_uri`.
    AccessToken exchangeUserToken(const CatalogState & catalog_state, const DB::ForwardedAuthToken & auth_token) const;

    AccessToken retrieveAccessToken(const std::string & client_id, const std::string & client_secret) const;

    struct PreparedAuthChanges;

    /// Hook for `prepareSettingsChanges`: validate `changes` and apply them to `new_state`,
    /// building the new auth artifacts, without publishing anything. When the OAuth
    /// credentials change, the eagerly fetched token goes into `new_access_token` and
    /// `new_auth_headers`, so that wrong credentials fail the ALTER right here and the
    /// config reload authenticates with the new token instead of the cached one.
    virtual void applySettingsChangesToState(
        const DB::SettingsChanges & changes,
        const CatalogState & old_state,
        CatalogState & new_state,
        std::optional<DB::HTTPHeaderEntries> & new_auth_headers,
        std::unique_ptr<AccessToken> & new_access_token);
};

class OneLakeCatalog : public RestCatalog
{
public:
    explicit OneLakeCatalog(
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
        DB::ContextPtr context_);

    DB::DatabaseDataLakeCatalogType getCatalogType() const override
    {
        return DB::DatabaseDataLakeCatalogType::ICEBERG_ONELAKE;
    }

    DB::HTTPHeaderEntries getAuthHeaders(const AuthContext & auth_context) const override;

    /// `bearer_mode` means the catalog authenticates with `onelake_bearer_token`,
    /// otherwise with the `onelake_client_id` + `onelake_client_secret` pair.
    /// The mode is fixed when the database is created.
    static void validateSettingsChanges(const DB::SettingsChanges & changes, bool bearer_mode);

protected:
    void applySettingsChangesToState(
        const DB::SettingsChanges & changes,
        const CatalogState & old_state,
        CatalogState & new_state,
        std::optional<DB::HTTPHeaderEntries> & new_auth_headers,
        std::unique_ptr<AccessToken> & new_access_token) override;
};

class BigLakeCatalog : public RestCatalog
{
public:
    explicit BigLakeCatalog(
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
        DB::ContextPtr context_);

    DB::DatabaseDataLakeCatalogType getCatalogType() const override
    {
        return DB::DatabaseDataLakeCatalogType::ICEBERG_BIGLAKE;
    }

    DB::HTTPHeaderEntries getAuthHeaders(const AuthContext & auth_context) const override;

    const std::string & getGoogleADCClientId() const { return google_adc_client_id; }
    const std::string & getGoogleADCClientSecret() const { return google_adc_client_secret; }
    const std::string & getGoogleADCRefreshToken() const { return google_adc_refresh_token; }

private:
    /// Parameters for Google Cloud OAuth2 (BigLake).
    const std::string google_project_id;
    const std::string google_service_account;
    const std::string google_metadata_service;
    const std::string google_adc_client_id;
    const std::string google_adc_client_secret;
    const std::string google_adc_refresh_token;
    const std::string google_adc_quota_project_id;

    AccessToken retrieveGoogleCloudAccessToken() const;
    AccessToken retrieveGoogleCloudAccessTokenFromRefreshToken() const;
};

/// Builds the JSON body for a schema-update commit via the Iceberg REST catalog.
/// Includes an assert-current-schema-id requirement (when previous_schema_id >= 0),
/// schema deduplication against existing schemas in metadata, and last-column-id
/// propagation when adding a new schema.
Poco::JSON::Object::Ptr buildUpdateSchemaRequestBody(
    const String & namespace_name,
    const String & table_name,
    Poco::JSON::Object::Ptr metadata,
    Poco::JSON::Object::Ptr new_schema,
    Int32 previous_schema_id,
    Int32 new_last_column_id);

Poco::JSON::Object::Ptr buildUpdateMetadataRequestBody(
    const String & namespace_name,
    const String & table_name,
    Poco::JSON::Object::Ptr new_snapshot);

}

#endif
