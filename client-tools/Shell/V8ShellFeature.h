////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2023 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ApplicationFeatures/ApplicationFeature.h"

#include <libplatform/libplatform.h>
#include <v8.h>

#include "Shell/ShellConsoleFeature.h"
#include "Shell/ShellFeature.h"

namespace arangodb {

class V8ClientConnection;

class V8ShellFeature final : public ArangoshFeature {
 public:
  static constexpr std::string_view name() noexcept { return "V8Shell"; }

  V8ShellFeature(Server& server, std::string const& name);

  void collectOptions(std::shared_ptr<options::ProgramOptions>) override;
  void validateOptions(
      std::shared_ptr<options::ProgramOptions> options) override;
  void start() override final;
  void unprepare() override final;
  void stop() override final;

  std::string const& startupDirectory() const { return _startupDirectory; }

 private:
  std::string _startupDirectory;
  std::string _nodeModulesDirectory;
  std::string _clientModule;
  std::string _copyDirectory;
  std::vector<std::string> _moduleDirectories;
  bool _currentModuleDirectory;
  bool _copyInstallation;
  bool _removeCopyInstallation;
  uint64_t _gcInterval;

 public:
  ErrorCode runShell(std::vector<std::string> const& positionals);
  bool runScript(std::vector<std::string> const& files,
                 std::vector<std::string> const&, bool,
                 std::vector<std::string> const& mainArgs, bool);
  bool runString(std::vector<std::string> const& files,
                 std::vector<std::string> const&);
  bool runUnitTests(std::vector<std::string> const& files,
                    std::vector<std::string> const& positionals,
                    std::string const& testFilter);

 private:
  void copyInstallationFiles();
  bool printHello(V8ClientConnection*);
  void initGlobals();
  void initMode(ShellFeature::RunMode, std::vector<std::string> const&);
  void loadModules(ShellFeature::RunMode);
  std::shared_ptr<V8ClientConnection> setup(v8::Local<v8::Context>& context,
                                            bool,
                                            std::vector<std::string> const&,
                                            bool* promptError = nullptr);

  std::string _name;
  v8::Isolate* _isolate;
  v8::Persistent<v8::Context> _context;
};

}  // namespace arangodb
