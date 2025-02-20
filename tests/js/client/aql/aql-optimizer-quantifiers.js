/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertEqual, assertTrue, assertFalse */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for ANY|ALL|NONE
///
/// @file
///
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
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");
var db = require("@arangodb").db;

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function optimizerQuantifiersTestSuite () {
  var c;

  return {
    setUpAll : function () {
      db._drop("UnitTestsCollection");
      c = db._create("UnitTestsCollection");

      for (var i = 0; i < 10; ++i) {
        c.insert({ value: i % 5 });
      }
    },

    tearDownAll : function () {
      db._drop("UnitTestsCollection");
    },
    
    testAllEmpty : function () {
      var query = "[] ALL == '1'", result;
      
      result = db._query("RETURN (" + query + ")").toArray()[0];
      assertTrue(result);

      result = db._query("RETURN NOOPT(" + query + ")").toArray()[0];
      assertTrue(result);
    },
    
    testAnyEmpty : function () {
      var query = "[] ANY == '1'", result;
      
      result = db._query("RETURN (" + query + ")").toArray()[0];
      assertFalse(result);

      result = db._query("RETURN NOOPT(" + query + ")").toArray()[0];
      assertFalse(result);
    },

    testNoneEmpty : function () {
      var query = "[] NONE == '1'", result;
      
      result = db._query("RETURN (" + query + ")").toArray()[0];
      assertTrue(result);

      result = db._query("RETURN NOOPT(" + query + ")").toArray()[0];
      assertTrue(result);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ALL IN
////////////////////////////////////////////////////////////////////////////////

    testAllIn : function () {
      var queries = [
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ALL IN [ doc.value ] SORT doc.value RETURN doc.value", [ ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ALL NOT IN [ doc.value ] SORT doc.value RETURN doc.value", [ 0, 0, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ALL == doc.value SORT doc.value RETURN doc.value", [ ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ALL != doc.value SORT doc.value RETURN doc.value", [ 0, 0, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ALL > doc.value SORT doc.value RETURN doc.value", [ 0, 0 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ALL >= doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ALL < doc.value SORT doc.value RETURN doc.value", [ 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ALL <= doc.value SORT doc.value RETURN doc.value", [ 3, 3, 4, 4 ] ]
      ];

      queries.forEach(function(query) {
        var result = db._query(query[0]).toArray();
        assertEqual(query[1], result);
      });
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ANY IN
////////////////////////////////////////////////////////////////////////////////

    testAnyIn : function () {
      var queries = [
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ANY IN [ doc.value ] SORT doc.value RETURN doc.value", [ 1, 1, 2, 2, 3, 3 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ANY NOT IN [ doc.value ] SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2, 3, 3, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ANY == doc.value SORT doc.value RETURN doc.value", [ 1, 1, 2, 2, 3, 3 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ANY != doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2, 3, 3, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ANY > doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ANY >= doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2, 3, 3 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ANY < doc.value SORT doc.value RETURN doc.value", [ 2, 2, 3, 3, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] ANY <= doc.value SORT doc.value RETURN doc.value", [ 1, 1, 2, 2, 3, 3, 4, 4 ] ]
      ];

      queries.forEach(function(query) {
        var result = db._query(query[0]).toArray();
        assertEqual(query[1], result);
      });
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test NONE IN
////////////////////////////////////////////////////////////////////////////////

    testNoneIn : function () {
      var queries = [
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] NONE IN [ doc.value ] SORT doc.value RETURN doc.value", [ 0, 0, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] NONE NOT IN [ doc.value ] SORT doc.value RETURN doc.value", [ ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] NONE == doc.value SORT doc.value RETURN doc.value", [ 0, 0, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] NONE != doc.value SORT doc.value RETURN doc.value", [ ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] NONE > doc.value SORT doc.value RETURN doc.value", [ 3, 3, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] NONE >= doc.value SORT doc.value RETURN doc.value", [ 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] NONE < doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] NONE <= doc.value SORT doc.value RETURN doc.value", [ 0, 0 ] ]
      ];

      queries.forEach(function(query) {
        var result = db._query(query[0]).toArray();
        assertEqual(query[1], result);
      });
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test AT LEAST IN
////////////////////////////////////////////////////////////////////////////////

    testAtLeastIn : function () {
      var queries = [
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (1) IN [ doc.value ] SORT doc.value RETURN doc.value", [ 1, 1, 2, 2, 3, 3 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (1) == doc.value SORT doc.value RETURN doc.value", [ 1, 1, 2, 2, 3, 3 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (1) != doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2, 3, 3, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (1) > doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (1) >= doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2, 3, 3 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (1) < doc.value SORT doc.value RETURN doc.value", [ 2, 2, 3, 3, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (1) <= doc.value SORT doc.value RETURN doc.value", [ 1, 1, 2, 2, 3, 3, 4, 4 ] ],
        
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (2) IN [ doc.value ] SORT doc.value RETURN doc.value", [ ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (2) == doc.value SORT doc.value RETURN doc.value", [ ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (2) != doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2, 3, 3, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (2) > doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (2) >= doc.value SORT doc.value RETURN doc.value", [ 0, 0, 1, 1, 2, 2 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (2) < doc.value SORT doc.value RETURN doc.value", [ 3, 3, 4, 4 ] ],
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST (2) <= doc.value SORT doc.value RETURN doc.value", [ 2, 2, 3, 3, 4, 4 ] ],
      ];

      queries.forEach(function(query) {
        var result = db._query(query[0]).toArray();
        assertEqual(query[1], result, query);
      });
    },

   testAtLeastInFail : function () {
      var queries = [
        // missing parentheses
        [ "FOR doc IN " + c.name() + " FILTER [ 1, 2, 3 ] AT LEAST 2 <= doc.value SORT doc.value RETURN doc.value", [ 2, 2, 3, 3, 4, 4 ] ],
      ];

      queries.forEach(function(query) {
        try {
          db._query(query[0]).toArray();
          assertTrue(false);
        } catch (e) {
          // Expect parse error
          assertEqual(1501, e.errorNum);
        }
      });
   },


  };
}
jsunity.run(optimizerQuantifiersTestSuite);

return jsunity.done();

