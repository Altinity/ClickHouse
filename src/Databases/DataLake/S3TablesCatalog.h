#pragma once

#include "config.h"

#if USE_AVRO && USE_SSL && USE_AWS_S3

#include <Databases/DataLake/RestCatalog.h>
#include <IO/S3/Credentials.h>

#include <aws/core/auth/signer/AWSAuthV4Signer.h>

#include <memory>

namespace Aws::Auth
{
class AWSCredentialsProvider;
}

namespace DataLake
{

/// Iceberg REST catalog for Amazon S3 Tables (SigV4, signing name `s3tables`).
/// https://docs.aws.amazon.com/AmazonS3/latest/userguide/s3-tables-integrating-open-source.html
class S3TablesCatalog final : public RestCatalog
{
public:
    S3TablesCatalog(
        const String & warehouse_,
        const String & base_url_,
        const String & region_,
        const DataLake::CatalogSettings & catalog_settings_,
        const String & namespaces_,
        DB::ContextPtr context_);

    DB::DatabaseDataLakeCatalogType getCatalogType() const override { return DB::DatabaseDataLakeCatalogType::S3_TABLES; }

    DB::ReadWriteBufferFromHTTPPtr createReadBuffer(
        const std::string & endpoint,
        const Poco::URI::QueryParameters & params = {},
        const DB::HTTPHeaderEntries & headers = {}) const override;

    void sendRequest(
        const String & endpoint,
        Poco::JSON::Object::Ptr request_body,
        const String & method = Poco::Net::HTTPRequest::HTTP_POST,
        bool ignore_result = false) const override;

protected:
    DB::HTTPHeaderEntries getAuthHeaders(bool /* update_token */) const override;

private:
    const String region;
    const String signing_service;
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials_provider;
    std::unique_ptr<Aws::Client::AWSAuthV4Signer> signer;
};

}

#endif
