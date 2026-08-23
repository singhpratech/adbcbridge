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

CREATE TABLE test_date (
    idx INTEGER,
    res DATE
);

INSERT INTO test_date (idx, res) VALUES (1, '2023-05-15');
INSERT INTO test_date (idx, res) VALUES (2, '0001-01-01');
INSERT INTO test_date (idx, res) VALUES (3, '1969-07-20');
INSERT INTO test_date (idx, res) VALUES (4, '9999-12-31');
INSERT INTO test_date (idx, res) VALUES (5, NULL);
