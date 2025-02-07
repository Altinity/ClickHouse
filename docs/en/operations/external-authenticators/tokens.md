---
slug: /en/operations/external-authenticators/oauth
title: "OAuth 2.0"
---
import SelfManaged from '@site/docs/en/_snippets/_self_managed_only_no_roadmap.md';

<SelfManaged />

OAuth 2.0 access tokens can be used to authenticate ClickHouse users. This works in two ways:

- Existing users (defined in `users.xml` or in local access control paths) can be authenticated with access token if this user can be `IDENTIFIED WITH jwt`. 
- Use Identity Provider (IdP) as an external user directory and allow locally undefined users to be authenticated with a token if it is valid and recognized by the provider.

Though this authentication method is different from JWT authentication, it works under the same authentication method to maintain better compatibility. 

For both of these approaches a definition of `access_token_processors` is mandatory.

## Access Token Processors

To define an access token processor, add `access_token_processors` section to `config.xml`. Example:
```xml
<clickhouse>
    <access_token_processors>
        <gogoogle>
            <provider>Google</provider>
            <email_filter>^[A-Za-z0-9._%+-]+@example\.com$</email_filter>
        </gogoogle>
        <azuure>
            <provider>azure</provider>
            <client_id>CLIENT_ID</client_id>
            <tenant_id>TENANT_ID</tenant_id>
        </azuure>
    </access_token_processors>
</clickhouse>
```

:::note
Different providers have different sets of parameters.
:::

**Parameters**

- `provider` -- name of identity provider. Mandatory, case-insensitive. Supported options: "Google", "Azure".
- `email_filter` -- Regex for validation of user emails. Optional parameter, only for Google IdP.
- `client_id` -- Azure AD (Entra ID) client ID. Optional parameter, only for Azure IdP.
- `tenant_id` -- Azure AD (Entra ID) tenant ID. Optional parameter, only for Azure IdP.

## IdP as External Authenticator {#idp-external-authenticator}

Locally defined users can be authenticated with an access token. To allow this, `jwt` must be specified as user's authentication method. Example:

```xml
<clickhouse>
    <!- ... -->
    <users>
        <!- ... -->
        <my_user>
            <!- ... -->
            </jwt>
        </my_user>
    </users>
</clickhouse>
```

At each login attempt, ClickHouse will attempt to validate token and get user info against every defined access token provider.

When SQL-driven [Access Control and Account Management](/docs/en/guides/sre/user-management/index.md#access-control) is enabled, users that are authenticated with tokens can also be created using the [CREATE USER](/docs/en/sql-reference/statements/create/user.md#create-user-statement) statement.

Query:

```sql
CREATE USER my_user IDENTIFIED WITH jwt;
```

## Identity Provider as an External User Directory {#idp-external-user-directory}

If there is no suitable user pre-defined in ClickHouse, authentication is still possible: Identity Provider can be used as source of user information.
To allow this, add `token` section to the `users_directories` section of the `config.xml` file. 

At each login attempt, ClickHouse tries to find the user definition locally and authenticate it as usual.
If the user is not defined, ClickHouse will treat user as externally defined, and will try to validate the token and get user information from the specified processor.
If validated successfully, the user will be considered existing and authenticated. The user will be assigned roles from the list specified in the `roles` section. 
All this implies that the SQL-driven [Access Control and Account Management](/docs/en/guides/sre/user-management/index.md#access-control) is enabled and roles are created using the [CREATE ROLE](/docs/en/sql-reference/statements/create/role.md#create-role-statement) statement.

**Example**

```xml
<clickhouse>
    <token>
        <processor>gogoogle</processor>
        <roles>
            <token_test_role_1 />
        </roles>
    </token>
</clickhouse>
```

**Parameters**

- `server` — Name of one of processors defined in `access_token_processors` config section described above. This parameter is mandatory and cannot be empty.
- `roles` — Section with a list of locally defined roles that will be assigned to each user retrieved from the IdP.
