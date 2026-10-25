---
description: 'How to authenticate ClickHouse users with OAuth / OpenID Connect tokens and JWTs, use an Identity Provider as an external user directory, and log in from clickhouse-client.'
sidebarTitle: 'OAuth'
slug: /antalya/oauth
title: 'Token-based authentication'
doc_type: 'guide'
---

# Token-based authentication {#token-based-authentication}

ClickHouse can authenticate users with access tokens issued by an external Identity Provider (IdP). Clients pass the token with `clickhouse-client --jwt <token>` or the HTTP header `Authorization: Bearer <token>`.

A token can authenticate:

- a user defined in ClickHouse with the `jwt` authentication method — see [Users defined in ClickHouse](#enabling-jwt-auth);
- a user that is not defined in ClickHouse at all, when the IdP is configured as an [external user directory](#idp-external-user-directory). Such users are created on first login.

In both cases the token is validated by a **token processor** defined in `config.xml`.

## Glossary {#glossary}

- **Identity Provider (IdP)** — An external service that authenticates users and issues tokens: Keycloak, Okta, Microsoft Entra ID, Google, and others.
- **Access token** — A string the client presents to ClickHouse as proof of the user's identity. It is either a JWT or an opaque token, depending on the provider.
- **JWT (JSON Web Token)** — A signed token that carries data about the user. ClickHouse can verify it locally, without contacting the IdP.
- **Opaque token** — A token that is just a random string. Only the IdP can tell whether it is valid and whom it belongs to.
- **Claim** — A field in a JWT, or in the IdP's response about a token. Some of them are provider-specific, some are standard, e.g. `sub` is the user ID, `iss` (*issuer*) is the IdP that issued the token, `aud` (*audience*) is the application it was issued for and `exp` is its expiration time.
- **JWKS (JSON Web Key Set)** — The set of public keys that verify a JWT's signature, usually published by the IdP at a pre-defined URL.
- **Group** — A group (entity) the user belongs to at the IdP. ClickHouse can map groups to its own roles.
- **Token processor** — A bridge between ClickHouse and an IdP. It validates a token, either locally or by asking the IdP, and passes the user name and groups from the token to ClickHouse.

## Token processors {#token-processors}

Token processors are defined in the `token_processors` section of `config.xml`; each child element is one processor, and its name is the element name:

```xml
<clickhouse>
    <token_processors>
        <my_processor>
            <type>jwt_dynamic_jwks</type>
            <jwks_uri>https://idp.example.com/.well-known/jwks.json</jwks_uri>
        </my_processor>
    </token_processors>
</clickhouse>
```

Token authentication is enabled by default. Set `<enable_token_auth>0</enable_token_auth>` in `config.xml` to disable it system-wide: token processors are then not parsed, the `token` user directory is unavailable, and all token-based logins are rejected.

Parameters common to all token processors:

- `type` — Processor type, case-insensitive. Mandatory. One of:
  - `jwt_static_key`, `jwt_static_jwks`, `jwt_dynamic_jwks` — validate a JWT locally ([JWT processors](#jwt));
  - `entra` — Microsoft Entra ID ([Entra](#entra));
  - `google` — Google OAuth 2.0 access tokens ([Google](#google));
  - `openid` — any OpenID Connect provider, through its `/userinfo` and introspection endpoints ([OpenID](#openid)).
- `username_claim` — Claim used as the ClickHouse user name. Default: `sub`.
- `groups_claim` — Claim with the list of the user's groups, read from the JWT or from the `/userinfo` response. Default: `groups`.
- `token_cache_lifetime` — How long, in seconds, a validated token is cached to avoid repeated requests to the IdP. A cache entry never outlives the token's own expiration. Default: `3600`.

Parameters that do not apply to the configured `type` are ignored.

:::tip
If the IdP issues JWT access tokens ([RFC 9068](https://datatracker.ietf.org/doc/html/rfc9068)), use `jwt_dynamic_jwks`: tokens are verified locally, without a request to the IdP. `openid` is a good fit for opaque tokens, or for when you want the IdP itself to confirm each token.
:::

### JWT processors {#jwt}

The token is a JWT whose signature is verified locally with a static key, a static JWKS (JSON Web Key Set), or a JWKS downloaded from the IdP. Parameters shared by `jwt_static_key`, `jwt_static_jwks`, `jwt_dynamic_jwks` and `entra`, all optional:

- `expected_issuer` — Required value of the `iss` claim.
- `expected_audience` — Required value of the `aud` claim.
- `expected_typ` — Required value of the JWT header `typ`, for example `at+jwt` for RFC 9068 access tokens.
- `claims` — JSON object that the token payload must contain, for example `{"tenant":"acme"}`.
- `allow_no_expiration` — Accept tokens without an `exp` claim. Default: `false`.
- `verifier_leeway` — Allowed clock skew between ClickHouse and the IdP, in seconds. Default: `60`.

A warning is logged at startup for each of `expected_issuer`, `expected_audience` and `expected_typ` that is not set.

#### `jwt_static_key` {#jwt-static-key}

```xml
<my_static_key_validator>
    <type>jwt_static_key</type>
    <algo>HS256</algo>
    <static_key>my_static_secret</static_key>
</my_static_key_validator>
```

- `algo` — Signature algorithm. Mandatory. Supported: `HS256`, `HS384`, `HS512`, `RS256`, `RS384`, `RS512`, `ES256`, `ES384`, `ES512`, `ES256K`, `PS256`, `PS384`, `PS512`, `Ed25519`, `Ed448`, and `None` (never use it in production).
- `static_key` — Key for `HS*` algorithms. `static_key_in_base64` — whether `static_key` is base64-encoded, default `false`.
- `public_key` — Public key for all other algorithms except `None`. `private_key`, `public_key_password`, `private_key_password` — optional.

#### `jwt_static_jwks` {#jwt-static-jwks}

```xml
<my_static_jwks_validator>
    <type>jwt_static_jwks</type>
    <static_jwks>{"keys": [{"kty": "RSA", "alg": "RS256", "kid": "mykid", "n": "_public_key_mod_", "e": "AQAB"}]}</static_jwks>
</my_static_jwks_validator>
```

- `static_jwks` — JWKS content as JSON, or `static_jwks_file` — path to a file with it. Exactly one of the two is required.

JWKS-based processors (`jwt_static_jwks`, `jwt_dynamic_jwks`, `entra`) support the `RS*` and `ES*` algorithms.

#### `jwt_dynamic_jwks` {#jwt-remote-jwks}

```xml
<my_jwks_validator>
    <type>jwt_dynamic_jwks</type>
    <jwks_uri>https://idp.example.com/.well-known/jwks.json</jwks_uri>
    <expected_issuer>https://idp.example.com</expected_issuer>
    <expected_audience>clickhouse</expected_audience>
</my_jwks_validator>
```

- `jwks_uri` — JWKS endpoint. Mandatory.
- `jwks_cache_lifetime` — How often the JWKS is re-fetched, in seconds. Default: `3600`.

### Entra {#entra}

`entra` is a preset of `jwt_dynamic_jwks` for Microsoft Entra ID: tokens are verified locally against the tenant's JWKS, and the user name and groups are read from the token. The token's audience must be your own application (registered with *Expose an API*), not Microsoft Graph — Graph-audience tokens cannot be verified by third parties.

```xml
<entra_prod>
    <type>entra</type>
    <tenant_id>aaaabbbb-0000-cccc-1111-dddd2222eeee</tenant_id>
    <expected_audience>api://clickhouse</expected_audience>
    <username_claim>preferred_username</username_claim>
</entra_prod>
```

- `tenant_id` — Tenant GUID or `*.onmicrosoft.com` domain. Mandatory. Multi-tenant aliases (`common`, `organizations`, `consumers`) are rejected.
- `expected_audience` — Your application's ID URI (for example `api://clickhouse`) or client ID. Strongly recommended: without it, any token from the tenant is accepted.
- `jwks_uri` — Default: `https://login.microsoftonline.com/{tenant_id}/discovery/v2.0/keys`. Override only for sovereign clouds.
- `expected_issuer` — Default: `https://login.microsoftonline.com/{tenant_id}/v2.0`. Override for v1.0 tokens (`https://sts.windows.net/{tenant_id}/`) or sovereign clouds.
- `username_claim` — Default `sub`; common choices for Entra are `preferred_username`, `upn` and `oid`.
- All other [JWT processor](#jwt) parameters, `jwks_cache_lifetime` and `token_cache_lifetime`.

`<type>azure</type>` is accepted as an alias for `entra`. A former `azure` configuration needs `tenant_id` (and should get `expected_audience`), and the application must issue tokens for its own audience rather than for Microsoft Graph.

#### Mapping Entra groups to ClickHouse roles {#entra-group-mapping}

By default, the `groups` claim contains group object IDs (GUIDs). To get readable values:

- **App Roles (recommended).** In the app registration, create app roles whose `Value` is the string ClickHouse should receive (for example `ch_admin`). In the enterprise application, set *Assignment required* to *Yes* and assign users or groups to the roles (assigning groups requires Entra ID P1/P2). Then set `<groups_claim>roles</groups_claim>`. This is compact and unaffected by group renames.
- **Group names in the `groups` claim.** In the app registration, set `groupMembershipClaims` to `ApplicationGroup` (or `SecurityGroup`) and add the `groups` optional access-token claim with `additionalProperties`: `sam_account_name` or `dns_domain_and_sam_account_name` for groups synced from on-premises AD, `cloud_displayname` for cloud-only groups. Groups not covered still arrive as GUIDs, and renaming a group in Entra breaks the mapping.
- **Keep GUIDs** and translate them with `roles_mapping` in the [user directory](#idp-external-user-directory).

If you switch from GUIDs to names, revisit `roles_filter`.

### Google {#google}

`google` validates Google OAuth 2.0 access tokens. They are opaque, so each token is checked with Google's `/tokeninfo` endpoint, and `/userinfo` is also called when `username_claim` is not in the `/tokeninfo` response.

```xml
<google_prod>
    <type>google</type>
    <expected_audience>1234567890-abcdef.apps.googleusercontent.com</expected_audience>
</google_prod>
```

- `expected_audience` — OAuth client ID the token must be issued to. Strongly recommended: without it, any Google access token is accepted.
- `username_claim` — Default `sub` (the Google account ID). `email` requires the `email` scope.
- `token_cache_lifetime`.

**Groups.** When the token carries the user's email, the processor reads the user's direct Google Groups memberships through the Cloud Identity API and uses the group email addresses as groups. This requires the `https://www.googleapis.com/auth/cloud-identity.groups.readonly` scope in the token and the Cloud Identity API enabled in the project of the OAuth client. Groups inherited through nested groups are not returned. If the lookup fails, the user is authenticated without groups. To turn group emails into role names, use `roles_filter` and `roles_transform`; for example, this maps `ch-analysts@example.com` to the role `ch-analysts`:

```xml
<token>
    <processor>google_prod</processor>
    <roles_filter>ch-[a-z-]+@example\.com</roles_filter>
    <roles_transform>s/@.*//</roles_transform>
</token>
```

### OpenID {#openid}

`openid` works with any OpenID Connect provider and is configured in one of two modes.

**Discovery mode** reads the endpoints from the provider's discovery document. If the document has a `jwks_uri`, JWT access tokens are verified locally. If it has an `introspection_endpoint` and introspection credentials are set, every login is also checked with [RFC 7662](https://datatracker.ietf.org/doc/html/rfc7662) introspection, which detects revoked tokens.

```xml
<oid_discovery>
    <type>openid</type>
    <configuration_endpoint>https://idp.example.com/.well-known/openid-configuration</configuration_endpoint>
    <expected_audience>my-clickhouse-client-id</expected_audience>
    <introspection_client_id>clickhouse-rs</introspection_client_id>
    <introspection_client_secret>...</introspection_client_secret>
</oid_discovery>
```

- `configuration_endpoint` — URL of `.well-known/openid-configuration`. Mandatory.
- `expected_issuer`, `expected_audience` — Required `iss` and `aud`, checked on the JWT or in the introspection response.
- `introspection_client_id`, `introspection_client_secret` — Credentials for the introspection endpoint. Set both or neither.
- `allow_no_expiration`, `verifier_leeway` (default `60`), `jwks_cache_lifetime` (default `3600`) — As for [JWT processors](#jwt).
- `allow_http_discovery_urls` — Allow non-HTTPS URLs in the discovery document. Default: `false`.

**Manual mode** calls `/userinfo` for every new token. Without introspection, it cannot check the token's issuer, audience or expiration.

```xml
<oid_manual>
    <type>openid</type>
    <userinfo_endpoint>https://idp.example.com/userinfo</userinfo_endpoint>
    <token_introspection_endpoint>https://idp.example.com/introspect</token_introspection_endpoint>
    <introspection_client_id>clickhouse-rs</introspection_client_id>
    <introspection_client_secret>...</introspection_client_secret>
    <expected_issuer>https://idp.example.com</expected_issuer>
    <expected_audience>clickhouse-rs</expected_audience>
</oid_manual>
```

- `userinfo_endpoint` — Mandatory.
- `token_introspection_endpoint`, `introspection_client_id`, `introspection_client_secret` — Optional, all three together.
- `expected_issuer`, `expected_audience` — Allowed only together with introspection.

`configuration_endpoint` and `userinfo_endpoint` are mutually exclusive, and `jwks_uri` is rejected in both modes — use `jwt_dynamic_jwks` for an explicit JWKS URL. The introspection credentials accept `from_env` and `from_zk`.

Introspection needs a separate confidential OAuth client representing ClickHouse, without redirect URIs:

| IdP | How to create the introspection client |
|---|---|
| Keycloak | Clients → confidential client with *Service Accounts* enabled; the secret is on the *Credentials* tab. |
| Okta | Applications → Create App Integration → *API Services*. |
| Auth0 | No introspection for opaque tokens; for JWT access tokens of a custom API use `jwt_dynamic_jwks`. |
| Google, GitHub, Microsoft Entra ID | No RFC 7662 endpoint; use `google`, `entra` or `jwt_dynamic_jwks`. |

## Users defined in ClickHouse {#enabling-jwt-auth}

A user can authenticate with a token if `jwt` is one of its authentication methods. 

:::tip
Locally-defined users are only _authenticated_ with tokens; no role mapping is done.
:::

In `users.xml`, where a user has exactly one authentication method, the `jwt` section replaces `password`.

```sql
CREATE USER alice IDENTIFIED WITH jwt PROCESSOR 'my_processor' CLAIMS '{"resource_access":{"account":{"roles":["view-profile"]}}}';
```

In `users.xml`:

```xml
<clickhouse>
    <users>
        <alice>
            <jwt>
                <processor>my_processor</processor>
                <claims>{"resource_access":{"account":{"roles":["view-profile"]}}}</claims>
            </jwt>
        </alice>
    </users>
</clickhouse>
```

Both clauses are optional:

- `PROCESSOR` / `processor` — Accept only tokens validated by this processor. Without it, any configured processor may validate the token.
- `CLAIMS` / `claims` — JSON object the token payload must contain. In the example, `resource_access.account.roles` must contain `"view-profile"`. Claims can only be checked in JWTs validated by `jwt_static_key`, `jwt_static_jwks`, `jwt_dynamic_jwks` or `entra`; a user with `claims` cannot log in with a token validated by another processor.

## IdP as an external user directory {#idp-external-user-directory}

With a `token` section in `user_directories`, a user that is not defined in ClickHouse can log in with a valid token. The user is created on first login with the name taken from `username_claim`, and its roles are updated on every login. Roles and settings profiles must exist in ClickHouse, created with SQL-driven [access control](/concepts/features/security/access-rights).

```xml
<clickhouse>
    <user_directories>
        <token>
            <processor>my_processor</processor>
            <common_roles>
                <token_user />
            </common_roles>
            <default_profile>token_users_profile</default_profile>
            <roles_mapping>
                <map>
                    <from>8a1b2c3d-4e5f-6789-abcd-ef0123456789</from>
                    <to>ch_admin</to>
                </map>
            </roles_mapping>
            <roles_filter>\bclickhouse-[a-zA-Z0-9]+\b</roles_filter>
            <roles_transform>s/-/_/g</roles_transform>
        </token>
    </user_directories>
</clickhouse>
```

Only one `token` section is allowed.

- `processor` — Name of the token processor. Mandatory.
- `common_roles` — Roles granted to every user from this directory.
- `default_profile` — Settings profile assigned to every user from this directory. If the profile does not exist, users are created without it.
- `profiles_mapping` — Settings profiles assigned by group; see [Settings profiles](#idp-settings-profiles).
- `networks` — Addresses that users from this directory may connect from, in the same format as [`networks`](/concepts/features/configuration/settings/settings-users#user-namenetworks) in `users.xml`. Default: any address.

The user's groups (from `groups_claim`) are turned into role names by three optional steps, applied in this order:

1. `roles_mapping` — Explicit renames, one `<map>` with `<from>` and `<to>` per group. Groups without an entry pass unchanged.
2. `roles_filter` — Regular expression; groups that do not match it are dropped.
3. `roles_transform` — Sed-style substitution `s/pattern/replacement/flags`; the `g` flag replaces all occurrences. For example, `s/-/_/g` turns `clickhouse-grp-dba` into `clickhouse_grp_dba`.

The user is granted every resulting role that exists in ClickHouse.

### Settings profiles {#idp-settings-profiles}

To give different settings to different groups of users, for example to users connecting from Grafana, map groups to settings profiles with `profiles_mapping`:

```sql
CREATE SETTINGS PROFILE grafana_profile SETTINGS max_execution_time = 60, readonly = 1;
```

```xml
<token>
    <processor>my_processor</processor>
    <default_profile>token_users_profile</default_profile>
    <profiles_mapping>
        <map>
            <from>grafana-viewers</from>
            <to>grafana_profile</to>
        </map>
    </profiles_mapping>
</token>
```

Users with the `grafana-viewers` group get `grafana_profile` in addition to `default_profile`. `from` is the group exactly as it comes in the token: `roles_mapping`, `roles_filter` and `roles_transform` do not apply. Profiles are reassigned on every login, so a user who leaves a group loses its profile.

If a setting is defined in several places, the first source in this list wins:

1. Profiles from `profiles_mapping`. When the user is in several mapped groups, the entry that comes later in `profiles_mapping` wins.
2. `default_profile`.
3. Profiles of the user's roles. A profile can be attached to a role with `CREATE ROLE ... SETTINGS PROFILE '...'`; when several roles set the same setting, which one wins is undefined.
4. The server's [default profile](/reference/settings/server-settings/settings/default#default_profile).

In distributed queries, the resulting settings that differ from their defaults are sent to the other nodes with the query, so the profiles only need to exist on the node the user connects to.

Limitation: if the user has logged in directly to another node, that node keeps the profiles and roles from that login until the user's next direct login there. Its part of a distributed query uses those profiles' settings and constraints, even if the user's groups have changed.

## Forwarding the token to external services {#token-forwarding}

The user's token can be forwarded to Iceberg REST or Glue catalogs. Enable it with `<enable_token_forwarding>1</enable_token_forwarding>` in `config.xml` (hot-reloadable, default `0`) and `oauth_forward_user_token` on the [`DataLakeCatalog`](/engines/database-engines/datalakecatalog) database; see [catalog token forwarding](/engines/database-engines/datalakecatalog#user-token-forwarding).

:::danger Restrict `CREATE DATABASE`
Whoever creates the database chooses the endpoints that receive users' tokens. Grant `CREATE DATABASE` only to trusted users, restrict `remote_url_allow_hosts` for the catalog and token-exchange endpoints, and use HTTPS for them. In passthrough mode the forwarded token is also valid for ClickHouse itself.
:::

- Only the session's verified token is forwarded. It is not inherited by `EXECUTE AS` or `DEFINER` views, not sent to other ClickHouse nodes, and not written to disk.
- HTTP requests authenticate separately, so a new token takes effect on the next request; native TCP sessions must reconnect.
- With `async_insert = 1`, each queued batch keeps its token until it is flushed, even with `wait_for_async_insert = 0`. Inserts with different tokens go to separate batches.

## clickhouse-client {#clickhouse-client}

`clickhouse-client` can get a token in three ways (see its [connection options](/concepts/features/interfaces/client#command-line-options-connection)):

- `--jwt <token>` — Use the given token.
- `--login=browser` or `--login=device` — Log in to an OpenID Connect provider with the Authorization Code + PKCE flow (opens a browser) or the Device Authorization flow (prints a URL and a code). Requires an [OAuth credentials file](#oauth-credentials-file).
- `--jwt-command <command>` — Get the token from a [command](#jwt-command-details).

### OAuth credentials file {#oauth-credentials-file}

`--login=browser` and `--login=device` read the OAuth client from a JSON file, by default `~/.clickhouse-client/oauth_client.json`, or the path given with `--oauth-credentials <path>`. The format is the one that the Google Cloud Console downloads for an OAuth client ID:

```json
{
  "installed": {
    "client_id": "YOUR_CLIENT_ID",
    "client_secret": "YOUR_CLIENT_SECRET",
    "auth_uri": "https://accounts.google.com/o/oauth2/auth",
    "token_uri": "https://oauth2.googleapis.com/token",
    "redirect_uris": ["http://127.0.0.1"]
  }
}
```

- The top-level key is `installed` or `web`.
- `client_id`, `auth_uri`, `token_uri` — Mandatory.
- `client_secret` — Omit it or leave it empty for public clients; it is then not sent.
- `device_authorization_uri` — For `--login=device`. If absent, it is discovered through OIDC discovery from `issuer`.

The refresh token is cached in `~/.clickhouse-client/oauth_cache.json` (mode `0600`), so the browser or device code is needed again only after it expires.

### `--jwt-command` {#jwt-command-details}

```bash
clickhouse-client --jwt-command "curl -sS https://idp.example/token | jq -r .access_token"
```

The command runs with `/bin/sh -c` and stdin closed. Its stdout, without the trailing newline, is the token; prompts, URLs and device codes must go to stderr, which is passed through to the client's stderr.

The command runs on the first connection and runs again when the cached token is a JWT expiring within 30 seconds, or when the server rejects the token (the connection is then retried once). Opaque tokens and JWTs without `exp` are reused until the server rejects them.

The command fails authentication if it exits with a non-zero code, prints nothing, or exceeds `--jwt-command-timeout` (default `30` seconds, also settable as `<jwt-command-timeout>` in the client config). On timeout, the command's whole process tree is killed. `--jwt-command` cannot be combined with `--jwt`, `--login` or a non-default `--user`.
