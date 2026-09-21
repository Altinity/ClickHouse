---
description: 'The DataLakeCatalog database engine enables you to connect ClickHouse to external data catalogs and query open table format data'
sidebar_label: 'DataLakeCatalog'
slug: /engines/database-engines/datalakecatalog
title: 'DataLakeCatalog'
doc_type: 'reference'
---

The `DataLakeCatalog` database engine enables you to connect ClickHouse to external
data catalogs and query open table format data without the need for data duplication.
This transforms ClickHouse into a powerful query engine that works seamlessly with
your existing data lake infrastructure.

## Supported catalogs {#supported-catalogs}

The `DataLakeCatalog` engine supports the following data catalogs:

- **AWS Glue Catalog** - For Iceberg tables in AWS environments
- **Databricks Unity Catalog** - For Delta Lake and Iceberg tables
- **Hive Metastore** - Traditional Hadoop ecosystem catalog
- **REST Catalogs** - Any catalog supporting the Iceberg REST specification

## Creating a database {#creating-a-database}

You will need to enable the relevant settings below to use the `DataLakeCatalog` engine:

```sql
SET allow_experimental_database_iceberg = 1;
SET allow_experimental_database_unity_catalog = 1;
SET allow_experimental_database_glue_catalog = 1;
SET allow_experimental_database_hms_catalog = 1;
SET allow_experimental_database_paimon_rest_catalog = 1;
```

Databases with the `DataLakeCatalog` engine can be created using the following syntax:

```sql
CREATE DATABASE database_name
ENGINE = DataLakeCatalog(catalog_endpoint[, user, password])
SETTINGS
catalog_type,
[...]
```

The following settings are supported:

| Setting                 | Description                                                                                   |
|-------------------------|-----------------------------------------------------------------------------------------------|
| `catalog_type`          | Type of catalog: `glue`, `unity` (Delta), `rest` (Iceberg), `hive`, `onelake` (Iceberg)       |
| `warehouse`             | The warehouse/database name to use in the catalog.                                            |
| `catalog_credential`    | Authentication credential for the catalog (e.g., API key or token)                            |
| `auth_header`           | Custom HTTP header for authentication with the catalog service                                |
| `auth_scope`            | OAuth2 scope for authentication (if using OAuth)                                              |
| `storage_endpoint`      | Endpoint URL for the underlying storage                                                       |
| `oauth_server_uri`      | URI of the OAuth2 authorization server for authentication                                     |
| `vended_credentials`    | Boolean indicating whether to use vended credentials from the catalog (supports AWS S3 and Azure ADLS Gen2) |
| `vended_credentials_cache_ttl` | Maximum cache entry lifetime (in seconds) for vended credentials (REST catalogs only). Default `300`; `0` disables caching. |
| `aws_access_key_id`     | AWS access key ID for S3/Glue access (if not using vended credentials)                        |
| `aws_secret_access_key` | AWS secret access key for S3/Glue access (if not using vended credentials)                    |
| `region`                | AWS region for the service (e.g., `us-east-1`)                                                |
| `dlf_access_key_id`     | Access key ID for DLF access                                                                  |
| `dlf_access_key_secret` | Access key Secret for DLF access                                                              |
| `namespaces`            | Comma-separated list of namespaces, implemented for catalog types: `rest`, `glue` and `unity` |
| `oauth_forward_user_token` | Authenticate to the catalog as the user running the query instead of as the shared service identity. Iceberg REST and Glue only. See [Forwarding the user's identity to the catalog](#user-token-forwarding) |
| `oauth_token_exchange_uri` | Empty (the default) forwards the user's token unchanged; non-empty performs an RFC 8693 token exchange at this URL first |
| `oauth_subject_token_type` | RFC 8693 `subject_token_type` of the forwarded token. Default `urn:ietf:params:oauth:token-type:access_token` |
| `oauth_requested_token_type` | RFC 8693 `requested_token_type`; empty omits the field. Default `urn:ietf:params:oauth:token-type:access_token` |
| `oauth_forward_actor_token` | Send the service principal's own token as the RFC 8693 `actor_token`. Default `0`. See [Delegation with an actor token](#user-token-forwarding-actor-token) |
| `oauth_user_token_cache_ttl` | Maximum lifetime (in seconds) of a cached exchanged session token; `0` disables caching. Default `300` |

## Examples {#examples}

See below sections for examples of using the `DataLakeCatalog` engine:

* [Unity Catalog](/use-cases/data-lake/unity-catalog)
* [Glue Catalog](/use-cases/data-lake/glue-catalog)
* OneLake Catalog
    Can be used by enabling `allow_experimental_database_iceberg` or `allow_database_iceberg`.
```sql
CREATE DATABASE database_name
ENGINE = DataLakeCatalog(catalog_endpoint)
SETTINGS
    catalog_type = 'onelake',
    warehouse = warehouse,
    onelake_tenant_id = tenant_id,
    oauth_server_uri = server_uri,
    auth_scope = auth_scope,
    onelake_client_id = client_id,
    onelake_client_secret = client_secret;
SHOW TABLES IN database_name;
SELECT count() from database_name.table_name;
```
To authenticate without sharing a client secret, set `onelake_bearer_token` to a pre-obtained bearer token (scoped to `https://storage.azure.com`) instead of `onelake_client_id`/`onelake_client_secret`. ClickHouse does not refresh the token, so the database must be recreated after it expires.

## Forwarding the user's identity to the catalog {#user-token-forwarding}

Set `oauth_forward_user_token = 1` to use the querying user's token for catalog authentication.
This requires:

- the server-level [`enable_token_forwarding`](/operations/server-configuration-parameters/settings#enable_token_forwarding)
  setting, which is `false` by default;
- `catalog_type = 'rest'` or `catalog_type = 'glue'`;
- token authentication through an `Authorization: Bearer` HTTP header or `--jwt` for the native
  protocol. See [Token-based authentication](/en/operations/external-authenticators/oauth).

:::danger Restrict `CREATE DATABASE`
Database creators choose the endpoints that receive users' tokens. Grant `CREATE DATABASE` only
to trusted users and restrict `remote_url_allow_hosts`, which applies to catalog and token-exchange
endpoints.
:::

### Passthrough: the default {#user-token-forwarding-passthrough}

For Iceberg REST catalogs, `oauth_forward_user_token = 1` forwards the user's bearer token
unchanged. No token endpoint or client credentials are required:

```sql
CREATE DATABASE demo
ENGINE = DataLakeCatalog('http://lakekeeper:8181/catalog')
SETTINGS
    catalog_type = 'rest',
    warehouse = 'demo',
    oauth_forward_user_token = 1;
```

The token's audience must cover both ClickHouse and the catalog. With Keycloak, add an audience
mapper to the ClickHouse client to include the catalog's audience.

### Token exchange: opt-in {#user-token-forwarding-exchange}

Set `oauth_token_exchange_uri` to exchange the user's token using
[RFC 8693](https://www.rfc-editor.org/rfc/rfc8693) before presenting it to the REST catalog.
Use an IdP token endpoint that issues tokens with an audience the catalog accepts:

```sql
CREATE DATABASE demo
ENGINE = DataLakeCatalog('http://lakekeeper:8181/catalog')
SETTINGS
    catalog_type = 'rest',
    warehouse = 'demo',
    catalog_credential = 'clickhouse:<client-secret>',
    auth_scope = 'lakekeeper',
    oauth_forward_user_token = 1,
    oauth_token_exchange_uri = 'http://keycloak:8080/realms/demo/protocol/openid-connect/token';
```

The exchange requires `catalog_credential`; its `client_id` and `client_secret` are sent in the
form body. `auth_scope` supplies the exchange `scope`. Override its Polaris-specific default,
`PRINCIPAL_ROLE:ALL`, for other providers.

A catalog's `/v1/oauth/tokens` endpoint can also be used if supported. The Iceberg REST
specification deprecates this endpoint, and some catalogs, including Lakekeeper, do not implement it.

### Delegation with an actor token {#user-token-forwarding-actor-token}

Set `oauth_forward_actor_token = 1` to include the service principal's token as the RFC 8693
`actor_token`. This requires `oauth_token_exchange_uri` and an endpoint that supports delegation
and can validate the actor token.

ClickHouse obtains the actor token through a `client_credentials` grant using `catalog_credential`
at `oauth_server_uri`, or the catalog's `/v1/oauth/tokens` endpoint if that setting is empty.
The token is cached until expiry and used only for exchanges. If obtaining it fails, the query fails.

### Glue {#user-token-forwarding-glue}

For Glue, ClickHouse exchanges the user's token through AWS STS `AssumeRoleWithWebIdentity`
for temporary credentials of `aws_role_arn`. These credentials sign Glue and S3 requests using
SigV4. The ClickHouse user name supplies `RoleSessionName` for CloudTrail auditing.

```sql
CREATE DATABASE glue_db
ENGINE = DataLakeCatalog
SETTINGS
    catalog_type = 'glue',
    region = 'us-east-1',
    aws_role_arn = 'arn:aws:iam::123456789012:role/data-lake-reader',
    oauth_forward_user_token = 1;
```

Register the token issuer as an IAM OIDC identity provider and configure the role's trust policy
to accept the users' tokens, including their `aud` and `sub` claims.

- `aws_role_arn` is required.
- `aws_access_key_id`, `aws_secret_access_key`, and RFC 8693 exchange settings are rejected.
- The AWS STS endpoint is determined by `region`.
- Users receive the assumed role's permissions. Use separate roles or session-tag policies to
  distinguish access. ClickHouse does not implement IAM Identity Center trusted identity propagation.

### Scope and limitations {#user-token-forwarding-scope}

- Forwarding covers catalog listings, table metadata, and write operations (`INSERT`, `ALTER`,
  mutations, `DROP TABLE`, and snapshot expiry).
- Vended storage credentials and Glue sessions are cached separately for each user token.
- Requests without a user token fail with `CATALOG_USER_TOKEN_NOT_AVAILABLE`; ClickHouse does not
  fall back to the service identity. Catalog listings may suppress these errors and appear empty,
  depending on `database_datalake_require_metadata_access`.
- With `object_storage_cluster`, workers receive table-scoped storage credentials over the
  interserver channel. Configure `interserver_https_port` or a cluster `<secret>` for cluster reads.
- For Iceberg REST, rotate `catalog_credential` with `ALTER DATABASE ... MODIFY SETTING`,
  authenticated with a user token. ClickHouse validates the new credentials and reloads the catalog
  configuration before applying the change, then invalidates cached session and storage credentials.
  Glue settings cannot be altered.

## Namespace filter {#namespace}

By default, ClickHouse reads tables from all namespaces available in the catalog. You can limit this behavior using the `namespaces` database setting. The value should be a comma‑separated list of namespaces that are allowed to be read.

Supported catalog types are `rest`, `glue` and `unity`.

For example, if the catalog contains three namespaces - `dev`, `stage`, and `prod` - and you want to read data only from dev and stage, set:
```
namespaces='dev,stage'
```

### Nested namespaces {#namespace-nested}

The Iceberg (`rest`) catalog supports nested namespaces. The `namespaces` filter accepts the following patterns:

- `namespace` - includes tables from the specified namespace, but not from its nested namespaces.
- `namespace.nested` - includes tables from the nested namespace, but not from the parent.
- `namespace.*` - includes tables from all nested namespaces, but not from the parent.

If you need to include both a namespace and its nested namespaces, specify both explicitly. For example:
```
namespaces='namespace,namespace.*'
```

The default value is '*', which means all namespaces are included.
