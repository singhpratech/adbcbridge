-- Licensed to the Apache Software Foundation (ASF) under one
-- or more contributor license agreements.  See the NOTICE file
-- distributed with this work for additional information
-- regarding copyright ownership.  The ASF licenses this file
-- to you under the Apache License, Version 2.0 (the
-- "License"); you may not use this file except in compliance
-- with the License.  You may obtain a copy of the License at
--
--   http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing,
-- software distributed under the License is distributed on an
-- "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
-- KIND, either express or implied.  See the License for the
-- specific language governing permissions and limitations
-- under the License.
--
-- SQLite override: SQLite has no typed datetime literals
-- (DATE 'x' / TIME 'x' / TIMESTAMP 'x'), so the same values are
-- inserted as plain string literals.  The declared column type is
-- unchanged, which is what SQLiteODBC reports through SQLDescribeCol.

CREATE TABLE test_time (
    idx INTEGER,
    res TIME
);

INSERT INTO test_time (idx, res) VALUES (1, '13:45:31.123456');
INSERT INTO test_time (idx, res) VALUES (2, '00:00:00');
INSERT INTO test_time (idx, res) VALUES (3, '23:59:59.999999');
INSERT INTO test_time (idx, res) VALUES (4, '12:30:45.500');
INSERT INTO test_time (idx, res) VALUES (5, NULL);
