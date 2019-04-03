// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_GN_TOOLCHAIN_H_
#define TOOLS_GN_TOOLCHAIN_H_

#include <memory>

#include "base/logging.h"
#include "base/strings/string_piece.h"
#include "tools/gn/item.h"
#include "tools/gn/label_ptr.h"
#include "tools/gn/scope.h"
#include "tools/gn/source_file_type.h"
#include "tools/gn/substitution_type.h"
#include "tools/gn/tool.h"
#include "tools/gn/value.h"

// Holds information on a specific toolchain. This data is filled in when we
// encounter a toolchain definition.
//
// This class is an Item so it can participate in dependency management. In
// particular, when a target uses a toolchain, it should have a dependency on
// that toolchain's object so that we can be sure we loaded the toolchain
// before generating the build for that target.
//
// Note on threadsafety: The label of the toolchain never changes so can
// safely be accessed from any thread at any time (we do this when asking for
// the toolchain name). But the values in the toolchain do, so these can't
// be accessed until this Item is resolved.
class Toolchain : public Item {
 public:
  // The Settings of an Item is always the context in which the Item was
  // defined. For a toolchain this is confusing because this is NOT the
  // settings object that applies to the things in the toolchain.
  //
  // To get the Settings object corresponding to objects loaded in the context
  // of this toolchain (probably what you want instead), see
  // Loader::GetToolchainSettings(). Many toolchain objects may be created in a
  // given build, but only a few might be used, and the Loader is in charge of
  // this process.
  //
  // We also track the set of build files that may affect this target, please
  // refer to Scope for how this is determined.
  Toolchain(const Settings* settings,
            const Label& label,
            const std::set<SourceFile>& build_dependency_files = {});
  ~Toolchain() override;

  // Item overrides.
  Toolchain* AsToolchain() override;
  const Toolchain* AsToolchain() const override;

  // Returns null if the tool hasn't been defined.
  Tool* GetTool(Tool::ToolType type);
  const Tool* GetTool(Tool::ToolType type) const;

  // Set a tool. When all tools are configured, you should call
  // ToolchainSetupComplete().
  void SetTool(std::unique_ptr<Tool> t);

  // Does final setup on the toolchain once all tools are known.
  void ToolchainSetupComplete();

  // Targets that must be resolved before compiling any targets.
  const LabelTargetVector& deps() const { return deps_; }
  LabelTargetVector& deps() { return deps_; }

  // Specifies build argument overrides that will be set on the base scope. It
  // will be as if these arguments were passed in on the command line. This
  // allows a toolchain to override the OS type of the default toolchain or
  // pass in other settings.
  Scope::KeyValueMap& args() { return args_; }
  const Scope::KeyValueMap& args() const { return args_; }

  // Specifies whether public_configs and all_dependent_configs in this
  // toolchain propagate to targets in other toolchains.
  bool propagates_configs() const { return propagates_configs_; }
  void set_propagates_configs(bool propagates_configs) {
    propagates_configs_ = propagates_configs;
  }

  // Returns the tool for compiling the given source file type.
  const Tool* GetToolForSourceType(SourceFileType type);

  // Returns the tool that produces the final output for the given target type.
  // This isn't necessarily the tool you would expect. For copy target, this
  // will return the stamp tool instead since the final output of a copy
  // target is to stamp the set of copies done so there is one output.
  const Tool* GetToolForTargetFinalOutput(const Target* target) const;

  const SubstitutionBits& substitution_bits() const {
    DCHECK(setup_complete_);
    return substitution_bits_;
  }

 private:
  std::unique_ptr<Tool> tools_[Tool::TYPE_NUMTYPES];

  bool setup_complete_ = false;

  // Substitutions used by the tools in this toolchain.
  SubstitutionBits substitution_bits_;

  LabelTargetVector deps_;
  Scope::KeyValueMap args_;
  bool propagates_configs_ = false;
};

#endif  // TOOLS_GN_TOOLCHAIN_H_
