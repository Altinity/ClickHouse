--- aes_encrypt_mysql(string, key, block_mode[, init_vector, AAD])
-- The MySQL-compatitable encryption, only ecb, cbc, cfb1, cfb8, cfb128 and ofb modes are supported,
-- just like for MySQL
-- https://dev.mysql.com/doc/refman/8.0/en/encryption-functions.html#function_aes-encrypt
-- https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_block_encryption_mode
-- Please note that for keys that exceed mode-specific length, keys are folded in a MySQL-specific way,
-- meaning that whole key is used, but effective key length is still determined by mode.
-- when key doesn't exceed the default mode length, ecryption result equals with AES_encypt()

-- error cases
SELECT aes_encrypt_mysql(); --{serverError 42} not enough arguments
SELECT aes_encrypt_mysql('text', 456, 'aes-128-ecb'); --{serverError 43} bad key type
SELECT aes_encrypt_mysql('text', 'key', 789); --{serverError 43} bad mode type
SELECT aes_encrypt_mysql('text', 'key', 'aes-128-ecb', 1011); --{serverError 43} bad mode IV type
SELECT aes_encrypt_mysql('text', 'key', 'aes-128-ecb', 'IV', 1213); --{serverError 43} bad mode AAD type

SELECT aes_encrypt_mysql('text', 'key', 'des-ede3-ecb'); -- {serverError 36} bad mode value
SELECT aes_encrypt_mysql('text', 'key', 'aes-128-ecb'); -- {serverError 36} bad key length
SELECT aes_encrypt('text', 'keykeykeykeykeyk', 'aes-128-gcm'); -- {serverError 454} zero IV length
SELECT aes_encrypt_mysql('text', 'key', 'aes-128-gcm'); -- {serverError 36} mode is not supported by MySQL
SELECT aes_encrypt('text', 'key', 'aes-128-ccm'); -- {serverError 36} mode is not supported

SELECT 'UInt64';
SELECT hex(aes_encrypt_mysql(123456789101112, 'keykeykeykeykeykeykeykeykeykeyke', 'aes-128-ecb'));
SELECT 'Float64';
SELECT hex(aes_encrypt_mysql(1234567891011.12, 'keykeykeykeykeykeykeykeykeykeyke', 'aes-128-ecb'));
SELECT 'Decimal64';
SELECT hex(aes_encrypt_mysql(toDecimal64(1234567891011.12, 2), 'keykeykeykeykeykeykeykeykeykeyke', 'aes-128-ecb'));

CREATE TABLE encryption_test
(
    input String,
    key String DEFAULT unhex('fb9958e2e897ef3fdb49067b51a24af645b3626eed2f9ea1dc7fd4dd71b7e38f9a68db2a3184f952382c783785f9d77bf923577108a88adaacae5c141b1576b0'),
    iv String DEFAULT unhex('8CA3554377DFF8A369BC50A89780DD85')
) Engine = Memory;

INSERT INTO encryption_test (input)
VALUES (''), ('text'), ('What Is ClickHouse? ClickHouse is a column-oriented database management system (DBMS) for online analytical processing of queries (OLAP).');

SELECT 'MySQL-specific key folding';
SELECT 'aes-128-ecb' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-192-ecb' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-256-ecb' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;

SELECT 'aes-128-cbc' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-192-cbc' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-256-cbc' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;

SELECT 'aes-128-cfb1' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-192-cfb1' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-256-cfb1' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;

SELECT 'aes-128-cfb8' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-192-cfb8' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-256-cfb8' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;

SELECT 'aes-128-cfb128' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-192-cfb128' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-256-cfb128' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;

SELECT 'aes-128-ofb' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-192-ofb' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;
SELECT 'aes-256-ofb' as mode, hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test;

SELECT 'Nullable and LowCardinality';
WITH CAST(NULL as Nullable(String)) as input, 'aes-256-ofb' as mode SELECT toTypeName(input), hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test LIMIT 1;
WITH CAST('text' as Nullable(String)) as input, 'aes-256-ofb' as mode SELECT toTypeName(input), hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test LIMIT 1;
WITH CAST('text' as LowCardinality(String)) as input, 'aes-256-ofb' as mode SELECT toTypeName(input), hex(aes_encrypt_mysql(input, key, mode, iv)) FROM encryption_test LIMIT 1;

SELECT 'GCM mode with IV';
SELECT 'aes-128-gcm' as mode, hex(aes_encrypt(input, substr(key, 1, 16), mode, iv)) FROM encryption_test;
SELECT 'aes-192-gcm' as mode, hex(aes_encrypt(input, substr(key, 1, 24), mode, iv)) FROM encryption_test;
SELECT 'aes-256-gcm' as mode, hex(aes_encrypt(input, substr(key, 1, 32), mode, iv)) FROM encryption_test;

SELECT 'GCM mode with IV and AAD';
SELECT 'aes-128-gcm' as mode, hex(aes_encrypt(input, substr(key, 1, 16), mode, iv, 'AAD')) FROM encryption_test;
SELECT 'aes-192-gcm' as mode, hex(aes_encrypt(input, substr(key, 1, 24), mode, iv, 'AAD')) FROM encryption_test;
SELECT 'aes-256-gcm' as mode, hex(aes_encrypt(input, substr(key, 1, 32), mode, iv, 'AAD')) FROM encryption_test;

-- based on https://github.com/openssl/openssl/blob/master/demos/evp/aesgcm.c#L20
WITH
    unhex('eebc1f57487f51921c0465665f8ae6d1658bb26de6f8a069a3520293a572078f') as key,
    unhex('67ba0510262ae487d737ee6298f77e0c') as tag,
    unhex('99aa3e68ed8173a0eed06684') as iv,
    unhex('f56e87055bc32d0eeb31b2eacc2bf2a5') as plaintext,
    unhex('4d23c3cec334b49bdb370c437fec78de') as aad,
    unhex('f7264413a84c0e7cd536867eb9f21736') as ciphertext
SELECT
    hex(aes_encrypt(plaintext, key, 'aes-256-gcm', iv, aad)) as ciphertext_actual,
    ciphertext_actual = concat(hex(ciphertext), hex(tag));
