SET allow_experimental_object_type = 1;
DROP TABLE IF EXISTS t_json_complex;
CREATE TABLE t_json_complex (id UInt32, arr Array(Object('json')))
ENGINE = MergeTree ORDER BY id;

-- Insert data with nested arrays inside JSON objects
INSERT INTO t_json_complex FORMAT JSONEachRow {"id": 1, "arr": [{"k1": [{"k2": "aaa", "k3": "bbb"}, {"k2": "ccc"}]}]}

INSERT INTO t_json_complex FORMAT JSONEachRow {"id": 2, "arr": [{"k1": [{"k3": "ddd", "k4": 10}, {"k4": 20}], "k5": {"k6": "foo"}}]}

-- This query used to crash the server
SELECT id, arr.k1.k2, arr.k1.k3, arr.k1.k4, arr.k5.k6 FROM t_json_complex ORDER BY id;
