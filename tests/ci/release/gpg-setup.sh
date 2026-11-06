#!/usr/bin/env bash
# Import the release GPG key and cache its passphrase in gpg-agent, or remove it again.
#
#   gpg-setup.sh setup <staging|production>   env: SIGNING_SECRET_ID, GPG_PASSPHRASE
#   gpg-setup.sh cleanup [fingerprint]
#
# Secrets only travel through pipes; nothing secret is written to disk.
set -euo pipefail

setup() {
  local field
  case "$1" in
    production) field=altinity_prod_gpg ;;
    staging)    field=altinity_staging_gpg ;;
    *) echo "Unknown environment: $1" >&2; exit 1 ;;
  esac
  : "${SIGNING_SECRET_ID:?is not set}" "${GPG_PASSPHRASE:?is not set}"
  local out="${GITHUB_OUTPUT:-/dev/null}"

  local fpr
  fpr=$(aws secretsmanager get-secret-value --query SecretString --output text \
          --secret-id "arn:aws:secretsmanager:${AWS_REGION:-us-east-1}:${SIGNING_SECRET_ID}" \
        | sed -e "s/^'//" -e "s/'$//" | jq -r ".${field} | @base64d" \
        | gpg --batch --quiet --status-fd 1 --import \
        | awk '$2 == "IMPORT_OK" && !f { print $4; f = 1 }')
  [ -n "$fpr" ] || { echo "No key imported from the signing secret" >&2; exit 1; }
  echo "fingerprint=$fpr" >> "$out"

  local listing grip name
  listing=$(gpg --batch --with-colons --with-keygrip --list-secret-keys "$fpr")
  grip=$(awk -F: '$1 == "grp" { print $10; exit }' <<< "$listing")
  name=$(awk -F: '$1 == "uid" { print $10; exit }' <<< "$listing")
  echo "Imported signing key $fpr ($name)"

  gpgconf --kill gpg-agent
  gpg-agent --daemon --allow-preset-passphrase > /dev/null

  base64 -d <<< "$GPG_PASSPHRASE" \
    | openssl enc -d -aes-256-cbc -pbkdf2 -pass fd:3 \
        3< <(aws ssm get-parameter --name /gitlab-runner/key-encrypting-key --with-decryption \
               --query Parameter.Value --output text) \
    | "$(gpgconf --list-dirs libexecdir)/gpg-preset-passphrase" --preset "$grip"

  echo test | gpg --batch --pinentry-mode error --local-user "$fpr" --clearsign > /dev/null \
    || { echo "Test signature failed: wrong GPG_PASSPHRASE?" >&2; exit 1; }
  echo "Signing key unlocked"
  { echo "key_id=$grip"; echo "key_name=$name"; } >> "$out"
}

cleanup() {
  gpgconf --kill gpg-agent || true
  if [ -n "${1:-}" ]; then
    gpg --batch --yes --delete-secret-and-public-key "$1"
  fi
  rm -f /tmp/gpg_passphrase
}

case "${1:-}" in
  setup)   setup "${2:?environment required}" ;;
  cleanup) cleanup "${2:-}" ;;
  *) echo "Usage: $0 setup <staging|production> | cleanup [fingerprint]" >&2; exit 2 ;;
esac
