/*jshint globalstrict:false, strict:false */
/*global assertEqual, assertNotEqual, assertTrue, getOptions*/

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2019, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

if (getOptions === true) {
  return {};
}

const jsunity = require("jsunity");
const arangodb = require("@arangodb");
const db = arangodb.db;
const errors = arangodb.errors;

function MaxDatabasesTestSuite () {
  return {
    tearDown: function () {
      db._databases().filter((d) => d !== '_system').forEach((d) => {
        db._dropDatabase(d);
      });
    },

    testMaxDatabases: function () {
      const n = "testDatabase";

      // "_system" must be there
      assertEqual(1, db._databases().length);

      for (let i = 0; i < 10; ++i) {
        // should just work
        db._createDatabase(n + i);
        assertNotEqual(-1, db._databases().indexOf(n + i));
      }
    },

  };
}

jsunity.run(MaxDatabasesTestSuite);

return jsunity.done();
