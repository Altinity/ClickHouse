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
/// The presence of `token_exchange_uri` is the mode: empty means passthrough (the user's bearer
/// token is presented unchanged), non-empty means an RFC 8693 exchange against that URL.
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

/// One OAuth token-endpoint request: a service principal `client_credentials` grant, or an
/// RFC 8693 token exchange.
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
    /// `ClientCredentials` (`oauth_server_use_request_body = 0`); an exchange must never put the
    /// user's JWT in a request line.
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

/// Key of the vended-credentials cache. `generation` makes entries derived from superseded
/// catalog credentials unreachable the moment the generation moves on; `principal` keeps one
/// user's credentials from being served to another, and is empty when forwarding is off.
struct CredentialsCacheKey
{
    UInt64 generation = 0;
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

    void onTokenForwardingDisabled() const override { user_token_cache.clear(); }

    /// A forwarding catalog cannot fetch `/v1/config` from the constructor, which has no user and
    /// possibly no service credential, so the first user query loads it instead.
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
        /// See `loadConfigIfNeeded`.
        bool config_loaded = false;
    };
    using CatalogStateVersion = MultiVersion<CatalogState>::Version;

    /// Everything a `getAuthHeaders` implementation may need about the request being authenticated.
    struct AuthContext
    {
        /// The snapshot the caller derived the endpoint from, so that one request never mixes
        /// the endpoint of one state version with the auth of another.
        const CatalogState & catalog_state;
        /// The auth generation `catalog_state` was taken in -- see `StateSnapshot`.
        UInt64 generation = 0;
        /// Force a fresh token instead of reusing the cached one. Under forwarding this re-runs
        /// the user's exchange, never a `client_credentials` grant.
        bool update_token = false;
        String method;
        Poco::URI url;
        DB::HTTPHeaderEntries extra_headers;
        String body;
        /// The token of the user on whose behalf this request is made, if any.
        DB::ForwardedAuthTokenPtr auth_token;
        /// Set to whether an already-cached OAuth token was reused, when not null. The caller
        /// accounts for `DataLakeRestCatalogAuthTokenCachedValid`, because only it knows whether
        /// the request went on to succeed.
        bool * used_cached_oauth_token = nullptr;
    };

    /// A `CatalogState` snapshot paired with the auth generation in force when it was taken.
    /// Everything a request derives from the snapshot is tagged with that generation and becomes
    /// unreachable once `commitSettingsChanges` moves the generation on.
    struct StateSnapshot
    {
        UInt64 generation = 0;
        CatalogStateVersion state;

        const CatalogState & operator*() const { return *state; }
        const CatalogState * operator->() const { return state.get(); }
    };

    /// Reads the generation before the state, never after: the reverse order could pair a new
    /// generation with an old state and let superseded credentials cache a result as current.
    StateSnapshot getStateSnapshot() const
    {
        const UInt64 generation = auth_generation.load(std::memory_order_acquire);
        return StateSnapshot{generation, state.get()};
    }

    ICatalog::PreparedSettingsChangesPtr prepareSettingsChanges(
        const DB::SettingsChanges & changes, const DB::ForwardedAuthTokenPtr & auth_token = {}) override;

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
    /// Serializes the lazy config load, so that queries fanned across the catalog thread pool
    /// issue one `GET /v1/config` between them.
    mutable std::mutex config_mutex;

    /// Parameters for OAuth (common for REST catalog).
    bool update_token_if_expired = false;
    std::string auth_scope;
    std::string oauth_server_uri;
    bool oauth_server_use_request_body;
    /// Strictly the service-principal / actor token. Shared by every user of the database, so a
    /// per-user token must never be stored here.
    mutable MultiVersion<AccessToken> access_token;

    TokenForwardingConfig token_forwarding;

    /// Session tokens obtained by exchanging a user's token, keyed on the token fingerprint so
    /// that a cached session cannot outlive the credential that produced it. Bounded, because the
    /// number of concurrent users is not. Passthrough caches nothing.
    static constexpr size_t user_token_cache_max_entries = 1024;
    mutable DB::CacheBase<String, AccessToken> user_token_cache;

    /// Bumped by `commitSettingsChanges` once per auth change, so that a request which
    /// authenticated with since-rotated credentials cannot publish or cache its result as current.
    /// Not a field of `CatalogState`, which is republished for unrelated reasons.
    std::atomic<UInt64> auth_generation{0};

    /// Serializes publishing an auth artifact against `commitSettingsChanges` publishing a new
    /// one, so that the generation check and the publish it guards cannot be split by an ALTER.
    /// Never held across a network request.
    mutable std::mutex auth_publish_mutex;

    /// TTL for caching vended credentials per table (0 means no caching).
    std::atomic<std::chrono::seconds> vended_credentials_cache_ttl{std::chrono::seconds::zero()};

    /// Sweep trigger threshold, not capacity!
    static constexpr size_t credentials_cache_cleanup_threshold = 1000;

    /// Hard capacity: the sweep above only triggers on expiry, and with per-user keys the cache
    /// is O(users x tables). Eviction is by earliest `expires_at`.
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
        UInt64 generation,
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

    /// Named apart from the `getTables(auth_token)` override, which it would otherwise overload.
    DB::Names getTablesInNamespace(const std::string & base_namespace, const DB::ForwardedAuthTokenPtr & auth_token, size_t limit = 0) const;

    DB::Names parseTables(DB::ReadBuffer & buf, const std::string & base_namespace, size_t limit, String & next_page_token) const;

    bool getTableMetadataImpl(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result,
        const DB::ForwardedAuthTokenPtr & auth_token,
        bool allow_credentials_cache = true) const;

    /// `tryGetTableMetadata` for callers that carry the token separately from the context.
    bool tryGetTableMetadataImpl(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result,
        const DB::ForwardedAuthTokenPtr & auth_token) const;

    /// Load catalog config (special http handler) utilizing information from catalog_state and auth_headers.
    Config loadConfig(
        const CatalogState & catalog_state,
        UInt64 generation,
        const DB::ForwardedAuthTokenPtr & auth_token,
        const std::optional<DB::HTTPHeaderEntries> & auth_headers = std::nullopt) const;

    virtual DB::HTTPHeaderEntries getAuthHeaders(const AuthContext & auth_context) const;

    void validateForwardedToken(const DB::ForwardedAuthTokenPtr & auth_token) const;

    /// The user's own token, or the session token obtained by exchanging it, depending on whether
    /// `oauth_token_exchange_uri` is set. Throws `CATALOG_USER_TOKEN_NOT_AVAILABLE` when there is
    /// no token, or when `enable_token_forwarding` has since been turned off; never falls back to
    /// the service principal.
    String getForwardedToken(
        const CatalogState & catalog_state, UInt64 generation, const DB::ForwardedAuthTokenPtr & auth_token, bool update_token) const;

    /// Whether a failed catalog request should be retried once with a freshly minted token.
    bool shouldRetryWithFreshToken(Poco::Net::HTTPResponse::HTTPStatus status) const;

    void validateAuthHeaders(const DB::HTTPHeaderEntry & header) const;

    static void parseCatalogConfigurationSettings(const Poco::JSON::Object::Ptr & object, Config & result);

    void sendRequest(
        const CatalogState & catalog_state,
        UInt64 generation,
        const String & endpoint,
        Poco::JSON::Object::Ptr request_body,
        const DB::ForwardedAuthTokenPtr & auth_token,
        const String & method = Poco::Net::HTTPRequest::HTTP_POST,
        bool ignore_result = false) const;

    VendedStorageCredentials getCredentialsAndEndpoint(Poco::JSON::Object::Ptr object, const String & location) const;

    /// Empty when forwarding is off.
    String getCredentialsCachePrincipal(const DB::ForwardedAuthTokenPtr & auth_token) const;

    std::optional<VendedStorageCredentials> tryGetCachedCredentials(const CredentialsCacheKey & key) const;

    void cacheCredentials(const CredentialsCacheKey & key, const VendedStorageCredentials & parsed) const;

    /// Publishes a freshly minted service-principal token into `access_token`, but only if the
    /// credentials it was minted with are still in force. Returns it either way.
    MultiVersion<AccessToken>::Version publishServiceToken(AccessToken minted, UInt64 generation) const;

    /// Performs one OAuth token-endpoint request, for either grant.
    AccessToken requestToken(const TokenRequest & request) const;

    /// RFC 8693 exchange of the user's token for a catalog session token, against
    /// `oauth_token_exchange_uri`.
    AccessToken exchangeUserToken(
        const CatalogState & catalog_state, UInt64 generation, const DB::ForwardedAuthToken & auth_token,
        const AccessToken * prepared_actor_token = nullptr) const;

    AccessToken retrieveAccessToken(const std::string & client_id, const std::string & client_secret) const;

    /// The catalog service principal's own token, minted with a `client_credentials` grant and
    /// cached in `access_token`. While forwarding is on it is only ever the RFC 8693 `actor_token`.
    String getServicePrincipalToken(const CatalogState & catalog_state, UInt64 generation) const;

    struct PreparedAuthChanges;

    /// Hook for `prepareSettingsChanges`: validate `changes` and apply them to `new_state`,
    /// building the new auth artifacts, without publishing anything. When the OAuth credentials
    /// change, a service token is fetched only for service authentication or delegation.
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
