---
description: 'OAuth2 / OpenID Connect login for clickhouse-client'
sidebar_label: 'OAuth login'
sidebar_position: 18
slug: /interfaces/cli-oauth-login
---

# OAuth login for `clickhouse-client`

`clickhouse-client` can authenticate to a server by obtaining an OpenID Connect ID token from a third-party identity provider (IdP) and presenting it as a JWT. Every setting can be supplied either as a `<oauth-*>` field in `~/.clickhouse-client/config.xml` or as the equivalent CLI flag — flags win on conflict. The connection block is the recommended home for stable per-server settings; CLI flags are for one-off overrides.

| XML field | CLI flag |
|---|---|
| `<login>` | `--login[=browser\|device]` |
| `<oauth-url>` | `--oauth-url` |
| `<oauth-client-id>` | `--oauth-client-id` |
| `<oauth-audience>` | `--oauth-audience` |
| `<oauth-client-secret>` | `--oauth-client-secret` |
| `<oauth-callback-port>` | `--oauth-callback-port` |

## Configuration model

Inside `<connections_credentials>`, a single `<connection>` holds both the server endpoint and its OAuth settings:

```xml
<connections_credentials>
    <connection>
        <name>my-server</name>
        <hostname>db.example.com</hostname>
        <port>9440</port>
        <secure>1</secure>

        <login>browser</login>                <!-- or device, or empty for cloud -->
        <oauth-url>https://idp.example.com</oauth-url>
        <oauth-client-id>YOUR_CLIENT_ID</oauth-client-id>
        <oauth-audience>https://db.example.com/</oauth-audience>
        <oauth-client-secret>...</oauth-client-secret>   <!-- optional -->
        <oauth-callback-port>49152</oauth-callback-port> <!-- optional -->
    </connection>
</connections_credentials>
```

Then:

```bash
clickhouse-client --connection my-server -q "SELECT 1"
```

| Field | Required | Description |
|---|---|---|
| `<login>` | yes | `browser` (auth-code + PKCE), `device` (device flow), or empty (ClickHouse Cloud auto-login). |
| `<oauth-url>` | for `browser` / `device` | OIDC issuer URL. Endpoints (`authorization_endpoint`, `token_endpoint`, `device_authorization_endpoint`) are fetched from `<oauth-url>/.well-known/openid-configuration`. |
| `<oauth-client-id>` | for `browser` / `device` | OAuth 2.0 client ID registered with the IdP. |
| `<oauth-audience>` | depends on IdP | Required when the IdP's access token would otherwise be opaque (Auth0). For pure id-token flows it can be omitted. |
| `<oauth-client-secret>` | rarely | Confidential-client secret. Omit for native/public clients (RFC 8252 §8.4) — PKCE and the device code provide client-binding. Sending an empty secret is rejected by Auth0/Entra ID/Keycloak/Okta as `invalid_client`. Some Google "Desktop app" registrations issue a secret that is not really secret; include it only if the IdP requires it. |
| `<oauth-callback-port>` | only for `browser` against IdPs that don't honor RFC 8252 §7.3 | Pin the loopback port the auth-code flow's local HTTP server binds to. The value `0` is reserved for **"any port"** semantics (kernel-picks an ephemeral port at flow start) and is the only value valid against RFC 8252 §7.3-compliant IdPs — currently that means Google. For Auth0 (and any other IdP that does literal-string match on the registered callback URL) you **must** pin a non-zero port and register `http://127.0.0.1:<port>/callback` in the IdP's allowed callback URLs. Default: `0`. |

After a successful login the refresh token is cached in `~/.clickhouse-client/oauth_cache.json` (file mode `0600`, keyed by `client_id`). Subsequent invocations reuse the cached token silently and only open the browser or print a device code when the refresh token has expired or been revoked.

## Modes

### `<login>browser</login>` — Authorization Code + PKCE

The client starts a one-shot HTTP server on `127.0.0.1`, opens your browser at `http://127.0.0.1:<port>/start`, which 302-redirects to the IdP's authorization endpoint with a fresh PKCE challenge and CSRF state. The user authenticates in the browser; the IdP redirects back to `http://127.0.0.1:<port>/callback?code=...`; the client exchanges the code for an id_token at the IdP's token endpoint. The local server shuts down immediately after.

The redirect URI you must register in the IdP is exactly:

```
http://127.0.0.1/callback           # or http://127.0.0.1:<oauth-callback-port>/callback
```

PKCE state and the CSRF parameter are kept off the launcher's argv via the `/start` indirection — the browser sees only `http://127.0.0.1:<port>/start` on its command line, never the full auth URL.

### `<login>device</login>` — Device Authorization Grant (RFC 8628)

The client requests a `device_code`/`user_code` pair from the IdP, prints them to the terminal, and polls the token endpoint until the user approves on a separate device. No callback URL or browser process on the client machine is required, so this is the correct mode in the absence of a desktop session (SSH-only servers, terminal-only environments, headless CI).

```
To authenticate, visit:
  https://idp.example.com/activate
And enter code: ABCD-EFGH

Waiting for authorization (this code expires in 900 seconds)...
Authentication successful.
```

### Bare `<login></login>` (or `--login` on the CLI) — ClickHouse Cloud auto-login

When the `<oauth-url>` and `<oauth-client-id>` fields are absent, the client defers to the cloud-only auto-detect path: the provider is inferred from the server (it must be a `*.clickhouse.cloud` / `*.clickhouse-staging.com` / `*.clickhouse-dev.com` endpoint, otherwise the client errors). This is the only OAuth form supported on the command line via plain `--login`.

## Provider-specific notes

### Auth0

| Setting | Value |
|---|---|
| Application Type | **Native** (required so Auth0 returns `device_authorization_endpoint` from OIDC discovery and so `audience` is honored) |
| `<oauth-url>` | `https://YOUR_TENANT.auth0.com` (no trailing slash — the client appends paths) |
| `<oauth-audience>` | The exact API identifier registered in **Applications → APIs**, including any trailing slash. Auth0 matches the audience as a literal string, so `https://api.example.com/` and `https://api.example.com` are different. |
| `<oauth-client-secret>` | Omit. Native clients are public; sending a secret causes `invalid_client`. |
| `<oauth-callback-port>` | **Required for `<login>browser</login>`.** Auth0 [does not implement port wildcarding for loopback redirects](https://community.auth0.com/t/allow-wildcard-port-in-redirect-uri-as-per-rfc-8252/98409) (RFC 8252 §7.3), so you must pin a port and register the exact `http://127.0.0.1:<port>/callback` in **Allowed Callback URLs**. Not needed for `<login>device</login>`. |

### Google

| Setting | Value |
|---|---|
| Application Type | **Desktop app** in Google Cloud Console → APIs & Services → Credentials |
| `<oauth-url>` | `https://accounts.google.com` |
| `<oauth-client-id>` | The "Client ID" from the Desktop app credentials |
| `<oauth-client-secret>` | The "Client secret" — Google's Desktop apps issue one, and the token endpoint requires it even though it is not secret in the cryptographic sense. This is a long-standing Google quirk; include it. |
| `<oauth-audience>` | Omit. Google id-tokens carry `aud = client_id`; ClickHouse's JWT validator should verify against the client_id. |
| Callback URL registration | None per port. Google honors RFC 8252 §7.3 and accepts any loopback port for Desktop apps. |

### Keycloak / Entra ID / generic OIDC

| Setting | Value |
|---|---|
| `<oauth-url>` | The issuer URL exactly as it appears in the discovery document's `issuer` field |
| `<oauth-client-secret>` | Omit for clients registered as `public`, include only if the realm/app explicitly requires `client_authentication` |
| Discovery requirements | The discovery document at `<oauth-url>/.well-known/openid-configuration` must publish `authorization_endpoint`, `token_endpoint`, and `device_authorization_endpoint`. Keycloak and Entra ID publish all three by default. |

## Server-side configuration

OAuth login on the client only obtains an id_token; the server still has to be configured to accept JWTs from your IdP. See [external authenticators – JWT](../operations/external-authenticators/jwt.md) for the server-side JWKS/audience/issuer configuration. The client's `<oauth-audience>` must match the audience the server validates against.

## Security considerations

### When to use which mode

* **`browser`** is the right default for interactive desktop use. PKCE binds the auth code to the originating client without requiring a secret, and the loopback redirect keeps the code off any external network.
* **`device`** is the right choice for terminal-only sessions (SSH, containers, CI workers with a human operator on a different device). The user_code is short and presented out-of-band, so phishing risk is bounded by the IdP's own consent page.

### Device flow risk for DBMS access

The Device Authorization Grant trades a redirect URI for a short, transcribable user_code. That tradeoff is appropriate for read-only consumer surfaces (TVs, set-top boxes — the original RFC 8628 use case) but **deserves explicit threat modeling before being enabled against a database**. The risk is not theoretical — it is documented in the protocol specification itself:

> **RFC 8628 §5.3 Remote Phishing.** *It is possible for the device flow to be initiated on a device in an attacker's possession. […] To mitigate such an attack, it is RECOMMENDED to inform the user that they are authorizing a device during the user-interaction step (see Section 3.3) and to confirm that the device is in their possession.*
> — [RFC 8628: OAuth 2.0 Device Authorization Grant, §5.3](https://www.rfc-editor.org/rfc/rfc8628.html#section-5.3)

See also [§5.4 Session Spying](https://www.rfc-editor.org/rfc/rfc8628.html#section-5.4) and [§5.5 Non-Confidential Clients](https://www.rfc-editor.org/rfc/rfc8628.html#section-5.5).

In practice this gives three concrete failure modes for DBMS access:

1. **No origin binding.** Unlike the auth-code flow, the device flow has nothing tying the approval to the device that initiated it. Anyone in possession of the user_code who can reach the IdP's verification page can approve the session.
2. **Phishable by construction (RFC 8628 §5.3).** A polling client cannot distinguish a legitimately-obtained user_code from one solicited by an attacker who tricked the victim into running a separate `clickhouse-client --login=device` instance. The well-known "consent-phishing" pattern: attacker initiates the device flow against a permissive client, sends the verification URL/code to the victim by chat or email under any pretext, and receives a token bound to the victim's identity once they approve. The RFC's §5.3 mitigation (the IdP's consent page must show what is being authorized) is only as strong as the IdP's consent UI and the user's attention to it — it does not eliminate the risk, only narrows it.
3. **Internal services.** The risk surface is narrow when the audience is an internal database that already requires VPN or zero-trust network access — the attacker would need both a phished consent and network reach. It widens dramatically for databases exposed on the public internet.

**Recommended posture:**

* For production, internet-facing, or otherwise sensitive ClickHouse deployments, use `<login>browser</login>` and disable the device grant on the IdP for the relevant client (Auth0: Application → Advanced Settings → Grant Types → uncheck Device Code; Keycloak: per-client capability config).
* For testing, public datasets, or clusters reachable only from a hardened operational network, `<login>device</login>` is acceptable.
* Never enable the device grant for a client whose tokens grant write or DDL privileges on a production DBMS without compensating controls (short token TTL, MFA on the IdP, alerting on first-use of a device-flow token, network-level access policy).

The `Authentication successful.` line in your terminal is not a substitute for verifying that the consent screen the operator approved was the one this `clickhouse-client` invocation initiated.

### Refresh tokens at rest

`~/.clickhouse-client/oauth_cache.json` is created mode `0600` and keyed by SHA-256 of `client_id`. Anyone who can read the file as your UID can resume a session until the IdP revokes the refresh token. Treat the file as the equivalent of a long-lived password and exclude it from backups, dotfile sync, and shared-host filesystems.

## Example: full `~/.clickhouse-client/config.xml`

```xml
<clickhouse>
    <connections_credentials>
        <!-- Internal cluster, browser flow, fixed loopback port for Auth0 -->
        <connection>
            <name>analytics</name>
            <hostname>analytics.example.com</hostname>
            <port>9440</port>
            <secure>1</secure>
            <login>browser</login>
            <oauth-url>https://example.auth0.com</oauth-url>
            <oauth-client-id>abc123</oauth-client-id>
            <oauth-audience>https://analytics.example.com/</oauth-audience>
            <oauth-callback-port>49152</oauth-callback-port>
        </connection>

        <!-- Headless CI worker, device flow, Google IdP -->
        <connection>
            <name>warehouse-ci</name>
            <hostname>warehouse.example.com</hostname>
            <port>9440</port>
            <secure>1</secure>
            <login>device</login>
            <oauth-url>https://accounts.google.com</oauth-url>
            <oauth-client-id>123456-abc.apps.googleusercontent.com</oauth-client-id>
            <oauth-client-secret>GOCSPX-...</oauth-client-secret>
        </connection>

        <!-- ClickHouse Cloud — bare login, no IdP fields -->
        <connection>
            <name>cloud</name>
            <hostname>tenant.us-east-1.aws.clickhouse.cloud</hostname>
            <port>9440</port>
            <secure>1</secure>
            <login></login>
        </connection>
    </connections_credentials>
</clickhouse>
```
