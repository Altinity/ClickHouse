#include "config.h"

#if USE_AVRO && USE_SSL && USE_AWS_S3

#include <Databases/DataLake/S3TablesCatalog.h>
#include <Databases/DataLake/AWSV4Signer.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/setThreadName.h>
#include <Common/threadPoolCallbackRunner.h>
#include <Core/ServerSettings.h>
#include <Core/Settings.h>
#include <Interpreters/Context.h>
#include <IO/ConnectionTimeouts.h>
#include <IO/ReadWriteBufferFromHTTP.h>
#include <IO/S3/Client.h>
#include <IO/ReadHelpers.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergWrites.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/URI.h>

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/signer/AWSAuthV4Signer.h>

#include <mutex>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

namespace DB::Setting
{
    extern const SettingsUInt64 s3_max_connections;
    extern const SettingsUInt64 s3_max_redirects;
    extern const SettingsUInt64 s3_retry_attempts;
    extern const SettingsBool s3_slow_all_threads_after_network_error;
    extern const SettingsBool enable_s3_requests_logging;
    extern const SettingsUInt64 s3_connect_timeout_ms;
    extern const SettingsUInt64 s3_request_timeout_ms;
}

namespace DB::ServerSetting
{
    extern const ServerSettingsUInt64 s3_max_redirects;
    extern const ServerSettingsUInt64 s3_retry_attempts;
}

namespace DataLake
{

S3TablesCatalog::S3TablesCatalog(
    const String & warehouse_,
    const String & base_url_,
    const String & region_,
    const CatalogSettings & catalog_settings_,
    const String & namespaces_,
    DB::ContextPtr context_)
    : RestCatalog(warehouse_, base_url_, "", "", false, namespaces_, context_)
    , region(region_)
    , signing_service("s3tables")
{
    if (region.empty())
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "S3 Tables catalog requires non-empty `region` setting");

    DB::S3::CredentialsConfiguration creds_config;
    creds_config.use_environment_credentials = true;
    creds_config.role_arn = catalog_settings_.aws_role_arn;
    creds_config.role_session_name = catalog_settings_.aws_role_session_name;

    const auto & server_settings = getContext()->getGlobalContext()->getServerSettings();
    const DB::Settings & global_settings = getContext()->getGlobalContext()->getSettingsRef();

    int s3_max_redirects = static_cast<int>(server_settings[DB::ServerSetting::s3_max_redirects]);
    if (global_settings.isChanged("s3_max_redirects"))
        s3_max_redirects = static_cast<int>(global_settings[DB::Setting::s3_max_redirects]);

    int s3_retry_attempts = static_cast<int>(server_settings[DB::ServerSetting::s3_retry_attempts]);
    if (global_settings.isChanged("s3_retry_attempts"))
        s3_retry_attempts = static_cast<int>(global_settings[DB::Setting::s3_retry_attempts]);

    bool s3_slow_all_threads_after_network_error = global_settings[DB::Setting::s3_slow_all_threads_after_network_error];
    bool s3_slow_all_threads_after_retryable_error = false;
    bool enable_s3_requests_logging = global_settings[DB::Setting::enable_s3_requests_logging];

    DB::S3::PocoHTTPClientConfiguration poco_config = DB::S3::ClientFactory::instance().createClientConfiguration(
        region,
        getContext()->getRemoteHostFilter(),
        s3_max_redirects,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = static_cast<unsigned>(s3_retry_attempts)},
        s3_slow_all_threads_after_network_error,
        s3_slow_all_threads_after_retryable_error,
        enable_s3_requests_logging,
        /* for_disk_s3 = */ false,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {});

    Aws::Auth::AWSCredentials credentials(catalog_settings_.aws_access_key_id, catalog_settings_.aws_secret_access_key);
    credentials_provider = DB::S3::getCredentialsProvider(poco_config, credentials, creds_config);

    signer = std::make_unique<Aws::Client::AWSAuthV4Signer>(
        credentials_provider,
        "s3tables",
        Aws::String(region.data(), region.size()),
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Always,
        /* urlEscapePath = */ false);

    config = loadConfig();

    if (config.prefix.empty())
    {
        String encoded_warehouse;
        Poco::URI::encode(warehouse_, "", encoded_warehouse);
        config.prefix = encoded_warehouse;
    }
}

DB::Names S3TablesCatalog::getTables() const
{
    auto namespaces = getNamespaces("");

    auto & pool = getContext()->getIcebergCatalogThreadpool();
    DB::ThreadPoolCallbackRunnerLocal<void> runner(pool, DB::ThreadName::DATALAKE_REST_CATALOG);

    DB::Names tables;
    std::mutex mutex;
    for (const auto & ns : namespaces)
    {
        if (!allowed_namespaces.isNamespaceAllowed(ns, /*nested*/ false))
            continue;
        runner.enqueueAndKeepTrack(
            [&, ns]
            {
                auto tables_in_ns = RestCatalog::getTables(ns);
                std::lock_guard lock(mutex);
                std::move(tables_in_ns.begin(), tables_in_ns.end(), std::back_inserter(tables));
            });
    }
    runner.waitForAllToFinishAndRethrowFirstError();
    return tables;
}

bool S3TablesCatalog::tryGetTableMetadata(
    const std::string & namespace_name,
    const std::string & table_name,
    DB::ContextPtr context_,
    TableMetadata & result) const
{
    if (!RestCatalog::tryGetTableMetadata(namespace_name, table_name, context_, result))
        return false;

    if (!result.requiresCredentials())
        return true;

    bool need_credentials = !result.hasStorageCredentials() || !result.getStorageCredentials();
    if (!need_credentials)
    {
        auto creds = std::dynamic_pointer_cast<S3Credentials>(result.getStorageCredentials());
        if (creds && creds->isEmpty())
            need_credentials = true;
    }

    if (need_credentials)
    {
        LOG_DEBUG(log, "S3 Tables: no vended credentials for {}.{}, injecting catalog IAM credentials", namespace_name, table_name);
        auto aws_creds = credentials_provider->GetAWSCredentials();
        result.setStorageCredentials(std::make_shared<S3Credentials>(
            String(aws_creds.GetAWSAccessKeyId().c_str(), aws_creds.GetAWSAccessKeyId().size()),
            String(aws_creds.GetAWSSecretKey().c_str(), aws_creds.GetAWSSecretKey().size()),
            String(aws_creds.GetSessionToken().c_str(), aws_creds.GetSessionToken().size())));
    }

    if (result.getEndpoint().empty())
    {
        String regional_endpoint = "https://s3." + region + ".amazonaws.com";
        LOG_DEBUG(log, "S3 Tables: no s3.endpoint for {}.{}, injecting regional endpoint: {}", namespace_name, table_name, regional_endpoint);
        result.setEndpoint(regional_endpoint);
    }

    if (result.hasDataLakeSpecificProperties())
    {
        auto props = result.getDataLakeSpecificProperties();
        if (props.has_value() && !props->iceberg_metadata_file_location.empty())
        {
            const String & loc = props->iceberg_metadata_file_location;
            auto scheme_end = loc.find("://");
            if (scheme_end != String::npos)
            {
                auto path_start = loc.find('/', scheme_end + 3);
                if (path_start != String::npos)
                    props->iceberg_metadata_file_location = loc.substr(path_start + 1);
            }
            result.setDataLakeSpecificProperties(std::move(props));
        }
    }

    return true;
}

DB::HTTPHeaderEntries S3TablesCatalog::getAuthHeaders(bool /* update_token */) const
{
    return {};
}

DB::ReadWriteBufferFromHTTPPtr S3TablesCatalog::createReadBuffer(
    const std::string & endpoint,
    const Poco::URI::QueryParameters & params,
    const DB::HTTPHeaderEntries & headers) const
{
    const auto & context = getContext();

    Poco::URI url(base_url / endpoint, /* enable_url_encoding */ false);
    if (!params.empty())
        url.setQueryParameters(params);

    auto create_buffer = [&]
    {
        DB::HTTPHeaderEntries signed_headers;
        signRequestWithAWSV4(Poco::Net::HTTPRequest::HTTP_GET, url, headers, "", *signer, region, signing_service, signed_headers);

        return DB::BuilderRWBufferFromHTTP(url)
            .withConnectionGroup(DB::HTTPConnectionGroupType::HTTP)
            .withSettings(getContext()->getReadSettings())
            .withTimeouts(DB::ConnectionTimeouts::getHTTPTimeouts(context->getSettingsRef(), context->getServerSettings()))
            .withHostFilter(&getContext()->getRemoteHostFilter())
            .withHeaders(signed_headers)
            .withDelayInit(false)
            .withSkipNotFound(false)
            .create(credentials);
    };

    LOG_DEBUG(log, "Requesting: {}", url.toString());

    try
    {
        return create_buffer();
    }
    catch (const DB::HTTPException & e)
    {
        const auto status = e.getHTTPStatus();
        if (status == Poco::Net::HTTPResponse::HTTPStatus::HTTP_UNAUTHORIZED
            || status == Poco::Net::HTTPResponse::HTTPStatus::HTTP_FORBIDDEN)
        {
            return create_buffer();
        }
        throw;
    }
}

void S3TablesCatalog::sendRequest(
    const String & endpoint,
    Poco::JSON::Object::Ptr request_body,
    const String & method,
    bool ignore_result) const
{
    std::ostringstream oss; // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    if (request_body)
        request_body->stringify(oss);
    const std::string body_str = DB::removeEscapedSlashes(oss.str());

    DB::HTTPHeaderEntries extra_headers;
    if (!body_str.empty())
        extra_headers.emplace_back("Content-Type", "application/json");

    const auto & context = getContext();

    Poco::URI url(endpoint, /* enable_url_encoding */ false);

    DB::HTTPHeaderEntries signed_headers;
    signRequestWithAWSV4(method, url, extra_headers, body_str, *signer, region, signing_service, signed_headers);

    DB::ReadWriteBufferFromHTTP::OutStreamCallback out_stream_callback;
    if (!body_str.empty())
    {
        out_stream_callback = [body_str](std::ostream & os) { os << body_str; };
    }

    auto wb = DB::BuilderRWBufferFromHTTP(url)
                  .withConnectionGroup(DB::HTTPConnectionGroupType::HTTP)
                  .withMethod(method)
                  .withSettings(context->getReadSettings())
                  .withTimeouts(DB::ConnectionTimeouts::getHTTPTimeouts(context->getSettingsRef(), context->getServerSettings()))
                  .withHostFilter(&context->getRemoteHostFilter())
                  .withHeaders(signed_headers)
                  .withOutCallback(out_stream_callback)
                  .withSkipNotFound(false)
                  .create(credentials);

    String response_str;
    if (!ignore_result)
        readJSONObjectPossiblyInvalid(response_str, *wb);
    else
        wb->ignoreAll();
}

}

#endif
