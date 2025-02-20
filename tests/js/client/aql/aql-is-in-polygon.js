/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertTrue, assertFalse */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for optimizer rules
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
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
/// @author Florian Bartels
/// @author Copyright 2014, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");
const db = require('internal').db;

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function isInPolygonSuite () {

  return {

////////////////////////////////////////////////////////////////////////////////
/// @brief test WITHIN_RECTANGLE as result
////////////////////////////////////////////////////////////////////////////////

    testIsInPolygonSuiteAsResult : function () {
      assertTrue(db._query("RETURN IS_IN_POLYGON ([[ 0, 0 ], [ 0, 10 ], [ 10, 10 ], [ 10, 0 ] ], 9, 9)").toArray()[0]);
    },

    testIsNotInPolygonSuiteAsResult : function () {
      assertFalse(db._query("RETURN IS_IN_POLYGON ([[ 0, 0 ], [ 0, 10 ], [ 10, 10 ], [ 10, 0 ] ], 11, 11)").toArray()[0]);
    },

    testIsInPolygonSuiteWithListParameter : function () {
      assertTrue(db._query("RETURN IS_IN_POLYGON ([[ 0, 0 ], [ 0, 10 ], [ 10, 10 ], [ 11, 0 ] ], [ 9, 1 ])").toArray()[0]);
    },

    testIsInPolygonSuiteWithListParameterWithReverseParameterOrder : function () {
      assertFalse(db._query("RETURN IS_IN_POLYGON ([[ 0, 0 ], [ 0, 10 ], [ 10, 10 ], [ 11, 0 ] ], [ 1, 11 ], false)").toArray()[0]);
    }
    
  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(isInPolygonSuite);

return jsunity.done();

