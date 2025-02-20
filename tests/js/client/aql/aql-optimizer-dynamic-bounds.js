/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertEqual, assertNotEqual, assertTrue, arango */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for optimizer rules
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

const jsunity = require("jsunity");
const helper = require("@arangodb/aql-helper");
const isEqual = helper.isEqual;
const db = require("@arangodb").db;
const _ = require("lodash");
const executeAllJson = require("@arangodb/aql-helper").executeAllJson;

const ruleName = "use-indexes";
const cn = "UnitTestsAhuacatlRange";

// various choices to control the optimizer: 
const paramEnabled  = { optimizer: { rules: [ "-all", "+" + ruleName ] } };
const paramDisabled = { optimizer: { rules: [ "+all", "-" + ruleName ] } };

function singleAttributeTestSuite () {
  return {
    setUpAll : function () {
      db._drop(cn);
      let c = db._create(cn);

      let docs = [];
      for (let i = 0; i < 100; ++i) {
        docs.push({ value1: "test" + i, value2: i });
      }
      c.insert(docs);

      c.ensureIndex({ type: "skiplist", fields: ["value1"] });
      c.ensureIndex({ type: "skiplist", fields: ["value2"] });
    },

    tearDownAll : function () {
      db._drop(cn);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test results
////////////////////////////////////////////////////////////////////////////////

    testRangesSingleAttribute : function () {
      const queries = [ 
        [ "FOR i IN [ 2 ] FOR j IN " + cn + " FILTER j.value2 == i SORT j.value2 RETURN j.value2", [ 2 ], true ],
        [ "FOR i IN [ 2 ] FOR j IN " + cn + " FILTER i == j.value2 SORT j.value2 RETURN j.value2", [ 2 ], true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER i == j.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ], true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 == i SORT j.value2 RETURN j.value2", [ 2, 3 ], true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 >= i FILTER j.value2 <= i SORT j.value2 RETURN j.value2", [ 2, 3 ] , true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER i <= j.value2 FILTER i >= j.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ] , true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 >= i FILTER j.value2 < i + 1 SORT j.value2 RETURN j.value2", [ 2, 3 ], true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 >= i FILTER j.value2 <= i + 1 SORT j.value2 RETURN j.value2", [ 2, 3, 3, 4 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 == 2 FOR j IN " + cn + " FILTER i.value2 == j.value2 RETURN j.value2", [ 2 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 == 2 FOR j IN " + cn + " FILTER j.value2 == i.value2 RETURN j.value2", [ 2 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 == 2 || i.value2 == 3 FOR j IN " + cn + " FILTER j.value2 == i.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 == i.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER i.value2 == j.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 >= i.value2 FILTER j.value2 <= i.value2 + 1 SORT j.value2 RETURN j.value2", [ 2, 3, 3, 4 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER i.value2 <= j.value2 FILTER i.value2 + 1 >= j.value2 SORT j.value2 RETURN j.value2", [ 2, 3, 3, 4 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 IN [ i.value2 ] SORT j.value2 RETURN j.value2", [ 2, 3 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 >= 97 FOR j IN " + cn + " FILTER j.value2 == i.value2 SORT j.value2 RETURN j.value2", [ 97, 98, 99 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 >= 97 FOR j IN " + cn + " FILTER j.value2 IN [ i.value2 ] SORT j.value2 RETURN j.value2", [ 97, 98, 99 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 >= 97 FOR j IN " + cn + " FILTER j.value2 >= i.value2 SORT j.value2 RETURN j.value2", [ 97, 98, 98, 99, 99, 99 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value2 >= 97 FOR j IN " + cn + " FILTER i.value2 <= j.value2 SORT j.value2 RETURN j.value2", [ 97, 98, 98, 99, 99, 99 ], true ]
      ];

      const opts = _.clone(paramEnabled);
      opts.allPlans = true;
      opts.verbosePlans = true;

      queries.forEach(function(query) {
        const resultDisabled = db._query(query[0], {}, paramDisabled).toArray();
        const resultEnabled  = db._query(query[0], {}, paramEnabled).toArray();

        if (query[2]) {
          assertNotEqual(-1, db._createStatement({query: query[0], bindvars: {}, options: paramEnabled}).explain().plan.rules.indexOf(ruleName), query[0]);
        } else {
          assertEqual(-1, db._createStatement({query: query[0], bindvars: {}, options: paramEnabled}).explain().plan.rules.indexOf(ruleName), query[0]);
        }

        assertTrue(isEqual(query[1], resultDisabled), query[0]);
        assertTrue(isEqual(query[1], resultEnabled), query[0]);

        const plans = db._createStatement({query: query[0], bindVars: {}, options: opts}).explain().plans;
        executeAllJson(plans, query[1], query[0]);
      });
    }

  };
}

function nonIndexedAttributeTestSuite () {
  return {
    setUp : function () {
      db._drop(cn);
      let c = db._create(cn);

      let docs = [];
      for (let i = 0; i < 100; ++i) {
        docs.push({ value1: i * 10, value2: i });
      }
      c.insert(docs);

      c.ensureIndex({ type: "skiplist", fields: ["value1", "value2"] });
    },

    tearDown : function () {
      db._drop(cn);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test results
////////////////////////////////////////////////////////////////////////////////

    testRangesNonIndexed : function () {
      const queries = [ 
        [ "FOR i IN [ 2 ] FOR j IN " + cn + " FILTER j.value2 == i SORT j.value2 RETURN j.value2", [ 2 ] ],
        [ "FOR i IN [ 2 ] FOR j IN " + cn + " FILTER i == j.value2 SORT j.value2 RETURN j.value2", [ 2 ] ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER i == j.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 == i SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 >= i FILTER j.value2 <= i SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER i <= j.value2 FILTER i >= j.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 >= i FILTER j.value2 < i + 1 SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 >= i FILTER j.value2 <= i + 1 SORT j.value2 RETURN j.value2", [ 2, 3, 3, 4 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 == 2 FOR j IN " + cn + " FILTER i.value2 == j.value2 RETURN j.value2", [ 2 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 == 2 FOR j IN " + cn + " FILTER j.value2 == i.value2 RETURN j.value2", [ 2 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 == 2 || i.value2 == 3 FOR j IN " + cn + " FILTER j.value2 == i.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 == i.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER i.value2 == j.value2 SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 >= i.value2 FILTER j.value2 <= i.value2 + 1 SORT j.value2 RETURN j.value2", [ 2, 3, 3, 4 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER i.value2 <= j.value2 FILTER i.value2 + 1 >= j.value2 SORT j.value2 RETURN j.value2", [ 2, 3, 3, 4 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value2 IN [ i.value2 ] SORT j.value2 RETURN j.value2", [ 2, 3 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 >= 97 FOR j IN " + cn + " FILTER j.value2 IN [ i.value2 ] SORT j.value2 RETURN j.value2", [ 97, 98, 99 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 >= 97 FOR j IN " + cn + " FILTER j.value2 >= i.value2 SORT j.value2 RETURN j.value2", [ 97, 98, 98, 99, 99, 99 ] ],
        [ "FOR i IN " + cn + " FILTER i.value2 >= 97 FOR j IN " + cn + " FILTER i.value2 <= j.value2 SORT j.value2 RETURN j.value2", [ 97, 98, 98, 99, 99, 99 ] ]
      ];

      const opts = _.clone(paramEnabled);
      opts.allPlans = true;
      opts.verbosePlans = true;

      queries.forEach(function(query) {
        const resultDisabled = db._query(query[0], {}, paramDisabled).toArray();
        const resultEnabled  = db._query(query[0], {}, paramEnabled).toArray();

        assertEqual(-1, db._createStatement({query: query[0], bindvars: {}, options: paramEnabled}).explain().plan.rules.indexOf(ruleName), query[0]);

        assertTrue(isEqual(query[1], resultDisabled), query[0]);
        assertTrue(isEqual(query[1], resultEnabled), query[0]);

        const plans = db._createStatement({query: query[0], bindVars: {}, options: opts}).explain().plans;
        executeAllJson(plans, query[1], query[0]);
      });
    }

  };
}

function nestedAttributeTestSuite () {
  return {
    setUp : function () {
      db._drop(cn);
      let c = db._create(cn);

      let docs = [];
      for (let i = 0; i < 100; ++i) {
        docs.push({ value1: { value2: i } });
      }
      c.insert(docs);

      c.ensureIndex({ type: "skiplist", fields: ["value1.value2"] });
    },

    tearDown : function () {
      db._drop(cn);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test results
////////////////////////////////////////////////////////////////////////////////

    testRangesNested : function () {
      const queries = [ 
        [ "FOR i IN [ 2 ] FOR j IN " + cn + " FILTER j.value1.value2 == i SORT j.value1.value2 RETURN j.value1.value2", [ 2 ], true ],
        [ "FOR i IN [ 2 ] FOR j IN " + cn + " FILTER i == j.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 2 ], true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER i == j.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ], true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value1.value2 == i SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ], true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value1.value2 >= i FILTER j.value1.value2 <= i SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ] , true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER i <= j.value1.value2 FILTER i >= j.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ] , true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value1.value2 >= i FILTER j.value1.value2 < i + 1 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ], true ],
        [ "FOR i IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value1.value2 >= i FILTER j.value1.value2 <= i + 1 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3, 3, 4 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 == 2 FOR j IN " + cn + " FILTER i.value1.value2 == j.value1.value2 RETURN j.value1.value2", [ 2 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 == 2 FOR j IN " + cn + " FILTER j.value1.value2 == i.value1.value2 RETURN j.value1.value2", [ 2 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 == 2 || i.value1.value2 == 3 FOR j IN " + cn + " FILTER j.value1.value2 == i.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value1.value2 == i.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER i.value1.value2 == j.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value1.value2 >= i.value1.value2 FILTER j.value1.value2 <= i.value1.value2 + 1 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3, 3, 4 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER i.value1.value2 <= j.value1.value2 FILTER i.value1.value2 + 1 >= j.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3, 3, 4 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 IN [ 2, 3 ] FOR j IN " + cn + " FILTER j.value1.value2 IN [ i.value1.value2 ] SORT j.value1.value2 RETURN j.value1.value2", [ 2, 3 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 >= 97 FOR j IN " + cn + " FILTER j.value1.value2 IN [ i.value1.value2 ] SORT j.value1.value2 RETURN j.value1.value2", [ 97, 98, 99 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 >= 97 FOR j IN " + cn + " FILTER j.value1.value2 >= i.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 97, 98, 98, 99, 99, 99 ], true ],
        [ "FOR i IN " + cn + " FILTER i.value1.value2 >= 97 FOR j IN " + cn + " FILTER i.value1.value2 <= j.value1.value2 SORT j.value1.value2 RETURN j.value1.value2", [ 97, 98, 98, 99, 99, 99 ], true ]
      ];

      const opts = _.clone(paramEnabled);
      opts.allPlans = true;
      opts.verbosePlans = true;

      queries.forEach(function(query) {
        const resultDisabled = db._query(query[0], {}, paramDisabled).toArray();
        const resultEnabled  = db._query(query[0], {}, paramEnabled).toArray();

        if (query[2]) {
          assertNotEqual(-1, db._createStatement({query: query[0], bindvars: {}, options: paramEnabled}).explain().plan.rules.indexOf(ruleName), query[0]);
        } else {
          assertEqual(-1, db._createStatement({query: query[0], bindvars: {}, options: paramEnabled}).explain().plan.rules.indexOf(ruleName), query[0]);
        }

        assertTrue(isEqual(query[1], resultDisabled), query[0]);
        assertTrue(isEqual(query[1], resultEnabled), query[0]);

        const plans = db._createStatement({query: query[0], bindVars: {}, options: opts}).explain().plans;
        executeAllJson(plans, query[1], query[0]);
      });
    }

  };
}

jsunity.run(singleAttributeTestSuite);
jsunity.run(nonIndexedAttributeTestSuite);
jsunity.run(nestedAttributeTestSuite);

return jsunity.done();
