-- Repeating `ADD IDENTIFIED WITH` must not accumulate identical methods.

DROP USER IF EXISTS user_05366;

CREATE USER user_05366 IDENTIFIED WITH plaintext_password BY 'p1';

ALTER USER user_05366 ADD IDENTIFIED WITH ldap SERVER 'srv';
ALTER USER user_05366 ADD IDENTIFIED WITH ldap SERVER 'srv';
SELECT auth_type, auth_params FROM system.users WHERE name = 'user_05366';

-- A method repeated within one statement is added once.
ALTER USER user_05366 ADD IDENTIFIED WITH kerberos REALM 'r', kerberos REALM 'r';
SELECT auth_type, auth_params FROM system.users WHERE name = 'user_05366';

-- The same hash with a different salt is a different credential.
ALTER USER user_05366 ADD IDENTIFIED WITH sha256_hash BY 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855' SALT 'salt1';
ALTER USER user_05366 ADD IDENTIFIED WITH sha256_hash BY 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855' SALT 'salt2';
SELECT auth_type FROM system.users WHERE name = 'user_05366';

DROP USER user_05366;

-- Deduplicate after applying the user-level deadline.
CREATE USER user_05366 IDENTIFIED WITH plaintext_password BY 'p1';
ALTER USER user_05366 VALID UNTIL '2099-01-01 00:00:00 UTC' ADD IDENTIFIED WITH ldap SERVER 'srv';
ALTER USER user_05366 VALID UNTIL '2099-01-01 00:00:00 UTC' ADD IDENTIFIED WITH ldap SERVER 'srv';
SELECT auth_type, arrayMap(x -> toUInt32(x), valid_until) FROM system.users WHERE name = 'user_05366';

DROP USER user_05366;

-- `RESET AUTHENTICATION METHODS TO NEW` keeps the method added last, even if the user already had it.
CREATE USER user_05366 IDENTIFIED WITH plaintext_password BY 'p1', ldap SERVER 'srv';
ALTER USER user_05366 ADD IDENTIFIED WITH plaintext_password BY 'p1';
ALTER USER user_05366 RESET AUTHENTICATION METHODS TO NEW;
SELECT auth_type FROM system.users WHERE name = 'user_05366';

DROP USER user_05366;
