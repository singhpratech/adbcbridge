// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Text;

namespace AdbcBridge
{
    /// <summary>
    /// Thrown by <see cref="Driver.Path"/> (and so by <see cref="Driver.Load"/>
    /// and <see cref="Driver.Connect"/>) when the adbcbridge shared library
    /// cannot be located. <see cref="Searched"/> lists every place that was
    /// tried, in order, and the message repeats it.
    /// </summary>
    public sealed class DriverNotFoundException : Exception
    {
        /// <summary>Every location that was examined, in lookup order.</summary>
        public IReadOnlyList<string> Searched { get; }

        /// <summary>The file name that was looked for (libadbc_driver_odbc.so, .dylib or .dll).</summary>
        public string LibraryName { get; }

        internal DriverNotFoundException(string libraryName, IReadOnlyList<string> searched, string? reason)
            : base(Compose(libraryName, searched, reason))
        {
            LibraryName = libraryName;
            Searched = searched;
        }

        private static string Compose(string libraryName, IReadOnlyList<string> searched, string? reason)
        {
            StringBuilder text = new StringBuilder();
            if (reason is null)
            {
                text.Append("could not find ").Append(libraryName).Append(", the adbcbridge driver.");
            }
            else
            {
                text.Append(reason);
            }

            text.AppendLine().AppendLine("Places searched, in order:");
            foreach (string place in searched)
            {
                text.Append("  ").AppendLine(place);
            }

            text.Append("Set ").Append(Driver.LibraryVariable).Append(" or ").Append(Driver.DriverVariable)
                .Append(" to the library's path, install the driver (./install.sh or cmake --install), ")
                .Append("or reference a build of the AdbcBridge package that ships the native library for ")
                .Append(Driver.RuntimeIdentifier()).Append('.');
            return text.ToString();
        }
    }
}
