# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""Pytest wiring for the ADBC Driver Foundry validation suite."""

import os

import adbc_drivers_validation.model
import adbc_drivers_validation.tests.conftest
import pytest
from adbc_drivers_validation.tests.conftest import (  # noqa: F401
    conn,
    conn_factory,
    db_kwargs,
    manual_test,
    noci,
    pytest_collection_modifyitems,
)

import quirks

# The suite reads the connection string out of the environment via
# model.FromEnv; default it here so the documented command is a one-liner.
for key, value in quirks.default_environment().items():
    os.environ.setdefault(key, value)


def pytest_addoption(parser: pytest.Parser) -> None:
    adbc_drivers_validation.tests.conftest.pytest_addoption(parser)
    parser.addoption("--vendor-version", action="store", default="odbc_sqlite")


@pytest.fixture(scope="session")
def driver(
    request: pytest.FixtureRequest, pytestconfig: pytest.Config
) -> adbc_drivers_validation.model.DriverQuirks:
    return quirks.get_quirks(pytestconfig.getoption("vendor_version"))


@pytest.fixture(scope="session")
def driver_path(driver: adbc_drivers_validation.model.DriverQuirks) -> str:
    return quirks.driver_path()
