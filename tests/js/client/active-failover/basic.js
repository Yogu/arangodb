/*jshint strict: false, sub: true */
/*global print, assertTrue, assertEqual, assertNotEqual, fail */
'use strict';

////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

const jsunity = require('jsunity');
const internal = require('internal');
const console = require('console');
const expect = require('chai').expect;
const arangosh = require('@arangodb/arangosh');
const crypto = require('@arangodb/crypto');
const request = require("@arangodb/request");
const tasks = require("@arangodb/tasks");
const fs = require('fs');
const path = require('path');
const utils = require('@arangodb/foxx/manager-utils');
const arango = internal.arango;
const db = internal.db;
const compareTicks = require("@arangodb/replication").compareTicks;

const jwtSecret = 'haxxmann';
const jwtSuperuser = crypto.jwtEncode(jwtSecret, {
  "server_id": "test",
  "iss": "arangodb",
  "exp": Math.floor(Date.now() / 1000) + 3600
}, 'HS256');
const jwtRoot = crypto.jwtEncode(jwtSecret, {
  "preferred_username": "root",
  "iss": "arangodb",
  "exp": Math.floor(Date.now() / 1000) + 3600
}, 'HS256');

const cname = "UnitTestActiveFailover";

function getUrl(endpoint) {
  return endpoint.replace(/^tcp:/, 'http:').replace(/^ssl:/, 'https:');
}

function baseUrl() {
  return getUrl(arango.getEndpoint());
}

function connectToServer(leader) {
  arango.reconnect(leader, "_system", "root", "");
  db._flushCache();
}

// getEndponts works with any server
function getClusterEndpoints() {
  //let jwt = crypto.jwtEncode(options['server.jwt-secret'], {'server_id': 'none', 'iss': 'arangodb'}, 'HS256');
  let tries = 60;
  let res;
  while (tries-- > 0) {
    res = request.get({
      url: baseUrl() + "/_api/cluster/endpoints",
      auth: {
        bearer: jwtRoot,
      },
      timeout: 300
    });

    if (res.statusCode === 200) {
      break;
    }
    internal.sleep(0.5);
  }
  assertTrue(res instanceof request.Response);
  assertTrue(res.hasOwnProperty('statusCode'), JSON.stringify(res));
  assertEqual(res.statusCode, 200, JSON.stringify(res));
  assertTrue(res.hasOwnProperty('json'));
  assertTrue(res.json.hasOwnProperty('endpoints'));
  assertTrue(res.json.endpoints instanceof Array);
  assertTrue(res.json.endpoints.length > 0);
  return res.json.endpoints.map(e => e.endpoint);
}

function getLoggerState(endpoint) {
  let tries = 60;
  let res;
  while (tries-- > 0) {
    res = request.get({
      url: getUrl(endpoint) + "/_db/_system/_api/replication/logger-state",
      auth: {
        bearer: jwtRoot,
      },
      timeout: 300
    });

    if (res.statusCode === 200) {
      break;
    }
    internal.sleep(0.5);
  }
  assertTrue(res instanceof request.Response);
  assertTrue(res.hasOwnProperty('statusCode'));
  assertEqual(res.statusCode, 200);
  assertTrue(res.hasOwnProperty('json'));
  return arangosh.checkRequestResult(res.json);
}

function getApplierState(endpoint) {
  let tries = 60;
  let res;
  while (tries-- > 0) {
    res = request.get({
      url: getUrl(endpoint) + "/_db/_system/_api/replication/applier-state?global=true",
      auth: {
        bearer: jwtRoot,
      },
      timeout: 300
    });

    if (res.statusCode === 200) {
      break;
    }
    internal.sleep(0.5);
  }
  assertTrue(res instanceof request.Response);
  assertTrue(res.hasOwnProperty('statusCode'));
  assertEqual(res.statusCode, 200, JSON.stringify(res));
  assertTrue(res.hasOwnProperty('json'));
  return arangosh.checkRequestResult(res.json);
}

// check the servers are in sync with the leader
function checkInSync(leader, servers, ignore) {
  print(Date() + "Checking in-sync state with lead: ", leader);

  const leaderTick = getLoggerState(leader).state.lastLogTick;

  let check = (endpoint) => {
    if (endpoint === leader || endpoint === ignore) {
      return true;
    }

    let applier = getApplierState(endpoint);

    print(Date() + "Checking endpoint ", endpoint, " applier.state.running=", applier.state.running, " applier.endpoint=", applier.endpoint);
    return applier.state.running && applier.endpoint === leader &&
      (compareTicks(applier.state.lastAppliedContinuousTick, leaderTick) >= 0 ||
        compareTicks(applier.state.lastProcessedContinuousTick, leaderTick) >= 0);
  };

  let loop = 100;
  while (loop-- > 0) {
    if (servers.every(check)) {
      print(Date() + "All followers are in sync with: ", leader);
      return true;
    }
    internal.sleep(1.0);
  }
  print(Date() + "Timeout waiting for followers of: ", leader);
  return false;
}

function checkData(server, allowDirty = false) {
  print(Date() + "Checking data of ", server);
  // Async agency cache should have received its data
  request.get({ 
    url: getUrl(server) + "/_api/cluster/agency-cache", auth: { bearer: jwtRoot },
    headers: { "X-Arango-Allow-Dirty-Read": allowDirty }, timeout: 3 
  });
  internal.sleep(2);

  let tries = 60;
  let res;
  while (tries-- > 0) {
    res = request.get({
      url: getUrl(server) + "/_api/collection/" + cname + "/count",
      auth: {
        bearer: jwtRoot,
      },
      headers: {
        "X-Arango-Allow-Dirty-Read": allowDirty
      },
      timeout: 300
    });
    if (res.statusCode === 200) {
      break;
    }
    internal.sleep(0.5);
  }

  assertTrue(res instanceof request.Response);
  assertTrue(res.hasOwnProperty('statusCode'));
  assertEqual(res.statusCode, 200, JSON.stringify(res));
  return res.json.count;
}

function readAgencyValue(path) {
  let agents = global.instanceManager.arangods.filter(arangod => arangod.instanceRole === "agent");
  assertTrue(agents.length > 0, "No agents present");
  print(Date() + "Querying agency... (", path, ")");
  let res = request.post({
    url: agents[0].url + "/_api/agency/read",
    auth: {
      bearer: jwtSuperuser,
    },
    body: JSON.stringify([[path]]),
    timeout: 300
  });
  assertTrue(res instanceof request.Response);
  assertTrue(res.hasOwnProperty('statusCode'), JSON.stringify(res));
  assertEqual(res.statusCode, 200, JSON.stringify(res));
  assertTrue(res.hasOwnProperty('json'));
  //print(Date() + "Agency response ", res.json);
  return arangosh.checkRequestResult(res.json);
}

// resolve leader from agency
function leaderInAgency() {
  let i = 10;
  do {
    let res = readAgencyValue("/arango/Plan/AsyncReplication/Leader");
    let uuid = res[0].arango.Plan.AsyncReplication.Leader;
    if (uuid && uuid.length > 0) {
      res = readAgencyValue("/arango/Supervision/Health");
      return res[0].arango.Supervision.Health[uuid].Endpoint;
    }
    internal.sleep(1.0);
  } while (i-- > 0);
  throw "Unable to resole leader from agency";
}

function checkForFailover(leader) {
  print(Date() + "Waiting for failover of ", leader);

  let oldLeaderUUID = "";
  let i = 24; // 24 * 5s == 120s
  do {
    let res = readAgencyValue("/arango/Supervision/Health");
    let srvHealth = res[0].arango.Supervision.Health;
    Object.keys(srvHealth).forEach(key => {
      let srv = srvHealth[key];
      if (srv['Endpoint'] === leader && srv.Status === 'FAILED') {
        print(Date() + "Server ", key, "( ", leader, " ) is marked FAILED");
        oldLeaderUUID = key;
      }
    });
    if (oldLeaderUUID !== "") {
      break;
    }
    internal.sleep(5.0);
  } while (i-- > 0);

  // now wait for new leader to appear
  let nextLeaderUUID = "";
  do {
    let res = readAgencyValue("/arango/Plan/AsyncReplication/Leader");
    nextLeaderUUID = res[0].arango.Plan.AsyncReplication.Leader;
    if (nextLeaderUUID !== oldLeaderUUID) {
      res = readAgencyValue("/arango/Supervision/Health");
      return res[0].arango.Supervision.Health[nextLeaderUUID].Endpoint;
    }
    internal.sleep(5.0);
  } while (i-- > 0);
  print(Date() + "Timing out, current leader value: ", nextLeaderUUID);
  throw "No failover occured";
}

function waitUntilHealthStatusIs(isHealthy, isFailed) {
  print(Date() + "Waiting for health status to be healthy: ", JSON.stringify(isHealthy), " failed: ", JSON.stringify(isFailed));
  // Wait 25 seconds, sleep 5 each run
  for (const start = Date.now(); (Date.now() - start) / 1000 < 25; internal.sleep(5.0)) {
    let needToWait = false;
    let res = readAgencyValue("/arango/Supervision/Health");
    let srvHealth = res[0].arango.Supervision.Health;
    let foundFailed = 0;
    let foundHealthy = 0;
    for (const [_, srv] of Object.entries(srvHealth)) {
      if (srv.Status === 'FAILED') {
        // We have a FAILED server, that we do not expect
        if (!isFailed.indexOf(srv.Endpoint) === -1) {
          needToWait = true;
          break;
        }
        foundFailed++;
      } else {
        if (!isHealthy.indexOf(srv.Endpoint) === -1) {
          needToWait = true;
          break;
        }
        foundHealthy++;
      }
    }
    if (!needToWait && foundHealthy === isHealthy.length && foundFailed === isFailed.length) {
      return true;
    }
  }
  print(Date() + "Timing out, could not reach desired state: ", JSON.stringify(isHealthy), " failed: ", JSON.stringify(isFailed));
  print(Date() + "We only got: ", JSON.stringify(readAgencyValue("/arango/Supervision/Health")[0].arango.Supervision.Health));
  return false;
}

function loadFoxxIntoZip(path) {
  let zip = utils.zipDirectory(path);
  let content = fs.readFileSync(zip);
  fs.remove(zip);
  return {
    type: 'inlinezip',
    buffer: content
  };
}
function checkFoxxService(readOnly) {
  const onlyJson = {
    'accept': 'application/json',
    'accept-content-type': 'application/json'
  };
  let reply;
  db._useDatabase("_system");

  [
    '/_db/_system/_admin/aardvark/index.html',
    '/_db/_system/itz/index',
    '/_db/_system/crud/xxx'
  ].forEach(route => {
    for (let i=0; i < 200; i++) {
      try {
        reply = arango.GET_RAW(route, onlyJson);
        if (reply.code === 200) {
          print(Date() + " " + route + " OK");
          return;
        }
        let msg = JSON.stringify(reply);
        if (reply.hasOwnProperty('parsedBody')) {
          msg = " '" + reply.parsedBody.errorNum + "' - " + reply.parsedBody.errorMessage;
        }
        print(Date() + " " + route + " Not yet ready, retrying: " + msg);
      } catch (e) {
        print(Date() + " " + route + " Caught - need to retry. " + JSON.stringify(e));
      }
      internal.sleep(3);
    }
    throw ("foxx route '" + route + "' not ready on time!");
  });

  print(Date() + "Foxx: Itzpapalotl getting the root of the gods");
  reply = arango.GET_RAW('/_db/_system/itz');
  assertEqual(reply.code, "307", JSON.stringify(reply));

  print(Date() + 'Foxx: Itzpapalotl getting index html with list of gods');
  reply = arango.GET_RAW('/_db/_system/itz/index');
  assertEqual(reply.code, "200", JSON.stringify(reply));

  print(Date() + "Foxx: Itzpapalotl summoning Chalchihuitlicue");
  reply = arango.GET_RAW('/_db/_system/itz/Chalchihuitlicue/summon', onlyJson);
  assertEqual(reply.code, "200", JSON.stringify(reply));
  let parsedBody = JSON.parse(reply.body);
  assertEqual(parsedBody.name, "Chalchihuitlicue");
  assertTrue(parsedBody.summoned);

  print(Date() + "Foxx: crud testing get xxx");
  reply = arango.GET_RAW('/_db/_system/crud/xxx', onlyJson);
  assertEqual(reply.code, "200");
  parsedBody = JSON.parse(reply.body);
  assertEqual(parsedBody, []);

  print(Date() + "Foxx: crud testing POST xxx");

  reply = arango.POST_RAW('/_db/_system/crud/xxx', {_key: "test"});
  if (readOnly) {
    assertEqual(reply.code, "400");
  } else {
    assertEqual(reply.code, "201");
  }

  print(Date() + "Foxx: crud testing get xxx");
  reply = arango.GET_RAW('/_db/_system/crud/xxx', onlyJson);
  assertEqual(reply.code, "200");
  parsedBody = JSON.parse(reply.body);
  if (readOnly) {
    assertEqual(parsedBody, []);
  } else {
    assertEqual(parsedBody.length, 1);
  }

  print(Date() + 'Foxx: crud testing delete document');
  reply = arango.DELETE_RAW('/_db/_system/crud/xxx/' + 'test');
  if (readOnly) {
    assertEqual(reply.code, "400");
  } else {
    assertEqual(reply.code, "204");
  }
}

function installFoxx(mountpoint, which, mode) {
  let headers = {};
  let content;
  if (which.type === 'js') {
    headers['content-type'] = 'application/javascript';
    content = which.buffer;
  } else if (which.type === 'dir') {
    headers['content-type'] = 'application/zip';
    var utils = require('@arangodb/foxx/manager-utils');
    let zip = utils.zipDirectory(which.buffer);
    content = fs.readFileSync(zip);
    fs.remove(zip);
  } else if (which.type === 'inlinezip') {
    content = which.buffer;
    headers['content-type'] = 'application/zip';
  } else if (which.type === 'url') {
    content = { source: which };
  } else if (which.type === 'file') {
    content = fs.readFileSync(which.buffer);
  }
  let devmode = '';
  if (typeof which.devmode === "boolean") {
    devmode = `&development=${which.devmode}`;
  }
  let crudResp;
  if (mode === "upgrade") {
    crudResp = arango.PATCH('/_api/foxx/service?mount=' + mountpoint + devmode, content, headers);
  } else if (mode === "replace") {
    crudResp = arango.PUT('/_api/foxx/service?mount=' + mountpoint + devmode, content, headers);
  } else {
    crudResp = arango.POST('/_api/foxx?mount=' + mountpoint + devmode, content, headers);
  }
  expect(crudResp).to.have.property('manifest');
  return crudResp;
}

// Testsuite that quickly checks some of the basic premises of
// the active failover functionality. It is designed as a quicker
// variant of the node resilience tests (for active failover).
function ActiveFailoverSuite() {
  let servers;
  let firstLeader;
  let suspended = [];
  let currentLead;

  return {
    setUpAll: function () {
      servers = getClusterEndpoints();
      assertTrue(servers.length >= 4, "This test expects four single instances " + JSON.stringify(servers));
      firstLeader = servers[0];
      currentLead = leaderInAgency();
      db._create(cname);
    },

    setUp: function () {
      assertTrue(checkInSync(currentLead, servers));

      let col = db._collection(cname);
      for (let i = 0; i < 10000; i++) {
        col.save({ attr: i});
      }
    },

    tearDown: function () {
      suspended.forEach(arangod => {
        print(`${Date()} Teardown: Resuming: ${arangod.name} ${arangod.pid}`);
        assertTrue(arangod.resume());
      });

      currentLead = leaderInAgency();
      print(Date() + "connecting shell to leader ", currentLead);
      connectToServer(currentLead);

      assertTrue(checkInSync(currentLead, servers));

      let i = 100;
      do {
        let endpoints = getClusterEndpoints();
        if (endpoints.length === servers.length && endpoints[0] === currentLead) {
          db._collection(cname).truncate({ compact: false });
          return;
        }
        print(Date() + "cluster endpoints not as expected: found =", endpoints, " expected =", servers);
        internal.sleep(1); // settle down
      } while(i --> 0);

      let endpoints = getClusterEndpoints();
      print(Date() + "endpoints: ", endpoints, " servers: ", servers);
      assertEqual(endpoints.length, servers.length);
      assertEqual(endpoints[0], currentLead);
      db._collection(cname).truncate({ compact: false });
    },

    tearDownAll: function () {
      if (db._collection(cname)) {
        db._drop(cname);
      }
    },

    // Basic test if followers get in sync
    testFollowerInSync: function () {
      assertEqual(servers[0], currentLead);

      let col = db._collection(cname);
      assertEqual(col.count(), 10000);
      assertTrue(checkInSync(currentLead, servers));
      assertEqual(checkData(currentLead), 10000);
    },

    // Simple failover case: Leader is suspended, slave needs to
    // take over within a reasonable amount of time
    testFailover: function () {
      const itzpapalotlPath = path.resolve(internal.pathForTesting('common'), 'test-data', 'apps', 'itzpapalotl');
      const itzpapalotlZip = loadFoxxIntoZip(itzpapalotlPath);
      installFoxx("/itz", itzpapalotlZip);

      const minimalWorkingServicePath = path.resolve(internal.pathForTesting('common'), 'test-data', 'apps', 'crud');
      const minimalWorkingZip = loadFoxxIntoZip(minimalWorkingServicePath);
      installFoxx('/crud', minimalWorkingZip);

      checkFoxxService(false);
      assertTrue(checkInSync(currentLead, servers));
      assertEqual(checkData(currentLead), 10000);

      let suspended;
      let oldLead = currentLead;
      try {
        suspended = global.instanceManager.arangods.filter(arangod => arangod.endpoint === currentLead);
        suspended.forEach(arangod => {
          print(`${Date()} Suspending Leader: ${arangod.name} ${arangod.pid}`);
          assertTrue(arangod.suspend());
        });

        // await failover and check that follower get in sync
        currentLead = checkForFailover(currentLead);
        assertNotEqual(currentLead, oldLead);
        print(Date() + "Failover to new leader : ", currentLead);

        internal.sleep(5); // settle down, heartbeat interval is 1s
        assertEqual(checkData(currentLead), 10000);
        print(Date() + "New leader has correct data");

        // check the remaining followers get in sync
        assertTrue(checkInSync(currentLead, servers, oldLead));

        connectToServer(currentLead);
        checkFoxxService(false);

      } finally {
        // restart the old leader
        suspended.forEach(arangod => {
          print(`${Date()} Resuming: ${arangod.name} ${arangod.pid}`);
          assertTrue(arangod.resume());
        });
        assertTrue(checkInSync(currentLead, servers));
        // after its in sync, halt all others so it becomes the leader again
        suspended = global.instanceManager.arangods.filter(arangod =>
          (arangod.endpoint !== oldLead) && (arangod.instanceRole === 'activefailover'));
        suspended.forEach(arangod => {
          print(`${Date()} Suspending all but old Leader: ${arangod.name} ${arangod.pid}`);
          assertTrue(arangod.suspend());
        });
        currentLead = checkForFailover(currentLead);
        assertEqual(currentLead, oldLead);
        connectToServer(currentLead);
        // restart the other followers so the system is all up and running again
        suspended.forEach(arangod => {
          print(`${Date()} Resuming: ${arangod.name} ${arangod.pid}`);
          assertTrue(arangod.resume());
        });
        assertTrue(checkInSync(currentLead, servers));
        let stati = [];
        ["/itz", "/crud"].forEach(mount => {
          try {
            print(Date() + "Uninstalling " + mount);
            let res = arango.DELETE(
              "/_db/_system/_admin/aardvark/foxxes?teardown=true&mount=" + mount);
            stati.push(res.error);
          } catch (e) {}
        });
        assertEqual(stati, [false, false]);
      }
    },

    // More complex case: We want to get the most up to date follower
    // Insert a number of documents, suspend n-1 followers for a few seconds.
    // We then suspend the leader and expect a specific follower to take over
    testFollowerSelection: function () {

      assertTrue(checkInSync(currentLead, servers));
      assertEqual(checkData(currentLead), 10000);

      // we assume the second leader is still the leader
      let endpoints = getClusterEndpoints();
      assertEqual(endpoints.length, servers.length);
      assertEqual(endpoints[0], currentLead);

      print(Date() + "Starting data creation task on ", currentLead, " (expect it to fail later)");
      connectToServer(currentLead);
      /// this task should stop once the server becomes a slave
      var task = tasks.register({
        name: "UnitTestsFailover",
        command: `
          const db = require('@arangodb').db;
          let col = db._collection("UnitTestActiveFailover");
          let cc = col.count();
          for (let i = 0; i < 1000000; i++) {
            col.save({attr: i + cc});
          }`
      });

      internal.sleep(2.5);

      // pick a random follower
      let nextLead = endpoints[2]; // could be any one of them
      // suspend remaining followers
      print(Date() + "Suspending followers, except one");
      suspended = global.instanceManager.arangods.filter(arangod => arangod.instanceRole !== 'agent' &&
        arangod.endpoint !== currentLead &&
        arangod.endpoint !== nextLead);
      suspended.forEach(arangod => {
          print(`${Date()} Suspending: ${arangod.name} ${arangod.pid}`);
        assertTrue(arangod.suspend());
      });

      // check our leader stays intact, while remaining followers fail
      let i = 20;
      //let expected = servers.length - suspended.length; // should be 2
      do {
        endpoints = getClusterEndpoints();
        assertEqual(endpoints[0], currentLead, "Unwanted leadership failover");
        internal.sleep(1.0); // Health status may take some time to change
      } while (endpoints.length !== 2 && i-- > 0);
      assertTrue(i > 0, "timed-out waiting for followers to fail");
      assertEqual(endpoints.length, 2);
      assertEqual(endpoints[1], nextLead); // this server must become new leader

      let upper = checkData(currentLead);
      let atLeast = 0;
      while (atLeast < upper) {
        internal.sleep(1.0);
        //update atLeast
        atLeast = checkData(nextLead, true);
      }

      let healthyList = [currentLead, nextLead].concat(suspended.map(s => s.endpoint));
      // resume followers
      print(Date() + "Resuming followers");
      suspended.forEach(arangod => {
        print(`${Date()} Resuming: ${arangod.name} ${arangod.pid}`);
        assertTrue(arangod.resume());
      });
      suspended = [];

      // Wait until all servers report healthy again
      assertTrue(waitUntilHealthStatusIs(healthyList, []));

      print(Date() + "Leader inserted ", upper, " documents so far desired follower has " , atLeast);
      print(Date() + "Suspending leader ", currentLead);
      global.instanceManager.arangods.forEach(arangod => {
        if (arangod.endpoint === currentLead) {
          print(`${Date()} Suspending: ${arangod.name} ${arangod.pid}`);
          suspended.push(arangod);
          assertTrue(arangod.suspend());
        }
      });

      // await failover and check that follower get in sync
      let oldLead = currentLead;
      currentLead = checkForFailover(currentLead);
      assertNotEqual(currentLead, oldLead);

      let cc = checkData(currentLead);
      assertTrue(cc >= atLeast, "The new Leader has too few documents");
      print(Date() + "Number of documents is in acceptable range");

      assertTrue(checkInSync(currentLead, servers, oldLead));
      print(Date() + "Remaining followers are in sync");

      // Resuming stopped second leader
      print(Date() + "Resuming server that still thinks it is leader (ArangoError 1004 is expected)");
      suspended.forEach(arangod => {
        print(`${Date()} Resuming: ${arangod.name} ${arangod.pid}`);
        assertTrue(arangod.resume());
      });
      suspended = [];

      assertTrue(checkInSync(currentLead, servers));
    },

    // try to failback to the original leader
    testFailback: function() {
      if (currentLead === firstLeader) {
        return; // nevermind then
      }

      assertTrue(checkInSync(currentLead, servers));
      assertEqual(checkData(currentLead), 10000);

      print(Date() + "Suspending followers, except original leader");
      suspended = global.instanceManager.arangods.filter(arangod => arangod.instanceRole !== 'agent' &&
        arangod.endpoint !== firstLeader);
      suspended.forEach(arangod => {
        print(`${Date()} Suspending: ${arangod.name} ${arangod.pid}`);
        assertTrue(arangod.suspend());
      });

      // await failover and check that follower get in sync
      let oldLead = currentLead;
      currentLead = checkForFailover(currentLead);
      assertNotEqual(currentLead, oldLead);
      assertEqual(currentLead, firstLeader, "Did not fail to original leader");

      suspended.forEach(arangod => {
        print(`${Date()} Resuming: ${arangod.name} ${arangod.pid}`);
        assertTrue(arangod.resume());
      });
      suspended = [];

      assertTrue(checkInSync(currentLead, servers));
      assertEqual(checkData(currentLead), 10000);
    },

    // Try to cleanup everything that was created
    /*testCleanup: function () {

      let res = readAgencyValue("/arango/Plan/AsyncReplication/Leader");
      assertNotEqual(res, null);
      let uuid = res[0].arango.Plan.AsyncReplication.Leader;
      res = readAgencyValue("/arango/Supervision/Health");
      let lead = res[0].arango.Supervision.Health[uuid].Endpoint;

      connectToServer(lead);
      db._drop(cname);

      assertTrue(checkInSync(lead, servers));
    }*/

    // Regression test. This endpoint was broken due to added checks in v8-cluster.cpp,
    // which allowed certain calls only in cluster mode, but not in active failover.
    testClusterHealth: function () {
      console.warn({currentLead: getUrl(currentLead)});
      const res = request.get({
        url: getUrl(currentLead) + "/_admin/cluster/health",
        auth: {
          bearer: jwtRoot,
        },
        timeout: 30
      });
      console.warn(JSON.stringify(res));
      console.warn(res.json);
      expect(res).to.be.an.instanceof(request.Response);
      // expect(res).to.be.have.property('statusCode', 200);
      expect(res).to.have.property('json');
      expect(res.json).to.include({error: false, code: 200});
      expect(res.json).to.have.property('Health');
    },

  };
}

jsunity.run(ActiveFailoverSuite);

return jsunity.done();
